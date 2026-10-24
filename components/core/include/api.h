#pragma once
// HTTP routes of the web interface. Platform code supplies a backend (locking, persistence,
// hardware) and an output sink; this module only routes, validates and renders JSON.
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
    uint32_t log_capacity_guaranteed; // capacity minus one sector, see histlog.h
    uint32_t log_bytes;
    uint32_t log_write_errors;
    uint32_t log_corrupt;
    int log_last_error;

    netcfg_t net;          // active parameters
    st_origin_t net_origin;
    net_apply_t net_apply;
    netcfg_t net_pending;  // valid while net_apply == NET_APPLY_PENDING
    const char *net_apply_msg; // last apply error in Ukrainian, or NULL
    uint16_t http_port;
    bool link_known;       // LINK/LINK2 pins wired
    bool link_cable;
    bool link_tcp;

    http_stats_t http;
    const char *platform;  // "esp32" or "emulator"
} api_snapshot_t;

typedef struct {
    void *ctx;
    void (*snapshot)(void *ctx, api_snapshot_t *out);
    stand_zero_start_t (*zero_start)(void *ctx, int ch);
    bool (*zero_reset)(void *ctx, int ch);                 // false: calibration in progress
    persist_t (*log_config)(void *ctx, const log_cfg_t *cfg); // returns persist result
    bool (*log_clear)(void *ctx);                          // false: already clearing
    size_t (*log_read)(void *ctx, uint32_t before, hl_record_t *out, size_t max, uint32_t *next_before);
    // Validated settings waiting for the module to move; committed by POST /api/net/commit at the new IP.
    void (*net_prepare)(void *ctx, const netcfg_t *cfg);
    bool (*net_pending)(void *ctx, netcfg_t *out);
    bool (*net_cancel)(void *ctx); // false: nothing was pending
    const uint8_t *page_gz;
    size_t page_gz_len;
    const uint8_t *page;
    size_t page_len;
    uint16_t http_port;
} api_backend_t;

// Output sink; returns 0 on success. Large bodies are written in one call, never copied.
typedef struct {
    void *ctx;
    int (*write)(void *ctx, const void *data, size_t len);
} api_out_t;

// Actions to run after the response has been fully sent. net_apply is set by POST /api/net/commit that
// arrived at the pending address, which proves the module moved: the platform then stores net as active.
typedef struct {
    bool net_apply;
    netcfg_t net;
} api_after_t;

// scratch: buffer for JSON bodies, >= 4 KB recommended.
api_after_t api_handle(const api_backend_t *b, const http_request_t *req, const api_out_t *out,
                       char *scratch, size_t scratch_cap);

// Replies to a parser error (400/413/431/...); pass parser->error_head as head_only and parser->req.rid.
void api_send_error(const api_out_t *out, int status, bool head_only, const char *rid, char *scratch,
                    size_t scratch_cap);

const char *api_sensor_status_code(sensor_status_t st);
const char *api_sensor_status_text(sensor_status_t st);

#ifdef __cplusplus
}
#endif
