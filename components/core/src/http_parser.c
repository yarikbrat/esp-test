#include "http_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

enum {
    ST_IDLE = 0,  // between requests, skipping CR/LF
    ST_HEAD,      // collecting request line and headers
    ST_BODY,      // reading Content-Length bytes
    ST_SKIP_HEAD, // after an error: drop bytes up to the end of headers
    ST_SKIP_BODY, // after an error: drop a body of known length
};

void http_parser_init(http_parser_t *p)
{
    memset(p, 0, sizeof(*p));
}

void http_parser_reset(http_parser_t *p)
{
    p->state = ST_IDLE;
    p->head_len = 0;
    p->body_need = 0;
    p->tail = 0;
}

bool http_parser_busy(const http_parser_t *p)
{
    return p->state != ST_IDLE;
}

static bool head_complete(const char *h, size_t n)
{
    return (n >= 4 && memcmp(h + n - 4, "\r\n\r\n", 4) == 0) ||
           (n >= 2 && memcmp(h + n - 2, "\n\n", 2) == 0);
}

static bool name_is(const char *name, size_t len, const char *want)
{
    size_t wl = strlen(want);
    if (len != wl) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (tolower((unsigned char)name[i]) != tolower((unsigned char)want[i])) {
            return false;
        }
    }
    return true;
}

static bool contains_token_ci(const char *s, size_t len, const char *tok)
{
    size_t tl = strlen(tok);
    for (size_t i = 0; i + tl <= len; i++) {
        if (name_is(s + i, tl, tok)) {
            return true;
        }
    }
    return false;
}

// Header pass. Sets p->body_need as soon as Content-Length is seen, so a body can still be
// skipped when a later header line or the request line turns out to be invalid.
static int parse_headers(http_parser_t *p, char *line, char *end)
{
    bool have_len = false;
    while (line < end) {
        char *nl = memchr(line, '\n', (size_t)(end - line));
        if (!nl) {
            break;
        }
        size_t ll = (size_t)(nl - line);
        if (ll > 0 && line[ll - 1] == '\r') {
            ll--;
        }
        if (ll == 0) {
            break;
        }
        char *colon = memchr(line, ':', ll);
        if (!colon) {
            return 400;
        }
        size_t nlen = (size_t)(colon - line);
        char *val = colon + 1;
        char *val_end = line + ll;
        while (val < val_end && (*val == ' ' || *val == '\t')) {
            val++;
        }
        while (val_end > val && (val_end[-1] == ' ' || val_end[-1] == '\t')) {
            val_end--;
        }
        size_t vlen = (size_t)(val_end - val);

        if (name_is(line, nlen, "Content-Length")) {
            if (vlen == 0 || vlen > 9) {
                return 400;
            }
            size_t v = 0;
            for (size_t i = 0; i < vlen; i++) {
                if (val[i] < '0' || val[i] > '9') {
                    return 400;
                }
                v = v * 10 + (size_t)(val[i] - '0');
            }
            if (have_len && v != p->body_need) {
                p->body_need = 0; // conflicting lengths: no length is trustworthy
                return 400;
            }
            have_len = true;
            p->body_need = v;
        } else if (name_is(line, nlen, "Transfer-Encoding")) {
            return 501; // chunked request bodies are not supported
        } else if (name_is(line, nlen, "Accept-Encoding")) {
            p->req.accept_gzip = contains_token_ci(val, vlen, "gzip");
        } else if (name_is(line, nlen, "Host")) {
            char *colon2 = memchr(val, ':', vlen); // drop ":port"
            size_t hlen = colon2 ? (size_t)(colon2 - val) : vlen;
            if (hlen <= HTTP_MAX_HOST) {
                memcpy(p->req.host, val, hlen);
                p->req.host[hlen] = '\0';
            }
        }
        line = nl + 1;
    }
    return 0;
}

static int parse_request_line(http_request_t *r, char *h, size_t line_len)
{
    int pending = 0; // unknown method is reported after the rest of the line is checked
    char *sp1 = memchr(h, ' ', line_len);
    if (!sp1) {
        return 400;
    }
    size_t mlen = (size_t)(sp1 - h);
    if (mlen == 3 && memcmp(h, "GET", 3) == 0) {
        r->method = HTTP_GET;
    } else if (mlen == 4 && memcmp(h, "HEAD", 4) == 0) {
        r->method = HTTP_HEAD;
    } else if (mlen == 4 && memcmp(h, "POST", 4) == 0) {
        r->method = HTTP_POST;
    } else {
        pending = 501;
    }
    char *target = sp1 + 1;
    char *line_end = h + line_len;
    char *sp2 = memchr(target, ' ', (size_t)(line_end - target));
    if (!sp2 || target[0] != '/') {
        return 400;
    }
    char *ver = sp2 + 1;
    if ((size_t)(line_end - ver) != 8 || memcmp(ver, "HTTP/1.", 7) != 0) {
        return 505;
    }
    char *q = memchr(target, '?', (size_t)(sp2 - target));
    char *path_end = q ? q : sp2;
    size_t plen = (size_t)(path_end - target);
    if (plen > HTTP_MAX_PATH) {
        return 414;
    }
    memcpy(r->path, target, plen);
    if (q) {
        size_t qlen = (size_t)(sp2 - q - 1);
        if (qlen > HTTP_MAX_QUERY) {
            return 414;
        }
        memcpy(r->query, q + 1, qlen);
    }
    return pending;
}

// Parses the collected head. Returns 0 or an HTTP error status; p->body_need is the body length
// whenever it could be determined, even on error.
static int parse_head(http_parser_t *p)
{
    memset(&p->req, 0, sizeof(p->req));
    p->body_need = 0;
    char *h = p->head;
    char *end = h + p->head_len;
    char *eol = memchr(h, '\n', (size_t)(end - h));
    if (!eol) {
        return 400;
    }
    int hdr_err = parse_headers(p, eol + 1, end);
    size_t line_len = (size_t)(eol - h);
    if (line_len > 0 && h[line_len - 1] == '\r') {
        line_len--;
    }
    int line_err = parse_request_line(&p->req, h, line_len);
    if (line_err) {
        return line_err;
    }
    if (hdr_err) {
        return hdr_err;
    }
    if (p->body_need > HTTP_MAX_BODY) {
        return 413;
    }
    return 0;
}

static void fail(http_parser_t *p, int status, http_event_t *ev)
{
    p->error_status = status;
    p->stats.errors++;
    *ev = HTTP_EV_ERROR;
}

size_t http_parser_feed(http_parser_t *p, const uint8_t *data, size_t len, uint32_t now_ms,
                        http_event_t *ev)
{
    *ev = HTTP_EV_NONE;
    size_t i = 0;
    if (len > 0) {
        p->last_ms = now_ms;
    }
    while (i < len) {
        uint8_t c = data[i];
        switch (p->state) {
        case ST_IDLE:
            if (c == '\r' || c == '\n') {
                i++;
                break;
            }
            p->state = ST_HEAD;
            p->head_len = 0;
            p->start_ms = now_ms;
            /* fallthrough */
        case ST_HEAD:
            if (p->head_len >= sizeof(p->head)) {
                p->state = ST_SKIP_HEAD;
                p->start_ms = now_ms;
                // Seed with the buffered tail: the terminator may straddle the overflow point.
                p->tail = ((uint32_t)(uint8_t)p->head[p->head_len - 3] << 16) |
                          ((uint32_t)(uint8_t)p->head[p->head_len - 2] << 8) |
                          (uint8_t)p->head[p->head_len - 1];
                p->error_head = memcmp(p->head, "HEAD ", 5) == 0;
                fail(p, 431, ev);
                return i;
            }
            p->head[p->head_len++] = (char)c;
            i++;
            if (head_complete(p->head, p->head_len)) {
                int status = parse_head(p);
                if (status != 0) {
                    // Skip a body of known, modest length so it is not parsed as the next request.
                    // A huge claimed length is not honoured: skipping it would swallow other
                    // clients' requests on the merged stream until the total timeout.
                    size_t skip = p->body_need;
                    p->error_head = p->head_len >= 5 && memcmp(p->head, "HEAD ", 5) == 0;
                    http_parser_reset(p);
                    if (skip > 0 && skip <= HTTP_MAX_SKIP) {
                        p->state = ST_SKIP_BODY;
                        p->body_need = skip;
                    }
                    fail(p, status, ev);
                    return i;
                }
                if (p->body_need == 0) {
                    p->state = ST_IDLE;
                    p->stats.requests++;
                    *ev = HTTP_EV_REQUEST;
                    return i;
                }
                p->state = ST_BODY;
            }
            break;
        case ST_BODY: {
            size_t n = len - i;
            if (n > p->body_need) {
                n = p->body_need;
            }
            memcpy(p->req.body + p->req.body_len, data + i, n);
            p->req.body_len += n;
            p->body_need -= n;
            i += n;
            if (p->body_need == 0) {
                p->req.body[p->req.body_len] = '\0';
                p->state = ST_IDLE;
                p->stats.requests++;
                *ev = HTTP_EV_REQUEST;
                return i;
            }
            break;
        }
        case ST_SKIP_HEAD:
            p->tail = (p->tail << 8) | c;
            p->stats.skipped_bytes++;
            i++;
            if (p->tail == 0x0D0A0D0Au || (p->tail & 0xFFFFu) == 0x0A0Au) {
                http_parser_reset(p);
            }
            break;
        case ST_SKIP_BODY: {
            size_t n = len - i;
            if (n > p->body_need) {
                n = p->body_need;
            }
            p->body_need -= n;
            p->stats.skipped_bytes += (uint32_t)n;
            i += n;
            if (p->body_need == 0) {
                http_parser_reset(p);
            }
            break;
        }
        }
    }
    return i;
}

bool http_parser_tick(http_parser_t *p, uint32_t now_ms)
{
    if (p->state == ST_IDLE) {
        return false;
    }
    if (now_ms - p->last_ms > HTTP_IDLE_TIMEOUT_MS || now_ms - p->start_ms > HTTP_TOTAL_TIMEOUT_MS) {
        p->stats.timeouts++;
        http_parser_reset(p);
        return true;
    }
    return false;
}

const char *http_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 422: return "Unprocessable Entity";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    }
    return "Unknown";
}

size_t http_format_head(char *buf, size_t cap, int status, const char *content_type,
                        size_t content_length, const char *extra)
{
    // Content-Length is mandatory: the ESP32 cannot close the TCP connection to mark the end.
    int n = snprintf(buf, cap,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %u\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: keep-alive\r\n"
                     "%s\r\n",
                     status, http_status_text(status), content_type, (unsigned)content_length,
                     extra ? extra : "");
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    return (size_t)n;
}
