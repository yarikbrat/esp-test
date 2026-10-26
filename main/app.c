#include "app.h"

#include <string.h>

#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "kv.h"

static const char *TAG = "app";

app_t g_app;

extern const uint8_t page_start[] asm("_binary_index_html_start");
extern const uint8_t page_end[] asm("_binary_index_html_end");
extern const uint8_t page_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t page_gz_end[] asm("_binary_index_html_gz_end");

uint32_t app_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

const char *app_zero_key(int ch) { return ch ? "zero1" : "zero0"; }

static const char *origin_text(st_origin_t o)
{
    return o == ST_LOADED ? "loaded" : o == ST_CORRUPT ? "CORRUPT, defaults" : "missing, defaults";
}

void app_load_settings(void)
{
    uint8_t buf[64];
    size_t n = kv_get("net", buf, sizeof(buf));
    g_app.net_origin = st_decode_net(n ? buf : NULL, n, &g_app.net);
    // Налаштування, надіслані модулю, але ще не підтверджені на новій адресі, зберігаються після перезавантаження.
    n = kv_get("netp", buf, sizeof(buf));
    if (n && st_decode_net(buf, n, &g_app.net_pending) == ST_LOADED) {
        if (memcmp(&g_app.net_pending, &g_app.net, sizeof(netcfg_t)) == 0) {
            kv_erase("netp"); // живлення зникло після запису "net", але до стирання "netp"
        } else {
            g_app.net_apply = NET_APPLY_PENDING;
            ESP_LOGI(TAG, "net: change pending, waiting for a commit at the new address");
        }
    }
    n = kv_get("log", buf, sizeof(buf));
    g_app.log_origin = st_decode_log(n ? buf : NULL, n, &g_app.log_cfg);
    ESP_LOGI(TAG, "net: %s, log: %s (%s, %u s)", origin_text(g_app.net_origin), origin_text(g_app.log_origin),
             g_app.log_cfg.enabled ? "on" : "off", g_app.log_cfg.interval_s);
    for (int ch = 0; ch < STAND_BARO_N; ch++) {
        zero_cfg_t z;
        n = kv_get(app_zero_key(ch), buf, sizeof(buf));
        st_origin_t o = st_decode_zero(n ? buf : NULL, n, (uint8_t)ch, &z);
        stand_load_zero(&g_app.stand, ch, &z, o);
        ESP_LOGI(TAG, "zero%d: %s%s", ch, origin_text(o), z.set ? ", set" : "");
    }
    // Лічильник завантажень, використовується як id сесії в записах журналу.
    uint16_t boot = 0;
    if (kv_get("boot", buf, sizeof(buf)) == 2) {
        boot = (uint16_t)(buf[0] | (buf[1] << 8));
    }
    boot++;
    buf[0] = (uint8_t)boot;
    buf[1] = (uint8_t)(boot >> 8);
    kv_set("boot", buf, 2);
    g_app.session = boot;
}

void app_request_zero_persist(int ch)
{
    uint8_t c = (uint8_t)ch;
    if (xQueueSend(g_app.persist_q, &c, 0) != pdTRUE) {
        ESP_LOGE(TAG, "persist queue full, zero%d not saved", ch);
        app_lock();
        stand_zero_persisted(&g_app.stand, ch, false);
        app_unlock();
    }
}

void app_refresh_log_stats(void)
{
    app_lock();
    g_app.log_count = histlog_count(&g_app.log);
    g_app.log_capacity = g_app.log.capacity;
    g_app.log_recs_per_sector = g_app.log.recs_per_sector;
    g_app.log_sectors = g_app.log.n_sectors;
    g_app.log_write_errors = g_app.log.write_errors;
    g_app.log_corrupt = g_app.log.corrupt;
    g_app.log_last_error = g_app.log.last_error;
    app_unlock();
}

// --- Бекенд веб-API --------------------------------------------------------------------------

static void be_snapshot(void *ctx, api_snapshot_t *o)
{
    (void)ctx;
    memset(o, 0, sizeof(*o));
    app_lock();
    o->now_ms = app_now_ms();
    o->uptime_s = o->now_ms / 1000u;
    o->session = g_app.session;
    o->st = g_app.stand;
    o->log = g_app.log_cfg;
    o->log_origin = g_app.log_origin;
    o->log_persist = g_app.log_persist;
    o->log_clearing = g_app.log_clearing || g_app.log_clear_req;
    o->log_count = g_app.log_count;
    o->log_capacity = g_app.log_capacity;
    o->log_capacity_guaranteed = g_app.log_capacity - g_app.log_recs_per_sector;
    o->log_bytes = g_app.log_sectors * g_app.flash_ops.sector_size;
    o->log_write_errors = g_app.log_write_errors;
    o->log_corrupt = g_app.log_corrupt;
    o->log_last_error = g_app.log_last_error;
    o->net = g_app.net;
    o->net_origin = g_app.net_origin;
    o->net_apply = g_app.net_apply;
    o->net_pending = g_app.net_pending;
    o->net_apply_msg = g_app.net_apply_msg;
    o->http_port = HTTP_PORT;
    o->link_known = false; // LINK/LINK2 не під'єднані
    o->http = g_app.http;
    o->platform = "esp32";
    app_unlock();
}

static stand_zero_start_t be_zero_start(void *ctx, int ch)
{
    (void)ctx;
    app_lock();
    stand_zero_start_t r = stand_zero_start(&g_app.stand, ch, app_now_ms());
    app_unlock();
    return r;
}

static bool be_zero_reset(void *ctx, int ch)
{
    (void)ctx;
    app_lock();
    bool ok = stand_zero_reset(&g_app.stand, ch);
    app_unlock();
    if (ok) {
        app_request_zero_persist(ch);
    }
    return ok;
}

static persist_t be_log_config(void *ctx, const log_cfg_t *cfg)
{
    (void)ctx;
    uint8_t buf[ST_LOG_BLOB_SIZE];
    size_t n = st_encode_log(cfg, buf, sizeof(buf));
    persist_t p = n && kv_set("log", buf, n) ? PERSIST_OK : PERSIST_ERROR;
    app_lock();
    g_app.log_cfg = *cfg;
    g_app.log_persist = p;
    g_app.log_cfg_changed = true;
    app_unlock();
    xTaskNotifyGive(g_app.logger_task);
    return p;
}

static bool be_log_clear(void *ctx)
{
    (void)ctx;
    app_lock();
    bool ok = !g_app.log_clear_req && !g_app.log_clearing;
    if (ok) {
        g_app.log_clear_req = true;
    }
    app_unlock();
    if (ok) {
        xTaskNotifyGive(g_app.logger_task);
    }
    return ok;
}

static int be_log_read(void *ctx, uint32_t before, hl_record_t *out, size_t max, uint32_t *next_before,
                       uint32_t *oldest)
{
    (void)ctx;
    *next_before = HL_SEQ_EMPTY;
    app_lock();
    bool busy = g_app.log_clearing || g_app.log_clear_req;
    app_unlock();
    // Стирання сектора тримає log_lock десятки ms; не затримувати міст довше за це.
    if (busy || xSemaphoreTake(g_app.log_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return -1;
    }
    size_t n = histlog_read_recent(&g_app.log, before, out, max, next_before);
    *oldest = g_app.log.oldest_seq;
    xSemaphoreGive(g_app.log_lock);
    return (int)n;
}

static void be_net_prepare(void *ctx, const netcfg_t *cfg)
{
    (void)ctx;
    char ip[16];
    netcfg_format_ipv4(cfg->ip, ip, sizeof(ip));
    uint8_t buf[ST_NET_BLOB_SIZE];
    size_t n = st_encode_net(cfg, buf, sizeof(buf));
    bool saved = n && kv_set("netp", buf, n);
    app_lock();
    g_app.net_pending = *cfg;
    g_app.net_apply = NET_APPLY_PENDING;
    g_app.net_apply_msg = NULL;
    app_unlock();
    ESP_LOGI(TAG, "net settings %s accepted, pending until committed at that address%s", ip,
             saved ? "" : " (not saved, lost on reboot)");
}

// Також прибирає зміну, що лишилася після невдалого збереження, щоб вона не повернулася після перезавантаження.
static bool be_net_cancel(void *ctx)
{
    (void)ctx;
    app_lock();
    bool p = g_app.net_apply == NET_APPLY_PENDING || g_app.net_apply == NET_APPLY_FAILED;
    if (p) {
        g_app.net_apply = NET_APPLY_IDLE;
        g_app.net_apply_msg = NULL;
    }
    app_unlock();
    kv_erase("netp");
    if (p) {
        ESP_LOGI(TAG, "pending net change cancelled");
    }
    return p;
}

static bool be_net_pending(void *ctx, netcfg_t *out)
{
    (void)ctx;
    app_lock();
    bool p = g_app.net_apply == NET_APPLY_PENDING;
    if (p) {
        *out = g_app.net_pending;
    }
    app_unlock();
    return p;
}

api_backend_t app_backend(void)
{
    return (api_backend_t){
        .snapshot = be_snapshot,
        .zero_start = be_zero_start,
        .zero_reset = be_zero_reset,
        .log_config = be_log_config,
        .log_clear = be_log_clear,
        .log_read = be_log_read,
        .net_prepare = be_net_prepare,
        .net_pending = be_net_pending,
        .net_cancel = be_net_cancel,
        .page = page_start,
        .page_len = (size_t)(page_end - page_start),
        .page_gz = page_gz_start,
        .page_gz_len = (size_t)(page_gz_end - page_gz_start),
        .http_port = HTTP_PORT,
    };
}

// --- Мережеві налаштування ---------------------------------------------------------------------------

// Адреса зберігається в модулі UART TO ETH; переносить модуль сама сторінка (docs/uart-to-eth-b.md). Це виконується
// на POST /api/net/commit за новою адресою (або після BOOT) і зберігає налаштування як активні.
void app_net_apply(const netcfg_t *cfg)
{
    char ip[16];
    netcfg_format_ipv4(cfg->ip, ip, sizeof(ip));
    uint8_t buf[ST_NET_BLOB_SIZE];
    size_t n = st_encode_net(cfg, buf, sizeof(buf));
    bool ok = n && kv_set("net", buf, n);
    if (ok) {
        kv_erase("netp"); // лишити запис в очікуванні, якщо "net" не вдалося зберегти
    }
    app_lock();
    g_app.net = *cfg; // модуль уже відповідає за цією адресою, збережено чи ні
    if (ok) {
        g_app.net_origin = ST_LOADED;
        g_app.net_apply = NET_APPLY_OK;
        g_app.net_apply_msg = NULL;
    } else {
        g_app.net_apply = NET_APPLY_FAILED;
        g_app.net_apply_msg = "Адресу застосовано, але не збережено в пам'яті ESP32";
    }
    app_unlock();
    ESP_LOGI(TAG, "net settings %s saved: %s", ip, ok ? "ok" : "FAILED");
}

void app_net_reset_default(void)
{
    netcfg_t def;
    netcfg_default(&def);
    app_net_apply(&def);
}
