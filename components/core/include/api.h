#pragma once
// HTTP-маршрути вебінтерфейсу. Код платформи дає бекенд (блокування, збереження,
// залізо) і вихідний приймач; цей модуль лише маршрутизує, перевіряє і формує JSON.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "http_parser.h"
#include "netcfg.h"
#include "settings_codec.h"
#include "stand.h"

#ifdef __cplusplus
extern "C" {
#endif

#define API_LOG_PAGE_MAX 20

typedef enum {
    NET_APPLY_IDLE = 0,
    NET_APPLY_PENDING,
    NET_APPLY_OK,
    NET_APPLY_FAILED,
} net_apply_t;

typedef struct {
    uint32_t now_ms;
    uint32_t uptime_s;
    uint16_t session;
    stand_t st;

    log_cfg_t log;
    st_origin_t log_origin;
    persist_t log_persist;
    bool log_clearing;
    uint32_t log_count;
    uint32_t log_capacity;
    uint32_t log_capacity_guaranteed; // місткість мінус один сектор, див. histlog.h
    uint32_t log_bytes;
    uint32_t log_write_errors;
    uint32_t log_corrupt;
    int log_last_error;

    netcfg_t net;          // активні параметри
    st_origin_t net_origin;
    net_apply_t net_apply;
    netcfg_t net_pending;  // дійсне, поки net_apply == NET_APPLY_PENDING
    const char *net_apply_msg; // остання помилка застосування українською, або NULL
    uint16_t http_port;
    bool link_known;       // виводи LINK/LINK2 під'єднані
    bool link_cable;
    bool link_tcp;

    http_stats_t http;
    const char *platform;  // "esp32" або "emulator"
} api_snapshot_t;

typedef struct {
    void *ctx;
    void (*snapshot)(void *ctx, api_snapshot_t *out);
    stand_zero_start_t (*zero_start)(void *ctx, int ch);
    bool (*zero_reset)(void *ctx, int ch);                 // false: калібрування триває
    persist_t (*log_config)(void *ctx, const log_cfg_t *cfg); // повертає результат збереження
    bool (*log_clear)(void *ctx);                          // false: очищення вже триває
    // Записи до `before`, найновіші першими; *oldest — найстаріший живий seq. -1: журнал зайнятий, спробуйте ще.
    int (*log_read)(void *ctx, uint32_t before, hl_record_t *out, size_t max, uint32_t *next_before,
                    uint32_t *oldest);
    // Перевірені налаштування чекають, поки модуль переїде; фіксуються через POST /api/net/commit на новій IP.
    void (*net_prepare)(void *ctx, const netcfg_t *cfg);
    bool (*net_pending)(void *ctx, netcfg_t *out);
    bool (*net_cancel)(void *ctx); // false: нічого не очікувало
    const uint8_t *page_gz;
    size_t page_gz_len;
    const uint8_t *page;
    size_t page_len;
    uint16_t http_port;
} api_backend_t;

// Вихідний приймач; повертає 0 при успіху. Великі тіла пишуться одним викликом, без копіювання.
typedef struct {
    void *ctx;
    int (*write)(void *ctx, const void *data, size_t len);
} api_out_t;

// Дії після повного надсилання відповіді. net_apply ставить POST /api/net/commit, що
// прийшов на нову адресу, а це доводить, що модуль переїхав: тоді платформа зберігає net як активну.
typedef struct {
    bool net_apply;
    netcfg_t net;
} api_after_t;

// scratch: буфер для тіл JSON, рекомендовано >= 4 KB.
api_after_t api_handle(const api_backend_t *b, const http_request_t *req, const api_out_t *out,
                       char *scratch, size_t scratch_cap);

// Відповідь на помилку парсера (400/413/431/...); передайте parser->error_head як head_only і parser->req.rid.
void api_send_error(const api_out_t *out, int status, bool head_only, const char *rid, char *scratch,
                    size_t scratch_cap);

const char *api_sensor_status_code(sensor_status_t st);
const char *api_sensor_status_text(sensor_status_t st);

#ifdef __cplusplus
}
#endif
