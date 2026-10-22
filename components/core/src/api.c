#include "api.h"

#include "form.h"
#include "jsonw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *const BARO_NAMES[STAND_BARO_N] = {"BMP585", "BMP581"};

const char *api_sensor_status_code(sensor_status_t st)
{
    switch (st) {
    case SS_INIT: return "init";
    case SS_OK: return "ok";
    case SS_NO_ECHO: return "no_echo";
    case SS_OUT_OF_RANGE: return "out_of_range";
    case SS_NOT_FOUND: return "not_found";
    case SS_COMM_ERROR: return "comm_error";
    case SS_REINIT: return "reinit";
    case SS_STALE: return "stale";
    }
    return "unknown";
}

const char *api_sensor_status_text(sensor_status_t st)
{
    switch (st) {
    case SS_INIT: return "Ініціалізація";
    case SS_OK: return "Норма";
    case SS_NO_ECHO: return "Немає сигналу ECHO";
    case SS_OUT_OF_RANGE: return "Значення поза діапазоном";
    case SS_NOT_FOUND: return "Датчик не відповідає";
    case SS_COMM_ERROR: return "Помилка обміну I²C";
    case SS_REINIT: return "Повторна ініціалізація";
    case SS_STALE: return "Немає свіжих даних";
    }
    return "Невідомий стан";
}

static const char *origin_code(st_origin_t o)
{
    return o == ST_LOADED ? "loaded" : o == ST_CORRUPT ? "corrupt" : "missing";
}

static const char *persist_code(persist_t p)
{
    switch (p) {
    case PERSIST_PENDING: return "pending";
    case PERSIST_OK: return "ok";
    case PERSIST_ERROR: return "error";
    default: return "none";
    }
}

static const char *zero_text(const baro_t *b)
{
    if (b->cal_view == CAL_RUNNING) {
        return "Калібрування…";
    }
    if (b->persist == PERSIST_ERROR) {
        return "Помилка запису нуля у пам'ять";
    }
    switch (b->origin) {
    case ZO_RESTORED: return "Нуль відновлено з пам'яті";
    case ZO_NEW: return "Нуль встановлено";
    default: return "Нуль не встановлено";
    }
}

static const char *cal_text(const baro_t *b)
{
    switch (b->cal_view) {
    case CAL_RUNNING: return "Калібрування…";
    case CAL_OK: return "Нуль встановлено успішно";
    case CAL_FAILED:
        return b->cal_err == CAL_ERR_NO_DATA
                   ? "Помилка: немає валідних даних датчика, попередній нуль збережено"
                   : "Помилка: недостатньо валідних вимірювань за 3 с, попередній нуль збережено";
    default: return NULL;
    }
}

static int send(const api_out_t *out, int status, const char *ctype, const void *body, size_t len,
                const char *extra, bool head_only)
{
    char head[256];
    size_t n = http_format_head(head, sizeof(head), status, ctype, len, extra);
    if (n == 0) {
        return -1;
    }
    int err = out->write(out->ctx, head, n);
    if (!err && !head_only && len > 0) {
        err = out->write(out->ctx, body, len);
    }
    return err;
}

static void send_json(const api_out_t *out, int status, jw_t *w, bool head_only)
{
    if (w->overflow) {
        static const char oops[] = "{\"ok\":false,\"error\":\"overflow\",\"message\":\"Відповідь завелика\"}";
        send(out, 500, "application/json; charset=utf-8", oops, sizeof(oops) - 1, NULL, head_only);
        return;
    }
    send(out, status, "application/json; charset=utf-8", w->buf, w->len, NULL, head_only);
}

// head_only must follow the request: a body sent to a HEAD client desyncs the shared stream.
static void send_fail_h(const api_out_t *out, bool head_only, int status, const char *code,
                        const char *msg, char *scratch, size_t cap)
{
    jw_t w;
    jw_init(&w, scratch, cap);
    jw_raw(&w, "{\"ok\":false,\"error\":");
    jw_str(&w, code);
    jw_raw(&w, ",\"message\":");
    jw_str(&w, msg);
    jw_raw(&w, "}");
    send_json(out, status, &w, head_only);
}

// POST-only handlers: the request can never be HEAD here.
static void send_fail(const api_out_t *out, int status, const char *code, const char *msg,
                      char *scratch, size_t cap)
{
    send_fail_h(out, false, status, code, msg, scratch, cap);
}

static void send_ok(const api_out_t *out, const char *msg, char *scratch, size_t cap)
{
    jw_t w;
    jw_init(&w, scratch, cap);
    jw_raw(&w, "{\"ok\":true,\"message\":");
    jw_str(&w, msg);
    jw_raw(&w, "}");
    send_json(out, 200, &w, false);
}

// Address 0 is rendered as null (used for "no gateway").
static void json_ip(jw_t *w, const char *key, uint32_t a)
{
    jw_printf(w, "\"%s\":", key);
    if (a == 0) {
        jw_raw(w, "null");
        return;
    }
    char ip[16];
    netcfg_format_ipv4(a, ip, sizeof(ip));
    jw_str(w, ip);
}

static void json_chan_common(jw_t *w, const chan_t *c, uint32_t stale_ms, uint32_t now, double scale,
                             int decimals, const char *unit_key)
{
    bool fresh = stand_chan_fresh(c, stale_ms, now);
    jw_raw(w, "\"st\":");
    jw_str(w, api_sensor_status_code(c->status));
    jw_raw(w, ",\"st_text\":");
    jw_str(w, api_sensor_status_text(c->status));
    jw_raw(w, ",\"fresh\":");
    jw_bool(w, fresh);
    jw_printf(w, ",\"%s\":", unit_key);
    jw_num(w, fresh ? c->value * scale : NAN, decimals);
    jw_printf(w, ",\"last_%s\":", unit_key); // last valid value, shown as stale by the UI
    jw_num(w, c->ever_ok ? c->value * scale : NAN, decimals);
    jw_raw(w, ",\"age_ms\":");
    if (c->ever_ok) {
        jw_printf(w, "%u", (unsigned)(now - c->last_ok_ms));
    } else {
        jw_raw(w, "null");
    }
    jw_printf(w, ",\"errors\":%u", (unsigned)c->errors);
}

static void render_state(jw_t *w, const api_snapshot_t *s)
{
    uint32_t now = s->now_ms;
    jw_raw(w, "{\"platform\":");
    jw_str(w, s->platform);
    jw_printf(w, ",\"up_s\":%u,\"session\":%u", (unsigned)s->uptime_s, (unsigned)s->session);

    jw_raw(w, ",\"sonar\":{");
    json_chan_common(w, &s->st.sonar, STAND_SONAR_STALE_MS, now, 1.0, 1, "cm");
    jw_raw(w, "}");

    jw_raw(w, ",\"baro\":[");
    for (int ch = 0; ch < STAND_BARO_N; ch++) {
        const baro_t *b = &s->st.baro[ch];
        float alt;
        bool has_alt = stand_baro_altitude(b, now, &alt);
        jw_raw(w, ch ? ",{" : "{");
        jw_raw(w, "\"name\":");
        jw_str(w, BARO_NAMES[ch]);
        jw_raw(w, ",");
        json_chan_common(w, &b->c, STAND_BARO_STALE_MS, now, 0.01, 2, "hpa");
        jw_raw(w, ",\"alt_m\":");
        jw_num(w, has_alt ? alt : NAN, 2);
        jw_raw(w, ",\"zero_set\":");
        jw_bool(w, b->zero.set);
        jw_raw(w, ",\"zero_origin\":");
        jw_str(w, b->origin == ZO_RESTORED ? "restored" : b->origin == ZO_NEW ? "new" : "none");
        jw_raw(w, ",\"zero_text\":");
        jw_str(w, zero_text(b));
        jw_raw(w, ",\"p0_hpa\":");
        jw_num(w, b->zero.set ? b->zero.p0_pa * 0.01 : NAN, 2);
        jw_raw(w, ",\"zero_load\":");
        jw_str(w, origin_code(b->load_origin));
        jw_raw(w, ",\"persist\":");
        jw_str(w, persist_code(b->persist));
        static const char *const cal_codes[] = {"idle", "running", "ok", "failed"};
        jw_raw(w, ",\"cal\":");
        jw_str(w, cal_codes[b->cal_view]);
        jw_raw(w, ",\"cal_text\":");
        jw_str(w, cal_text(b));
        jw_raw(w, ",\"cal_spread_pa\":");
        jw_num(w, b->cal_view == CAL_OK ? b->cal_spread_pa : NAN, 1);
        jw_raw(w, "}");
    }
    jw_raw(w, "]");

    jw_raw(w, ",\"log\":{\"enabled\":");
    jw_bool(w, s->log.enabled);
    jw_printf(w, ",\"interval_s\":%u,\"count\":%u,\"capacity\":%u,\"capacity_min\":%u,\"bytes\":%u",
              (unsigned)s->log.interval_s, (unsigned)s->log_count, (unsigned)s->log_capacity,
              (unsigned)s->log_capacity_guaranteed, (unsigned)s->log_bytes);
    jw_printf(w, ",\"write_errors\":%u,\"corrupt\":%u,\"last_error\":%d", (unsigned)s->log_write_errors,
              (unsigned)s->log_corrupt, s->log_last_error);
    jw_raw(w, ",\"clearing\":");
    jw_bool(w, s->log_clearing);
    jw_raw(w, ",\"origin\":");
    jw_str(w, origin_code(s->log_origin));
    jw_raw(w, ",\"persist\":");
    jw_str(w, persist_code(s->log_persist));
    jw_raw(w, "}");

    jw_raw(w, ",\"net\":{");
    json_ip(w, "ip", s->net.ip);
    jw_raw(w, ",");
    json_ip(w, "mask", s->net.mask);
    jw_raw(w, ",");
    json_ip(w, "gw", s->net.gw);
    jw_printf(w, ",\"port\":%u,\"origin\":", (unsigned)s->http_port);
    jw_str(w, origin_code(s->net_origin));
    static const char *const apply_codes[] = {"idle", "pending", "ok", "failed"};
    jw_raw(w, ",\"apply\":");
    jw_str(w, apply_codes[s->net_apply]);
    jw_raw(w, ",\"apply_msg\":");
    jw_str(w, s->net_apply_msg);
    jw_raw(w, ",\"link\":");
    if (s->link_known) {
        jw_raw(w, "{\"cable\":");
        jw_bool(w, s->link_cable);
        jw_raw(w, ",\"tcp\":");
        jw_bool(w, s->link_tcp);
        jw_raw(w, "}");
    } else {
        jw_raw(w, "null");
    }
    jw_raw(w, "}");

    jw_printf(w, ",\"http\":{\"requests\":%u,\"errors\":%u,\"timeouts\":%u,\"skipped\":%u}}",
              (unsigned)s->http.requests, (unsigned)s->http.errors, (unsigned)s->http.timeouts,
              (unsigned)s->http.skipped_bytes);
}

static void render_log(jw_t *w, const hl_record_t *r, size_t n, uint32_t next_before)
{
    jw_raw(w, "{\"ok\":true,\"records\":[");
    for (size_t i = 0; i < n; i++) {
        jw_printf(w, "%s{\"seq\":%u,\"session\":%u,\"t_s\":%u,\"ch\":[", i ? "," : "",
                  (unsigned)r[i].seq, (unsigned)r[i].session, (unsigned)r[i].t_s);
        for (int ch = 0; ch < HL_CHANNELS; ch++) {
            uint8_t f = r[i].flags[ch];
            jw_raw(w, ch ? ",{\"valid\":" : "{\"valid\":");
            jw_bool(w, f & HL_F_VALID);
            jw_raw(w, ",\"zero\":");
            jw_bool(w, f & HL_F_ZERO);
            jw_raw(w, ",\"st\":");
            jw_str(w, api_sensor_status_code((sensor_status_t)HL_F_STATUS(f)));
            jw_raw(w, ",\"st_text\":");
            jw_str(w, api_sensor_status_text((sensor_status_t)HL_F_STATUS(f)));
            jw_raw(w, ",\"hpa\":");
            jw_num(w, r[i].p_pa[ch] * 0.01, 2);
            jw_raw(w, ",\"alt_m\":");
            jw_num(w, r[i].h_m[ch], 2);
            jw_raw(w, "}");
        }
        jw_raw(w, "]}");
    }
    jw_raw(w, "],\"next_before\":");
    if (next_before != HL_SEQ_EMPTY) {
        jw_printf(w, "%u", (unsigned)next_before);
    } else {
        jw_raw(w, "null");
    }
    jw_raw(w, "}");
}

static bool get_channel(const http_request_t *req, int *ch)
{
    long v;
    if (!form_get_long(req->body, "ch", 0, STAND_BARO_N - 1, &v)) {
        return false;
    }
    *ch = (int)v;
    return true;
}

api_after_t api_handle(const api_backend_t *b, const http_request_t *req, const api_out_t *out,
                       char *scratch, size_t cap)
{
    api_after_t after = {0};
    const char *path = req->path;
    bool is_get = req->method == HTTP_GET || req->method == HTTP_HEAD;
    bool head_only = req->method == HTTP_HEAD;
    bool is_post = req->method == HTTP_POST;

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        if (!is_get) {
            send_fail_h(out, head_only, 405, "method", "Метод не підтримується", scratch, cap);
        } else if (req->accept_gzip && b->page_gz) {
            send(out, 200, "text/html; charset=utf-8", b->page_gz, b->page_gz_len,
                 "Content-Encoding: gzip\r\n", head_only);
        } else {
            send(out, 200, "text/html; charset=utf-8", b->page, b->page_len, NULL, head_only);
        }
        return after;
    }
    if (strcmp(path, "/favicon.ico") == 0) {
        send(out, 204, "image/x-icon", NULL, 0, NULL, true);
        return after;
    }
    if (strcmp(path, "/api/state") == 0 && is_get) {
        api_snapshot_t snap;
        b->snapshot(b->ctx, &snap);
        jw_t w;
        jw_init(&w, scratch, cap);
        render_state(&w, &snap);
        send_json(out, 200, &w, head_only);
        return after;
    }
    if (strcmp(path, "/api/log") == 0 && is_get) {
        long before = HL_SEQ_EMPTY, limit = API_LOG_PAGE_MAX;
        form_get_long(req->query, "before", 0, 0x7FFFFFFFL, &before);
        form_get_long(req->query, "limit", 1, API_LOG_PAGE_MAX, &limit);
        hl_record_t recs[API_LOG_PAGE_MAX];
        uint32_t next = HL_SEQ_EMPTY;
        size_t n = b->log_read(b->ctx, (uint32_t)before, recs, (size_t)limit, &next);
        jw_t w;
        jw_init(&w, scratch, cap);
        render_log(&w, recs, n, next);
        send_json(out, 200, &w, head_only);
        return after;
    }
    // Remaining routes are POST-only; anything else is 405 for a known path, 404 otherwise.
    static const char *const post_routes[] = {"/api/zero", "/api/zero/reset", "/api/log/config",
                                              "/api/log/clear", "/api/net"};
    static const char *const get_routes[] = {"/api/state", "/api/log"};
    bool post_route = false, get_route = false;
    for (size_t i = 0; i < sizeof(post_routes) / sizeof(post_routes[0]); i++) {
        post_route |= strcmp(path, post_routes[i]) == 0;
    }
    for (size_t i = 0; i < sizeof(get_routes) / sizeof(get_routes[0]); i++) {
        get_route |= strcmp(path, get_routes[i]) == 0;
    }
    if (!post_route || !is_post) {
        if (post_route || get_route) {
            send_fail_h(out, head_only, 405, "method", "Метод не підтримується", scratch, cap);
        } else {
            send_fail_h(out, head_only, 404, "not_found", "Сторінку не знайдено", scratch, cap);
        }
        return after;
    }

    int ch;
    if (strcmp(path, "/api/zero") == 0) {
        if (!get_channel(req, &ch)) {
            send_fail(out, 400, "bad_channel", "Невірний номер каналу", scratch, cap);
            return after;
        }
        switch (b->zero_start(b->ctx, ch)) {
        case STAND_ZERO_STARTED:
            send_ok(out, "Калібрування розпочато", scratch, cap);
            break;
        case STAND_ZERO_BUSY:
            send_fail(out, 409, "busy", "Калібрування вже виконується", scratch, cap);
            break;
        case STAND_ZERO_NO_DATA:
            send_fail(out, 422, "no_data", "Немає валідних даних датчика, попередній нуль збережено",
                      scratch, cap);
            break;
        }
    } else if (strcmp(path, "/api/zero/reset") == 0) {
        if (!get_channel(req, &ch)) {
            send_fail(out, 400, "bad_channel", "Невірний номер каналу", scratch, cap);
        } else if (!b->zero_reset(b->ctx, ch)) {
            send_fail(out, 409, "busy", "Зачекайте завершення калібрування", scratch, cap);
        } else {
            send_ok(out, "Збережений нуль скинуто", scratch, cap);
        }
    } else if (strcmp(path, "/api/log/config") == 0) {
        long en, iv;
        if (!form_get_long(req->body, "enabled", 0, 1, &en)) {
            send_fail(out, 400, "bad_enabled", "Невірне значення перемикача журналювання", scratch, cap);
        } else if (!form_get_long(req->body, "interval", LOG_INTERVAL_MIN_S, LOG_INTERVAL_MAX_S, &iv)) {
            send_fail(out, 422, "bad_interval", "Інтервал має бути цілим числом від 1 до 3600 с",
                      scratch, cap);
        } else {
            log_cfg_t c = {.enabled = en == 1, .interval_s = (uint16_t)iv};
            if (b->log_config(b->ctx, &c) == PERSIST_OK) {
                send_ok(out, "Налаштування журналу збережено", scratch, cap);
            } else {
                send_fail(out, 500, "persist", "Налаштування застосовано, але не збережено у пам'ять",
                          scratch, cap);
            }
        }
    } else if (strcmp(path, "/api/log/clear") == 0) {
        if (b->log_clear(b->ctx)) {
            send_ok(out, "Очищення історії розпочато", scratch, cap);
        } else {
            send_fail(out, 409, "busy", "Очищення вже виконується", scratch, cap);
        }
    } else if (strcmp(path, "/api/net") == 0) {
        char ip[20], mask[20], gw[20];
        if (!form_get(req->body, "ip", ip, sizeof(ip)) || !form_get(req->body, "mask", mask, sizeof(mask))) {
            send_fail(out, 400, "missing", "Заповніть IP-адресу та маску", scratch, cap);
            return after;
        }
        if (!form_get(req->body, "gw", gw, sizeof(gw))) {
            gw[0] = '\0';
        }
        netcfg_t cfg;
        netcfg_err_t err = netcfg_from_strings(ip, mask, gw, &cfg);
        if (err != NETCFG_OK) {
            send_fail(out, 422, netcfg_err_code(err), netcfg_err_text(err), scratch, cap);
            return after;
        }
        jw_t w;
        jw_init(&w, scratch, cap);
        jw_raw(&w, "{\"ok\":true,\"message\":");
        jw_str(&w, "Налаштування прийнято, стенд перемикається на нову адресу");
        jw_raw(&w, ",\"url\":");
        char url[40];
        netcfg_format_ipv4(cfg.ip, ip, sizeof(ip));
        if (b->http_port == 80) {
            snprintf(url, sizeof(url), "http://%s/", ip);
        } else {
            snprintf(url, sizeof(url), "http://%s:%u/", ip, (unsigned)b->http_port);
        }
        jw_str(&w, url);
        jw_raw(&w, "}");
        send_json(out, 200, &w, false);
        after.net_apply = true;
        after.net = cfg;
    } else {
        send_fail(out, 404, "not_found", "Сторінку не знайдено", scratch, cap);
    }
    return after;
}

void api_send_error(const api_out_t *out, int status, bool head_only, char *scratch, size_t cap)
{
    const char *msg;
    switch (status) {
    case 413: msg = "Запит завеликий"; break;
    case 414: msg = "Адреса запиту задовга"; break;
    case 431: msg = "Заголовки запиту завеликі"; break;
    case 501: msg = "Метод або кодування не підтримується"; break;
    case 505: msg = "Версія HTTP не підтримується"; break;
    default: msg = "Некоректний запит"; break;
    }
    send_fail_h(out, head_only, status, "http", msg, scratch, cap);
}
