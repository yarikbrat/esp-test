#pragma once
// Streaming HTTP/1.x request parser for the UART byte stream.
// The UART-to-Ethernet bridge is transparent: no TCP boundaries, requests may arrive in pieces
// and junk may sit between them. Bounded buffers, timeouts and self-resync after errors.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HTTP_MAX_HEAD         1536 // request line + headers
#define HTTP_MAX_BODY         512
#define HTTP_MAX_SKIP         2048 // largest rejected body we skip instead of resyncing
#define HTTP_MAX_PATH         96
#define HTTP_MAX_QUERY        96
#define HTTP_MAX_HOST         64
#define HTTP_MAX_RID          16
#define HTTP_IDLE_TIMEOUT_MS  2000 // max gap between bytes inside a request
#define HTTP_TOTAL_TIMEOUT_MS 5000 // max time from first byte

typedef enum {
    HTTP_GET,
    HTTP_HEAD,
    HTTP_POST,
} http_method_t;

typedef struct {
    http_method_t method;
    char path[HTTP_MAX_PATH + 1];
    char query[HTTP_MAX_QUERY + 1]; // without '?', not decoded
    bool accept_gzip;
    char host[HTTP_MAX_HOST + 1]; // Host header without the port; "" if absent or too long
    char rid[HTTP_MAX_RID + 1];   // X-Req-Id header, letters and digits only; "" otherwise
    char client[HTTP_MAX_RID + 1]; // X-Client-Id header, same rules: one id per open page
    size_t body_len;
    char body[HTTP_MAX_BODY + 1]; // always NUL-terminated
} http_request_t;

typedef enum {
    HTTP_EV_NONE = 0,
    HTTP_EV_REQUEST, // parser->req holds a complete request
    HTTP_EV_ERROR,   // reply with parser->error_status
} http_event_t;

typedef struct {
    uint32_t requests;
    uint32_t errors;
    uint32_t timeouts;
    uint32_t skipped_bytes;
} http_stats_t;

typedef struct {
    int state;
    char head[HTTP_MAX_HEAD];
    size_t head_len;
    size_t body_need;
    uint32_t start_ms;
    uint32_t last_ms;
    uint32_t tail; // last 4 bytes while skipping to the end of headers
    int error_status;
    bool error_head; // the failed request was HEAD, so the error reply must have no body
    http_request_t req;
    http_stats_t stats;
} http_parser_t;

void http_parser_init(http_parser_t *p);

// Returns bytes consumed; stops right after an event so the caller can handle it and feed the rest.
size_t http_parser_feed(http_parser_t *p, const uint8_t *data, size_t len, uint32_t now_ms,
                        http_event_t *ev);

// Returns true if an incomplete request was dropped. No reply: the recipient is unknown.
bool http_parser_tick(http_parser_t *p, uint32_t now_ms);

// Drops an incomplete request, e.g. when the bridge reports a TCP disconnect.
void http_parser_reset(http_parser_t *p);

bool http_parser_busy(const http_parser_t *p);

const char *http_status_text(int status);

// Writes status line and headers. Returns length, or 0 if it does not fit.
// extra: additional header lines each ending in "\r\n", or NULL.
size_t http_format_head(char *buf, size_t cap, int status, const char *content_type,
                        size_t content_length, const char *extra);

#ifdef __cplusplus
}
#endif
