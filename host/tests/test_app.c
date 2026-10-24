#include "api.h"
#include "check.h"
#include "jsonw.h"
#include "stand.h"

#include <stdlib.h>
#include <string.h>

static void feed_baro(stand_t *s, int ch, float p, uint32_t from, uint32_t to, stand_event_t *last)
{
    for (uint32_t t = from; t <= to; t += 50) {
        stand_event_t ev = stand_baro_sample(s, ch, SS_OK, p, t);
        if (ev != STAND_EV_NONE && last) {
            *last = ev;
        }
    }
}

void test_stand(void)
{
    stand_t s;
    stand_init(&s);
    float h;

    // Sonar: median of 5 rejects a single spike; stale after 500 ms; window resets after outage.
    float seq[] = {50.0f, 50.4f, 300.0f, 49.8f, 50.2f};
    for (int i = 0; i < 5; i++) {
        stand_sonar_sample(&s, SS_OK, seq[i], (uint32_t)(i * 60));
    }
    CHECK_NEAR(s.sonar.value, 50.2, 1e-4);
    CHECK(stand_chan_fresh(&s.sonar, STAND_SONAR_STALE_MS, 740));
    stand_sonar_sample(&s, SS_NO_ECHO, NAN, 800);
    CHECK_EQ_INT(s.sonar.status, SS_NO_ECHO);
    CHECK(!stand_chan_fresh(&s.sonar, STAND_SONAR_STALE_MS, 800));
    CHECK_NEAR(s.sonar.value, 50.2, 1e-4); // last valid kept for the "stale" display
    stand_sonar_sample(&s, SS_OK, 20.0f, 900);
    CHECK_NEAR(s.sonar.value, 20.0, 1e-4); // old window dropped
    stand_sonar_sample(&s, SS_OK, 21.0f, 960);
    CHECK_NEAR(s.sonar.value, 20.5, 1e-4);

    // Zero needs fresh data.
    CHECK_EQ_INT(stand_zero_start(&s, 0, 0), STAND_ZERO_NO_DATA);
    CHECK_EQ_INT(s.baro[0].cal_view, CAL_FAILED);
    CHECK_EQ_INT(s.baro[0].cal_err, CAL_ERR_NO_DATA);
    CHECK(!stand_baro_altitude(&s.baro[0], 0, &h));

    feed_baro(&s, 0, 100000.0f, 0, 500, NULL);
    feed_baro(&s, 1, 100500.0f, 0, 500, NULL);
    CHECK(!stand_baro_altitude(&s.baro[0], 500, &h)); // fresh but no zero

    CHECK_EQ_INT(stand_zero_start(&s, 0, 500), STAND_ZERO_STARTED);
    CHECK_EQ_INT(stand_zero_start(&s, 0, 520), STAND_ZERO_BUSY);
    CHECK(!stand_zero_reset(&s, 0)); // no reset during calibration
    stand_event_t ev = STAND_EV_NONE;
    feed_baro(&s, 0, 100000.0f, 550, 1600, &ev);
    feed_baro(&s, 1, 100500.0f, 550, 1600, NULL);
    CHECK_EQ_INT(ev, STAND_EV_ZERO_DONE);
    CHECK(s.baro[0].zero.set);
    CHECK_EQ_INT(s.baro[0].origin, ZO_NEW);
    CHECK_EQ_INT(s.baro[0].persist, PERSIST_PENDING);
    CHECK(!s.baro[1].zero.set); // channels independent
    CHECK(stand_baro_altitude(&s.baro[0], 1600, &h));
    CHECK_NEAR(h, 0.0, 0.01);
    stand_zero_persisted(&s, 0, true);
    CHECK_EQ_INT(s.baro[0].persist, PERSIST_OK);

    // Lift: lower pressure -> positive altitude, reached once the 1 s average window has refilled.
    feed_baro(&s, 0, 99970.0f, 1650, 1700, NULL);
    CHECK(stand_baro_altitude(&s.baro[0], 1700, &h));
    CHECK(h > 0.1f && h < 0.4f); // 2 of 20 samples are new
    feed_baro(&s, 0, 99970.0f, 1750, 2650, NULL);
    CHECK(stand_baro_altitude(&s.baro[0], 2650, &h));
    CHECK(h > 2.0f && h < 3.0f);
    CHECK_NEAR(s.baro[0].c.value, 99970.0, 0.01);

    // Failed calibration keeps the previous p0: sensor drops out during the run.
    float p0_before = s.baro[0].zero.p0_pa;
    CHECK_EQ_INT(stand_zero_start(&s, 0, 2650), STAND_ZERO_STARTED);
    ev = STAND_EV_NONE;
    for (uint32_t t = 2700; t <= 6000; t += 50) {
        stand_event_t e = stand_baro_sample(&s, 0, SS_COMM_ERROR, NAN, t);
        if (e != STAND_EV_NONE) {
            ev = e;
        }
    }
    CHECK_EQ_INT(ev, STAND_EV_ZERO_FAILED);
    CHECK_EQ_INT(s.baro[0].cal_err, CAL_ERR_TIMEOUT);
    CHECK(s.baro[0].zero.set && s.baro[0].zero.p0_pa == p0_before);
    CHECK(!stand_baro_altitude(&s.baro[0], 6000, &h)); // no fresh data -> no number

    // Calibration timeout when the driver stops calling sample() entirely.
    feed_baro(&s, 1, 100500.0f, 5000, 5100, NULL);
    CHECK_EQ_INT(stand_zero_start(&s, 1, 5100), STAND_ZERO_STARTED);
    CHECK_EQ_INT(stand_baro_poll(&s, 1, 8000), STAND_EV_NONE);
    CHECK_EQ_INT(stand_baro_poll(&s, 1, 8101), STAND_EV_ZERO_FAILED);
    CHECK(!s.baro[1].zero.set);

    // Log record: both channels from one state, failed channel flagged, never 0 for invalid.
    feed_baro(&s, 1, 100500.0f, 8200, 8300, NULL);
    hl_record_t rec;
    stand_fill_record(&s, 8300, 3, 77, &rec);
    CHECK_EQ_INT(rec.session, 3);
    CHECK_EQ_INT(rec.t_s, 77);
    CHECK(!(rec.flags[0] & HL_F_VALID) && (rec.flags[0] & HL_F_ZERO));
    CHECK_EQ_INT(HL_F_STATUS(rec.flags[0]), SS_COMM_ERROR);
    CHECK(isnan(rec.p_pa[0]) && isnan(rec.h_m[0]));
    CHECK((rec.flags[1] & HL_F_VALID) && !(rec.flags[1] & HL_F_ZERO));
    CHECK_NEAR(rec.p_pa[1], 100500.0, 0.01);
    CHECK(isnan(rec.h_m[1])); // altitude undefined before zero

    // A channel that silently stopped (last status OK) is logged as stale, not as OK.
    stand_fill_record(&s, 8300 + STAND_BARO_STALE_MS + 1, 3, 78, &rec);
    CHECK(!(rec.flags[1] & HL_F_VALID));
    CHECK_EQ_INT(HL_F_STATUS(rec.flags[1]), SS_STALE);

    // Reset + restore.
    CHECK(stand_zero_reset(&s, 0));
    CHECK(!s.baro[0].zero.set);
    CHECK_EQ_INT(s.baro[0].origin, ZO_NONE);
    zero_cfg_t z = {.set = true, .p0_pa = 100100.0f};
    stand_load_zero(&s, 1, &z, ST_LOADED);
    CHECK_EQ_INT(s.baro[1].origin, ZO_RESTORED);
}

// Pressure is the mean of the last STAND_BARO_AVG_N valid samples; an outage restarts the window.
void test_baro_average(void)
{
    stand_t s;
    stand_init(&s);
    uint32_t t = 0;
    for (int i = 0; i < STAND_BARO_AVG_N; i++, t += 50) {
        stand_baro_sample(&s, 0, SS_OK, (i % 2) ? 100002.0f : 99998.0f, t); // +-2 Pa noise
    }
    CHECK_NEAR(s.baro[0].c.value, 100000.0, 0.01);
    stand_baro_sample(&s, 0, SS_COMM_ERROR, NAN, t); // a failed read is not averaged
    CHECK_NEAR(s.baro[0].c.value, 100000.0, 0.01);
    for (int i = 0; i < STAND_BARO_AVG_N; i++, t += 50) {
        stand_baro_sample(&s, 0, SS_OK, 100100.0f, t);
    }
    CHECK_NEAR(s.baro[0].c.value, 100100.0, 0.01); // old samples fully replaced
    t += STAND_BARO_STALE_MS + 50; // outage longer than the stale limit
    stand_baro_sample(&s, 0, SS_OK, 99000.0f, t);
    CHECK_NEAR(s.baro[0].c.value, 99000.0, 0.01); // window restarted, no pre-outage mix
    CHECK_NEAR(s.baro[1].c.value, 0.0, 0.01); // other channel untouched
}

void test_jsonw(void)
{
    char buf[64];
    jw_t w;
    jw_init(&w, buf, sizeof(buf));
    jw_raw(&w, "{\"a\":");
    jw_str(&w, "q\"b\\\n");
    jw_raw(&w, ",\"n\":");
    jw_num(&w, NAN, 2);
    jw_raw(&w, ",\"x\":");
    jw_num(&w, -1.005, 1);
    jw_raw(&w, "}");
    CHECK(!w.overflow);
    CHECK(strcmp(buf, "{\"a\":\"q\\\"b\\\\\\u000a\",\"n\":null,\"x\":-1.0}") == 0);

    jw_init(&w, buf, 8);
    jw_raw(&w, "1234567");
    CHECK(!w.overflow);
    jw_raw(&w, "8");
    CHECK(w.overflow);
    jw_printf(&w, "%d", 1);
    CHECK(w.overflow && strcmp(buf, "1234567") == 0);
}

// Fake backend recording calls.
typedef struct {
    api_snapshot_t snap;
    stand_zero_start_t zero_ret;
    int zero_calls, reset_calls, cfg_calls, clear_calls;
    log_cfg_t last_cfg;
    int prepare_calls;
    bool pending;
    netcfg_t pending_cfg;
    char host[HTTP_MAX_HOST + 1]; // Host header of the next call
    char rid[HTTP_MAX_RID + 1];   // X-Req-Id of the next call
    char client[HTTP_MAX_RID + 1]; // X-Client-Id of the next call
    char out[16384];
    size_t out_len;
} fake_t;

static void f_snapshot(void *ctx, api_snapshot_t *o) { *o = ((fake_t *)ctx)->snap; }
static stand_zero_start_t f_zero(void *ctx, int ch) { (void)ch; fake_t *f = ctx; f->zero_calls++; return f->zero_ret; }
static bool f_reset(void *ctx, int ch) { (void)ch; ((fake_t *)ctx)->reset_calls++; return true; }
static persist_t f_cfg(void *ctx, const log_cfg_t *c)
{
    fake_t *f = ctx;
    f->cfg_calls++;
    f->last_cfg = *c;
    return PERSIST_OK;
}
static bool f_clear(void *ctx) { ((fake_t *)ctx)->clear_calls++; return true; }
static void f_prepare(void *ctx, const netcfg_t *c)
{
    fake_t *f = ctx;
    f->prepare_calls++;
    f->pending = true;
    f->pending_cfg = *c;
}
static bool f_pending(void *ctx, netcfg_t *out)
{
    fake_t *f = ctx;
    if (f->pending) {
        *out = f->pending_cfg;
    }
    return f->pending;
}
static size_t f_read(void *ctx, uint32_t before, hl_record_t *out, size_t max, uint32_t *next_before)
{
    (void)ctx;
    uint32_t start = before == HL_SEQ_EMPTY ? 3 : before;
    *next_before = start > max ? start - (uint32_t)max : HL_SEQ_EMPTY;
    size_t n = 0;
    for (uint32_t seq = before == HL_SEQ_EMPTY ? 3 : before; seq > 0 && n < max; seq--, n++) {
        memset(&out[n], 0, sizeof(out[n]));
        out[n].seq = seq - 1;
        out[n].p_pa[0] = 100000.0f;
        out[n].h_m[0] = NAN;
        out[n].p_pa[1] = NAN;
        out[n].h_m[1] = NAN;
        out[n].flags[0] = HL_F_MAKE(true, false, SS_OK);
        out[n].flags[1] = HL_F_MAKE(false, false, SS_NOT_FOUND);
    }
    return n;
}
static int f_write(void *ctx, const void *d, size_t n)
{
    fake_t *f = ctx;
    if (f->out_len + n >= sizeof(f->out)) {
        return -1;
    }
    memcpy(f->out + f->out_len, d, n);
    f->out_len += n;
    f->out[f->out_len] = '\0';
    return 0;
}

static api_after_t call(fake_t *f, http_method_t m, const char *path, const char *query, const char *body,
                        bool gzip)
{
    static const uint8_t page[] = "<html>plain</html>";
    static const uint8_t page_gz[] = "GZ";
    api_backend_t b = {
        .ctx = f, .snapshot = f_snapshot, .zero_start = f_zero, .zero_reset = f_reset,
        .log_config = f_cfg, .log_clear = f_clear, .log_read = f_read,
        .net_prepare = f_prepare, .net_pending = f_pending,
        .page = page, .page_len = sizeof(page) - 1, .page_gz = page_gz, .page_gz_len = 2,
        .http_port = 80,
    };
    api_out_t out = {.ctx = f, .write = f_write};
    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = m;
    strcpy(req.path, path);
    strcpy(req.query, query ? query : "");
    strcpy(req.body, body ? body : "");
    req.body_len = strlen(req.body);
    req.accept_gzip = gzip;
    strcpy(req.host, f->host);
    strcpy(req.rid, f->rid);
    strcpy(req.client, f->client);
    f->out_len = 0;
    f->out[0] = '\0';
    char scratch[6144];
    return api_handle(&b, &req, &out, scratch, sizeof(scratch));
}

static bool has(const fake_t *f, const char *s) { return strstr(f->out, s) != NULL; }

void test_api(void)
{
    fake_t *f = calloc(1, sizeof(*f));
    stand_init(&f->snap.st);
    netcfg_default(&f->snap.net);
    f->snap.platform = "test";
    f->snap.http_port = 80;
    f->snap.now_ms = 10000;
    f->snap.log.interval_s = 10;

    call(f, HTTP_GET, "/", NULL, NULL, true);
    CHECK(has(f, "200 OK") && has(f, "Content-Encoding: gzip") && has(f, "Content-Length: 2\r\n"));
    call(f, HTTP_GET, "/", NULL, NULL, false);
    CHECK(!has(f, "gzip") && has(f, "<html>plain</html>"));
    call(f, HTTP_HEAD, "/", NULL, NULL, false);
    CHECK(has(f, "Content-Length: 18\r\n") && !has(f, "<html>"));
    call(f, HTTP_POST, "/", NULL, NULL, false);
    CHECK(has(f, "405"));
    call(f, HTTP_GET, "/nope", NULL, NULL, false);
    CHECK(has(f, "404") && has(f, "Сторінку не знайдено"));
    call(f, HTTP_GET, "/api/zero", NULL, NULL, false);
    CHECK(has(f, "405"));
    call(f, HTTP_POST, "/api/state", NULL, NULL, false);
    CHECK(has(f, "405"));
    // HEAD error replies carry Content-Length but no body, or the shared stream desyncs.
    call(f, HTTP_HEAD, "/api/zero", NULL, NULL, false);
    CHECK(has(f, "405") && has(f, "Content-Length: ") && strstr(f->out, "\r\n\r\n")[4] == '\0');
    call(f, HTTP_HEAD, "/nope", NULL, NULL, false);
    CHECK(has(f, "404") && strstr(f->out, "\r\n\r\n")[4] == '\0');
    call(f, HTTP_POST, "/index.html", NULL, NULL, false);
    CHECK(has(f, "405") && has(f, "\"ok\":false"));

    // State before any data: no numbers, Ukrainian statuses, altitude null.
    call(f, HTTP_GET, "/api/state", NULL, NULL, false);
    CHECK(has(f, "\"name\":\"BMP585\"") && has(f, "\"name\":\"BMP581\""));
    CHECK(has(f, "\"hpa\":null") && has(f, "\"alt_m\":null") && has(f, "\"cm\":null"));
    CHECK(has(f, "Нуль не встановлено") && has(f, "Ініціалізація"));
    CHECK(has(f, "\"ip\":\"192.168.144.36\"") && has(f, "\"gw\":null"));
    CHECK(!has(f, "\"hpa\":0.00"));

    // State with data, zero and a failed channel.
    stand_t *s = &f->snap.st;
    for (uint32_t t = 9000; t <= 10000; t += 50) {
        stand_baro_sample(s, 0, SS_OK, 101325.0f, t);
    }
    zero_cfg_t z = {.set = true, .p0_pa = 101400.0f};
    stand_load_zero(s, 0, &z, ST_LOADED);
    stand_baro_sample(s, 1, SS_NOT_FOUND, NAN, 10000);
    call(f, HTTP_GET, "/api/state", NULL, NULL, false);
    CHECK(has(f, "\"hpa\":1013.25") && has(f, "\"p0_hpa\":1014.00"));
    CHECK(has(f, "\"alt_m\":6.24")); // p < p0 -> positive
    CHECK(has(f, "Нуль відновлено з пам'яті") && has(f, "Датчик не відповідає"));

    // Zero.
    f->zero_ret = STAND_ZERO_STARTED;
    call(f, HTTP_POST, "/api/zero", NULL, "ch=1", false);
    CHECK(has(f, "200 OK") && f->zero_calls == 1);
    f->zero_ret = STAND_ZERO_BUSY;
    call(f, HTTP_POST, "/api/zero", NULL, "ch=0", false);
    CHECK(has(f, "409") && has(f, "Калібрування вже виконується"));
    f->zero_ret = STAND_ZERO_NO_DATA;
    call(f, HTTP_POST, "/api/zero", NULL, "ch=0", false);
    CHECK(has(f, "422"));
    call(f, HTTP_POST, "/api/zero", NULL, "ch=2", false);
    CHECK(has(f, "400") && f->zero_calls == 3);
    call(f, HTTP_POST, "/api/zero", NULL, "", false);
    CHECK(has(f, "400"));
    call(f, HTTP_POST, "/api/zero/reset", NULL, "ch=0", false);
    CHECK(has(f, "200 OK") && f->reset_calls == 1);

    // Log settings.
    call(f, HTTP_POST, "/api/log/config", NULL, "enabled=1&interval=5", false);
    CHECK(has(f, "200 OK") && f->last_cfg.enabled && f->last_cfg.interval_s == 5);
    call(f, HTTP_POST, "/api/log/config", NULL, "enabled=1&interval=0", false);
    CHECK(has(f, "422") && f->cfg_calls == 1);
    call(f, HTTP_POST, "/api/log/config", NULL, "enabled=1&interval=3601", false);
    CHECK(has(f, "422"));
    call(f, HTTP_POST, "/api/log/config", NULL, "enabled=2&interval=10", false);
    CHECK(has(f, "400") && f->cfg_calls == 1);
    call(f, HTTP_POST, "/api/log/clear", NULL, NULL, false);
    CHECK(has(f, "200 OK") && f->clear_calls == 1);
    call(f, HTTP_GET, "/api/log", "limit=2", NULL, false);
    CHECK(has(f, "\"seq\":2") && has(f, "\"seq\":1") && !has(f, "\"seq\":0"));
    CHECK(has(f, "\"next_before\":1") && has(f, "\"alt_m\":null"));
    call(f, HTTP_GET, "/api/log", "before=1&limit=20", NULL, false);
    CHECK(has(f, "\"seq\":0") && has(f, "\"next_before\":null"));

    // Request id is echoed so the page can spot replies meant for another tab; never left over.
    strcpy(f->rid, "17");
    call(f, HTTP_GET, "/api/state", NULL, NULL, false);
    CHECK(has(f, "X-Req-Id: 17\r\n"));
    call(f, HTTP_GET, "/", NULL, NULL, true);
    CHECK(has(f, "Content-Encoding: gzip\r\nX-Req-Id: 17\r\n"));
    f->rid[0] = '\0';
    call(f, HTTP_GET, "/api/state", NULL, NULL, false);
    CHECK(!has(f, "X-Req-Id"));

    // Open pages are counted by X-Client-Id within a 3 s window; requests without an id are not counted.
    f->snap.now_ms = 100000;
    strcpy(f->client, "pageA");
    call(f, HTTP_GET, "/api/state", NULL, NULL, false);
    CHECK(has(f, "\"clients\":1,"));
    strcpy(f->client, "pageB");
    f->snap.now_ms = 101000;
    call(f, HTTP_GET, "/api/state", NULL, NULL, false);
    CHECK(has(f, "\"clients\":2,"));
    f->snap.now_ms = 103500; // pageA silent for 3.5 s, pageB polls
    call(f, HTTP_GET, "/api/state", NULL, NULL, false);
    CHECK(has(f, "\"clients\":1,"));
    const char *many[] = {"p1", "p2", "p3", "p4", "p5"};
    for (int i = 0; i < 5; i++) {
        strcpy(f->client, many[i]);
        call(f, HTTP_GET, "/api/state", NULL, NULL, false);
    }
    CHECK(has(f, "\"clients\":4,")); // table is full, still more than one
    f->client[0] = '\0';
    f->snap.now_ms = 200000;
    call(f, HTTP_GET, "/api/state", NULL, NULL, false);
    CHECK(has(f, "\"clients\":0,"));

    // Network: invalid input never prepares a change.
    strcpy(f->host, "192.168.144.36");
    api_after_t a = call(f, HTTP_POST, "/api/net", NULL, "ip=192.168.144.0&mask=255.255.255.0&gw=", false);
    CHECK(has(f, "422") && has(f, "ip_host_part") && !a.net_apply && f->prepare_calls == 0);
    a = call(f, HTTP_POST, "/api/net", NULL, "ip=10.1.1.1", false);
    CHECK(has(f, "400") && !a.net_apply && f->prepare_calls == 0);
    // Valid input: pending only, plus the form the page must send to the module.
    a = call(f, HTTP_POST, "/api/net", NULL, "ip=192.168.144.50&mask=255.255.255.0&gw=192.168.144.1", false);
    CHECK(has(f, "200 OK") && has(f, "\"url\":\"http://192.168.144.50/\"") && !a.net_apply);
    CHECK(f->prepare_calls == 1 && f->pending_cfg.ip == 0xC0A89032u && f->pending_cfg.gw == 0xC0A89001u);
    CHECK(has(f, "\"module_url\":\"http://192.168.144.36:8000/\""));
    CHECK(has(f, "lca=192.168.144.50&lcp=80&wbp=8000") && has(f, "lcm=255.255.255.0&lcg=192.168.144.1"));
    CHECK(has(f, "bdr=B&dtb=0&prt=0&stb=0&flc=0&ndr=0") && has(f, "post=Submit"));
    // Only an explicit commit that arrived at the new address applies it; ordinary requests never do.
    a = call(f, HTTP_GET, "/api/state", NULL, NULL, false);
    CHECK(!a.net_apply);
    strcpy(f->host, "192.168.144.50");
    a = call(f, HTTP_GET, "/api/state", NULL, NULL, false);
    CHECK(!a.net_apply); // a poll at the new address is not a commit
    strcpy(f->host, "192.168.144.36");
    a = call(f, HTTP_POST, "/api/net/commit", NULL, NULL, false);
    CHECK(!a.net_apply && has(f, "409") && has(f, "wrong_host"));
    strcpy(f->host, "stand.local");
    a = call(f, HTTP_POST, "/api/net/commit", NULL, NULL, false);
    CHECK(!a.net_apply && has(f, "wrong_host"));
    strcpy(f->host, "192.168.144.50");
    a = call(f, HTTP_GET, "/api/net/commit", NULL, NULL, false);
    CHECK(!a.net_apply && has(f, "405"));
    a = call(f, HTTP_POST, "/api/net/commit", NULL, NULL, false);
    CHECK(a.net_apply && a.net.ip == 0xC0A89032u && a.net.gw == 0xC0A89001u && has(f, "200 OK"));
    // A second change prepared at the pending address does not commit the first one.
    a = call(f, HTTP_POST, "/api/net", NULL, "ip=192.168.144.51&mask=255.255.255.0&gw=", false);
    CHECK(!a.net_apply && f->pending_cfg.ip == 0xC0A89033u);
    f->pending = false;
    a = call(f, HTTP_POST, "/api/net/commit", NULL, NULL, false);
    CHECK(!a.net_apply && has(f, "no_pending"));
    // Without a usable Host the module URL falls back to the active address from the snapshot.
    f->pending = false;
    f->host[0] = '\0';
    f->snap.net.ip = 0x0A000005u;
    a = call(f, HTTP_POST, "/api/net", NULL, "ip=10.0.0.6&mask=255.0.0.0&gw=", false);
    CHECK(has(f, "\"module_url\":\"http://10.0.0.5:8000/\"") && has(f, "lcg=0.0.0.0") && !a.net_apply);

    free(f);
}
