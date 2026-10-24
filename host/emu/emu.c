// Bench emulator: runs the real core (parser, api, stand, histlog, settings codec) on Linux.
// Threads mirror the firmware tasks; the TCP side mimics UART TO ETH (B) in transparent mode:
// all client connections are merged into one byte stream and the reply goes to every client.
#define _GNU_SOURCE
#include "api.h"
#include "flash_emu.h"
#include "histlog.h"
#include "http_parser.h"
#include "settings_codec.h"
#include "stand.h"

#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS   30 // same limit as the module's TCP server
#define LOG_SECTORS   64
#define SECTOR_SIZE   4096

typedef struct {
    int port;
    int baud; // 0 = unthrottled
    bool broadcast;
    const char *data_dir;
    const char *page_path;
    int log_sectors;
} opts_t;

static opts_t g_opt = {.port = 8080, .baud = 115200, .broadcast = true, .data_dir = "host/emu_data",
                       .page_path = "web/index.html", .log_sectors = LOG_SECTORS};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_persist_lock = PTHREAD_MUTEX_INITIALIZER; // orders zero writes; taken before g_lock
static pthread_cond_t g_log_cond = PTHREAD_COND_INITIALIZER;

// Everything below is guarded by g_lock.
static stand_t g_stand;
static histlog_t g_log;
static log_cfg_t g_log_cfg;
static st_origin_t g_log_origin;
static persist_t g_log_persist;
static bool g_log_clear_req, g_log_clearing;
static netcfg_t g_net;
static st_origin_t g_net_origin;
static net_apply_t g_net_apply;
static netcfg_t g_net_pending;
static const char *g_net_apply_msg;
static uint16_t g_session;
static http_stats_t g_http_stats;
static int g_nclients;

// Simulation knobs, also under g_lock.
static bool g_fail[3]; // baro0, baro1, sonar
static float g_lift_m;
static float g_noise_pa = 2.0f;
static float g_distance_cm = 50.0f;

static flash_emu_t g_flash;
static hl_flash_t g_flash_ops;
static uint64_t g_boot_ms;

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint32_t now_ms(void) { return (uint32_t)(mono_ms() - g_boot_ms); }

static void sleep_ms(unsigned ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

// --- NVS stand-in: one file per key -----------------------------------------------------------

static void kv_path(const char *key, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s/nvs_%s.bin", g_opt.data_dir, key);
}

static size_t kv_get(const char *key, uint8_t *buf, size_t cap)
{
    char path[256];
    kv_path(key, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static bool kv_set(const char *key, const uint8_t *buf, size_t len)
{
    char path[256], tmp[260];
    kv_path(key, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(buf, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    return ok && rename(tmp, path) == 0; // atomic like an NVS commit
}

static const char *zero_key(int ch) { return ch ? "zero1" : "zero0"; }

// Writes the channel's current RAM zero. Reading it under g_persist_lock means a late writer always
// stores the newest value, so a reset can never be overwritten by an older calibration result.
static bool persist_zero(int ch)
{
    pthread_mutex_lock(&g_persist_lock);
    pthread_mutex_lock(&g_lock);
    zero_cfg_t z = g_stand.baro[ch].zero;
    pthread_mutex_unlock(&g_lock);
    uint8_t buf[ST_ZERO_BLOB_SIZE];
    size_t n = st_encode_zero(&z, (uint8_t)ch, buf, sizeof(buf));
    bool ok = n && kv_set(zero_key(ch), buf, n);
    pthread_mutex_lock(&g_lock);
    stand_zero_persisted(&g_stand, ch, ok);
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&g_persist_lock);
    return ok;
}

// --- Sensor task --------------------------------------------------------------------------------

static float gauss(void)
{
    double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = (rand() + 1.0) / (RAND_MAX + 2.0);
    return (float)(sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2));
}

static void *sensor_task(void *arg)
{
    (void)arg;
    uint32_t next_sonar = 0;
    for (;;) {
        uint32_t t = now_ms();
        pthread_mutex_lock(&g_lock);
        stand_event_t ev[STAND_BARO_N];
        float p0[STAND_BARO_N];
        for (int ch = 0; ch < STAND_BARO_N; ch++) {
            // ~12 Pa per metre near sea level; slow common drift imitates weather.
            float p = 101325.0f + (ch ? 15.0f : 0.0f) - 12.0f * g_lift_m +
                      3.0f * sinf((float)t / 600000.0f) + g_noise_pa * gauss();
            ev[ch] = g_fail[ch] ? stand_baro_sample(&g_stand, ch, SS_NOT_FOUND, NAN, t)
                                : stand_baro_sample(&g_stand, ch, SS_OK, p, t);
            p0[ch] = g_stand.baro[ch].zero.p0_pa;
        }
        if ((int32_t)(t - next_sonar) >= 0) {
            next_sonar = t + 60;
            float cm = g_distance_cm + 0.3f * gauss();
            if (rand() % 50 == 0) {
                cm += 40.0f; // occasional echo spike, removed by the median
            }
            if (g_fail[2]) {
                stand_sonar_sample(&g_stand, SS_NO_ECHO, NAN, t);
            } else {
                stand_sonar_sample(&g_stand, SS_OK, cm, t);
            }
        }
        pthread_mutex_unlock(&g_lock);

        // Persist outside the lock, like the firmware's storage task.
        for (int ch = 0; ch < STAND_BARO_N; ch++) {
            if (ev[ch] == STAND_EV_ZERO_DONE) {
                bool ok = persist_zero(ch);
                printf("[baro%d] zero set p0=%.2f Pa, persist %s\n", ch, p0[ch], ok ? "ok" : "FAILED");
            } else if (ev[ch] == STAND_EV_ZERO_FAILED) {
                printf("[baro%d] zero failed, previous kept\n", ch);
            }
        }
        sleep_ms(50); // 20 Hz
    }
    return NULL;
}

// --- Logger task --------------------------------------------------------------------------------

static void *logger_task(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_lock);
    uint64_t next = mono_ms() + (uint64_t)g_log_cfg.interval_s * 1000u;
    for (;;) {
        if (g_log_clear_req) {
            g_log_clear_req = false;
            g_log_clearing = true;
            pthread_mutex_unlock(&g_lock);
            sleep_ms(500); // the real erase of 64 sectors takes seconds
            pthread_mutex_lock(&g_lock);
            int err = histlog_clear(&g_log);
            g_log_clearing = false;
            if (err) {
                printf("[log] clear FAILED (%d), rescanning\n", err);
                uint32_t errors = g_log.write_errors; // open() resets counters; keep the history
                histlog_open(&g_log, &g_flash_ops, g_log.n_sectors); // true state after a partial erase
                g_log.write_errors = errors;
                g_log.last_error = err;
            } else {
                printf("[log] cleared\n");
            }
            continue;
        }
        uint64_t t = mono_ms();
        if (t >= next) {
            if (g_log_cfg.enabled) {
                hl_record_t rec;
                uint32_t nt = now_ms();
                stand_fill_record(&g_stand, nt, g_session, nt / 1000u, &rec);
                if (histlog_append(&g_log, &rec) != 0) {
                    printf("[log] write error %d\n", g_log.last_error);
                }
            }
            // Schedule from the previous deadline so intervals do not drift.
            next += (uint64_t)g_log_cfg.interval_s * 1000u;
            if (next <= t) {
                next = t + (uint64_t)g_log_cfg.interval_s * 1000u;
            }
            continue;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t wait = next - t;
        ts.tv_sec += (time_t)(wait / 1000u);
        ts.tv_nsec += (long)(wait % 1000u) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        uint16_t iv = g_log_cfg.interval_s;
        pthread_cond_timedwait(&g_log_cond, &g_lock, &ts);
        if (g_log_cfg.interval_s != iv) {
            next = mono_ms() + (uint64_t)g_log_cfg.interval_s * 1000u; // new interval starts now
        }
    }
    return NULL;
}

// --- API backend --------------------------------------------------------------------------------

static void be_snapshot(void *ctx, api_snapshot_t *o)
{
    (void)ctx;
    memset(o, 0, sizeof(*o));
    pthread_mutex_lock(&g_lock);
    o->now_ms = now_ms();
    o->uptime_s = o->now_ms / 1000u;
    o->session = g_session;
    o->st = g_stand;
    o->log = g_log_cfg;
    o->log_origin = g_log_origin;
    o->log_persist = g_log_persist;
    o->log_clearing = g_log_clearing || g_log_clear_req;
    o->log_count = histlog_count(&g_log);
    o->log_capacity = g_log.capacity;
    o->log_capacity_guaranteed = g_log.capacity - g_log.recs_per_sector;
    o->log_bytes = g_log.n_sectors * SECTOR_SIZE;
    o->log_write_errors = g_log.write_errors;
    o->log_corrupt = g_log.corrupt;
    o->log_last_error = g_log.last_error;
    o->net = g_net;
    o->net_origin = g_net_origin;
    o->net_apply = g_net_apply;
    o->net_pending = g_net_pending;
    o->net_apply_msg = g_net_apply_msg;
    o->http_port = (uint16_t)g_opt.port;
    o->link_known = true;
    o->link_cable = true;
    o->link_tcp = g_nclients > 0;
    o->http = g_http_stats;
    o->platform = "emulator";
    pthread_mutex_unlock(&g_lock);
}

static stand_zero_start_t be_zero_start(void *ctx, int ch)
{
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    stand_zero_start_t r = stand_zero_start(&g_stand, ch, now_ms());
    pthread_mutex_unlock(&g_lock);
    return r;
}

static bool be_zero_reset(void *ctx, int ch)
{
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    bool ok = stand_zero_reset(&g_stand, ch);
    pthread_mutex_unlock(&g_lock);
    if (ok) {
        persist_zero(ch);
    }
    return ok;
}

static persist_t be_log_config(void *ctx, const log_cfg_t *cfg)
{
    (void)ctx;
    uint8_t buf[ST_LOG_BLOB_SIZE];
    size_t n = st_encode_log(cfg, buf, sizeof(buf));
    persist_t p = n && kv_set("log", buf, n) ? PERSIST_OK : PERSIST_ERROR;
    pthread_mutex_lock(&g_lock);
    g_log_cfg = *cfg;
    g_log_persist = p;
    pthread_cond_signal(&g_log_cond);
    pthread_mutex_unlock(&g_lock);
    return p;
}

static bool be_log_clear(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    bool ok = !g_log_clear_req && !g_log_clearing;
    if (ok) {
        g_log_clear_req = true;
        pthread_cond_signal(&g_log_cond);
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

static size_t be_log_read(void *ctx, uint32_t before, hl_record_t *out, size_t max, uint32_t *next_before)
{
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    *next_before = HL_SEQ_EMPTY;
    size_t n = g_log_clearing ? 0 : histlog_read_recent(&g_log, before, out, max, next_before);
    pthread_mutex_unlock(&g_lock);
    return n;
}

// The emulator listens on 127.0.0.1 only, so a request never arrives at the pending address and the
// change stays pending; the page then reports that the new address is unreachable.
static void be_net_prepare(void *ctx, const netcfg_t *cfg)
{
    (void)ctx;
    char ip[16];
    netcfg_format_ipv4(cfg->ip, ip, sizeof(ip));
    printf("[net] %s accepted, pending until a request arrives there\n", ip);
    pthread_mutex_lock(&g_lock);
    g_net_pending = *cfg;
    g_net_apply = NET_APPLY_PENDING;
    g_net_apply_msg = NULL;
    pthread_mutex_unlock(&g_lock);
}

static bool be_net_pending(void *ctx, netcfg_t *out)
{
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    bool p = g_net_apply == NET_APPLY_PENDING;
    if (p) {
        *out = g_net_pending;
    }
    pthread_mutex_unlock(&g_lock);
    return p;
}

static void net_apply(const netcfg_t *cfg)
{
    char ip[16];
    netcfg_format_ipv4(cfg->ip, ip, sizeof(ip));
    printf("[net] request arrived at %s, saving\n", ip);
    uint8_t buf[ST_NET_BLOB_SIZE];
    size_t n = st_encode_net(cfg, buf, sizeof(buf));
    bool ok = n && kv_set("net", buf, n);
    pthread_mutex_lock(&g_lock);
    if (ok) {
        g_net = *cfg;
        g_net_origin = ST_LOADED;
        g_net_apply = NET_APPLY_OK;
        g_net_apply_msg = NULL;
    } else {
        g_net_apply = NET_APPLY_FAILED;
        g_net_apply_msg = "Не вдалося зберегти налаштування мережі";
    }
    pthread_mutex_unlock(&g_lock);
}

// --- Bridge emulation ---------------------------------------------------------------------------

static http_parser_t g_parser;
static int g_clients[MAX_CLIENTS];
static int g_reply_to = -1; // direct mode: client whose bytes completed the request

static void drop_client(int i)
{
    close(g_clients[i]);
    g_clients[i] = -1;
    pthread_mutex_lock(&g_lock);
    bool none_left = --g_nclients == 0;
    pthread_mutex_unlock(&g_lock);
    // Like the module's LINK pin: only "no TCP connection at all" is visible, so reset then.
    if (none_left) {
        http_parser_reset(&g_parser);
    }
}

// UART TX: throttled to the configured baud, then copied to every TCP client.
static int bridge_write(void *ctx, const void *data, size_t len)
{
    (void)ctx;
    const uint8_t *p = data;
    size_t chunk = g_opt.baud ? (size_t)g_opt.baud / 10 / 50 : len; // 20 ms worth of bytes
    if (chunk == 0) {
        chunk = 1;
    }
    while (len > 0) {
        size_t n = len < chunk ? len : chunk;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i] < 0 || (!g_opt.broadcast && i != g_reply_to)) {
                continue;
            }
            if (send(g_clients[i], p, n, MSG_NOSIGNAL) < 0) {
                drop_client(i);
            }
        }
        if (g_opt.baud) {
            sleep_ms((unsigned)(n * 10u * 1000u / (unsigned)g_opt.baud));
        }
        p += n;
        len -= n;
    }
    return 0;
}

static uint8_t g_page[64 * 1024];
static size_t g_page_len;

// Re-read on every page request so the UI can be edited live; keep the old copy if the file
// is missing or would not fit, instead of serving an empty or truncated page.
static bool load_page(void)
{
    static uint8_t tmp[sizeof(g_page) + 1];
    FILE *f = fopen(g_opt.page_path, "rb");
    if (!f) {
        fprintf(stderr, "[web] cannot open %s\n", g_opt.page_path);
        return false;
    }
    size_t n = fread(tmp, 1, sizeof(tmp), f);
    fclose(f);
    if (n == 0 || n > sizeof(g_page)) {
        fprintf(stderr, "[web] %s is empty or larger than %zu bytes\n", g_opt.page_path, sizeof(g_page));
        return false;
    }
    memcpy(g_page, tmp, n);
    g_page_len = n;
    return true;
}

static void *stdin_task(void *arg)
{
    (void)arg;
    char line[128];
    while (fgets(line, sizeof(line), stdin)) {
        char cmd[32] = {0}, val[32] = {0};
        if (sscanf(line, "%31s %31s", cmd, val) < 1) {
            continue;
        }
        pthread_mutex_lock(&g_lock);
        int idx = strcmp(cmd, "baro0") == 0 ? 0 : strcmp(cmd, "baro1") == 0 ? 1 : strcmp(cmd, "sonar") == 0 ? 2 : -1;
        if (idx >= 0) {
            g_fail[idx] = strcmp(val, "off") == 0;
            printf("%s %s\n", cmd, g_fail[idx] ? "off" : "on");
        } else if (strcmp(cmd, "lift") == 0) {
            g_lift_m = strtof(val, NULL);
            printf("lift %.2f m\n", g_lift_m);
        } else if (strcmp(cmd, "dist") == 0) {
            g_distance_cm = strtof(val, NULL);
            printf("distance %.1f cm\n", g_distance_cm);
        } else if (strcmp(cmd, "noise") == 0) {
            g_noise_pa = strtof(val, NULL);
            printf("noise %.2f Pa\n", g_noise_pa);
        } else {
            printf("commands: baro0|baro1|sonar on|off, lift <m>, dist <cm>, noise <Pa>\n");
        }
        pthread_mutex_unlock(&g_lock);
        fflush(stdout);
    }
    return NULL;
}

static void load_settings(void)
{
    uint8_t buf[64];
    size_t n = kv_get("net", buf, sizeof(buf));
    g_net_origin = st_decode_net(n ? buf : NULL, n, &g_net);
    n = kv_get("log", buf, sizeof(buf));
    g_log_origin = st_decode_log(n ? buf : NULL, n, &g_log_cfg);
    for (int ch = 0; ch < STAND_BARO_N; ch++) {
        zero_cfg_t z;
        n = kv_get(zero_key(ch), buf, sizeof(buf));
        st_origin_t o = st_decode_zero(n ? buf : NULL, n, (uint8_t)ch, &z);
        stand_load_zero(&g_stand, ch, &z, o);
        printf("[boot] zero%d: %s%s\n", ch, o == ST_LOADED ? "loaded" : o == ST_CORRUPT ? "CORRUPT, defaults" : "missing",
               z.set ? " (set)" : "");
    }
    // Boot counter for the session id; a raw u16 is enough here.
    uint16_t boot = 0;
    n = kv_get("boot", buf, sizeof(buf));
    if (n == 2) {
        boot = (uint16_t)(buf[0] | (buf[1] << 8));
    }
    boot++;
    buf[0] = (uint8_t)boot;
    buf[1] = (uint8_t)(boot >> 8);
    kv_set("boot", buf, 2);
    g_session = boot;
}

static void usage(void)
{
    fprintf(stderr, "usage: emu [--port N] [--baud N|0] [--direct] [--data DIR] [--page FILE] [--log-sectors N]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            g_opt.port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--baud") && i + 1 < argc) {
            g_opt.baud = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--direct")) {
            g_opt.broadcast = false;
        } else if (!strcmp(argv[i], "--data") && i + 1 < argc) {
            g_opt.data_dir = argv[++i];
        } else if (!strcmp(argv[i], "--page") && i + 1 < argc) {
            g_opt.page_path = argv[++i];
        } else if (!strcmp(argv[i], "--log-sectors") && i + 1 < argc) {
            g_opt.log_sectors = atoi(argv[++i]);
        } else {
            usage();
        }
    }
    if (g_opt.log_sectors < 2 || g_opt.log_sectors > LOG_SECTORS) {
        usage();
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    mkdir(g_opt.data_dir, 0755);
    srand((unsigned)time(NULL));
    g_boot_ms = mono_ms();

    stand_init(&g_stand);
    load_settings();
    char flash_path[256];
    snprintf(flash_path, sizeof(flash_path), "%s/histlog.bin", g_opt.data_dir);
    flash_emu_init(&g_flash, LOG_SECTORS * SECTOR_SIZE, SECTOR_SIZE, flash_path);
    g_flash_ops = flash_emu_ops(&g_flash);
    if (histlog_open(&g_log, &g_flash_ops, (uint32_t)g_opt.log_sectors) != 0) {
        fprintf(stderr, "histlog_open failed\n");
        return 1;
    }
    printf("[boot] session %u, log %u/%u records (corrupt %u), logging %s every %u s\n", g_session,
           histlog_count(&g_log), g_log.capacity, g_log.corrupt, g_log_cfg.enabled ? "on" : "off",
           g_log_cfg.interval_s);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)g_opt.port),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(ls, 8) < 0) {
        perror("bind/listen");
        return 1;
    }
    printf("[net] http://127.0.0.1:%d/  mode=%s baud=%d\n", g_opt.port, g_opt.broadcast ? "broadcast" : "direct",
           g_opt.baud);

    pthread_t th;
    pthread_create(&th, NULL, sensor_task, NULL);
    pthread_create(&th, NULL, logger_task, NULL);
    pthread_create(&th, NULL, stdin_task, NULL);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        g_clients[i] = -1;
    }
    http_parser_init(&g_parser);
    static char scratch[8192];
    api_backend_t be = {
        .snapshot = be_snapshot, .zero_start = be_zero_start, .zero_reset = be_zero_reset,
        .log_config = be_log_config, .log_clear = be_log_clear, .log_read = be_log_read,
        .net_prepare = be_net_prepare, .net_pending = be_net_pending, .http_port = (uint16_t)g_opt.port,
    };
    api_out_t out = {.write = bridge_write};
    if (!load_page()) {
        return 1;
    }
    be.page = g_page;

    for (;;) {
        struct pollfd pfd[MAX_CLIENTS + 1];
        int map[MAX_CLIENTS + 1];
        int n = 0;
        pfd[n].fd = ls;
        pfd[n].events = POLLIN;
        map[n++] = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i] >= 0) {
                pfd[n].fd = g_clients[i];
                pfd[n].events = POLLIN;
                map[n++] = i;
            }
        }
        poll(pfd, (nfds_t)n, 100);

        if (pfd[0].revents & POLLIN) {
            int c = accept(ls, NULL, NULL);
            int slot = -1;
            for (int i = 0; i < MAX_CLIENTS && slot < 0; i++) {
                if (g_clients[i] < 0) {
                    slot = i;
                }
            }
            if (c >= 0 && slot >= 0) {
                g_clients[slot] = c;
                pthread_mutex_lock(&g_lock);
                g_nclients++;
                pthread_mutex_unlock(&g_lock);
            } else if (c >= 0) {
                close(c);
            }
        }
        for (int k = 1; k < n; k++) {
            int i = map[k];
            if (!(pfd[k].revents & (POLLIN | POLLHUP | POLLERR)) || g_clients[i] < 0) {
                continue;
            }
            uint8_t buf[512];
            ssize_t got = recv(g_clients[i], buf, sizeof(buf), 0);
            if (got <= 0) {
                drop_client(i);
                continue;
            }
            // Merged stream: the parser cannot tell which client sent these bytes.
            size_t off = 0;
            while (off < (size_t)got) {
                http_event_t ev;
                off += http_parser_feed(&g_parser, buf + off, (size_t)got - off, now_ms(), &ev);
                g_reply_to = i;
                if (ev == HTTP_EV_REQUEST) {
                    if (!strcmp(g_parser.req.path, "/") || !strcmp(g_parser.req.path, "/index.html")) {
                        load_page();
                    }
                    be.page_len = g_page_len;
                    api_after_t after = api_handle(&be, &g_parser.req, &out, scratch, sizeof(scratch));
                    if (after.net_apply) {
                        net_apply(&after.net);
                    }
                } else if (ev == HTTP_EV_ERROR) {
                    printf("[http] error %d\n", g_parser.error_status);
                    api_send_error(&out, g_parser.error_status, g_parser.error_head, scratch, sizeof(scratch));
                }
            }
        }
        if (http_parser_tick(&g_parser, now_ms())) {
            printf("[http] incomplete request dropped (timeout)\n");
        }
        pthread_mutex_lock(&g_lock);
        g_http_stats = g_parser.stats;
        pthread_mutex_unlock(&g_lock);
    }
}
