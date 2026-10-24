#include "check.h"
#include "form.h"
#include "http_parser.h"
#include "netcfg.h"

#include <string.h>

static uint32_t ip(const char *s)
{
    uint32_t a = 0;
    CHECK(netcfg_parse_ipv4(s, &a));
    return a;
}

void test_netcfg(void)
{
    uint32_t a;
    CHECK(netcfg_parse_ipv4("192.168.144.36", &a) && a == NETCFG_DEFAULT_IP);
    CHECK(netcfg_parse_ipv4("0.0.0.0", &a) && a == 0);
    CHECK(netcfg_parse_ipv4("255.255.255.255", &a) && a == 0xFFFFFFFFu);
    const char *bad[] = {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.-4", " 1.2.3.4",
                         "1.2.3.4 ", "1..3.4", "01.2.3.4", "1.2.3.4a", "1234.1.1.1", "a.b.c.d",
                         "1.2.3.", ".1.2.3"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        a = 0x12345678u;
        CHECK(!netcfg_parse_ipv4(bad[i], &a));
        CHECK(a == 0x12345678u);
    }
    CHECK(!netcfg_parse_ipv4(NULL, &a));

    char buf[16];
    netcfg_format_ipv4(NETCFG_DEFAULT_IP, buf, sizeof(buf));
    CHECK(strcmp(buf, "192.168.144.36") == 0);

    netcfg_t c;
    netcfg_default(&c);
    CHECK_EQ_INT(netcfg_validate(&c), NETCFG_OK);

    netcfg_t out = {1, 2, 3};
    CHECK_EQ_INT(netcfg_from_strings("10.0.0.5", "255.0.0.0", "10.0.0.1", &out), NETCFG_OK);
    CHECK(out.ip == ip("10.0.0.5") && out.gw == ip("10.0.0.1"));
    CHECK_EQ_INT(netcfg_from_strings("192.168.1.10", "255.255.255.0", "", &out), NETCFG_OK);
    CHECK(out.gw == 0);

    // Errors leave out untouched.
    netcfg_t keep = out;
    CHECK_EQ_INT(netcfg_from_strings("192.168.1.300", "255.255.255.0", "", &out), NETCFG_ERR_IP_FORMAT);
    CHECK_EQ_INT(netcfg_from_strings("192.168.1.10", "255.255.0.255", "", &out), NETCFG_ERR_MASK_INVALID);
    CHECK_EQ_INT(netcfg_from_strings("192.168.1.10", "255.255.255.255", "", &out), NETCFG_ERR_MASK_INVALID);
    CHECK_EQ_INT(netcfg_from_strings("192.168.1.10", "255.255.255.254", "", &out), NETCFG_ERR_MASK_INVALID);
    CHECK_EQ_INT(netcfg_from_strings("192.168.1.10", "0.0.0.0", "", &out), NETCFG_ERR_MASK_INVALID);
    CHECK_EQ_INT(netcfg_from_strings("192.168.1.10", "255.255.255.0", "x", &out), NETCFG_ERR_GW_FORMAT);
    CHECK_EQ_INT(netcfg_from_strings("192.168.1.0", "255.255.255.0", "", &out), NETCFG_ERR_IP_HOST_PART);
    CHECK_EQ_INT(netcfg_from_strings("192.168.1.255", "255.255.255.0", "", &out), NETCFG_ERR_IP_HOST_PART);
    CHECK_EQ_INT(netcfg_from_strings("127.0.0.1", "255.0.0.0", "", &out), NETCFG_ERR_IP_RESERVED);
    CHECK_EQ_INT(netcfg_from_strings("224.0.0.1", "255.255.255.0", "", &out), NETCFG_ERR_IP_RESERVED);
    CHECK_EQ_INT(netcfg_from_strings("0.1.2.3", "255.255.255.0", "", &out), NETCFG_ERR_IP_RESERVED);
    CHECK_EQ_INT(netcfg_from_strings("169.254.1.1", "255.255.0.0", "", &out), NETCFG_ERR_IP_RESERVED);
    CHECK_EQ_INT(netcfg_from_strings("192.168.1.10", "255.255.255.0", "192.168.2.1", &out), NETCFG_ERR_GW_SUBNET);
    CHECK_EQ_INT(netcfg_from_strings("192.168.1.10", "255.255.255.0", "192.168.1.10", &out), NETCFG_ERR_GW_INVALID);
    CHECK_EQ_INT(netcfg_from_strings("192.168.1.10", "255.255.255.0", "192.168.1.255", &out), NETCFG_ERR_GW_INVALID);
    CHECK(memcmp(&out, &keep, sizeof(out)) == 0);
    CHECK(strlen(netcfg_err_code(NETCFG_ERR_GW_SUBNET)) > 0);
    CHECK(strcmp(netcfg_err_code(NETCFG_ERR_GW_SUBNET), "gw_subnet") == 0);
    for (int e = NETCFG_OK; e <= NETCFG_ERR_GW_INVALID; e++) {
        CHECK(strcmp(netcfg_err_text((netcfg_err_t)e), "Невідома помилка") != 0);
    }
}

// Feeds s in chunk-sized pieces and collects events.
typedef struct {
    int requests, errors;
    int last_status;
    http_request_t last;
} feed_result_t;

static feed_result_t feed_str(http_parser_t *p, const char *s, size_t chunk, uint32_t now)
{
    feed_result_t fr = {0};
    size_t len = strlen(s), off = 0;
    while (off < len) {
        size_t n = len - off < chunk ? len - off : chunk;
        size_t used = 0;
        while (used < n) {
            http_event_t ev;
            used += http_parser_feed(p, (const uint8_t *)s + off + used, n - used, now, &ev);
            if (ev == HTTP_EV_REQUEST) {
                fr.requests++;
                fr.last = p->req;
            } else if (ev == HTTP_EV_ERROR) {
                fr.errors++;
                fr.last_status = p->error_status;
            }
        }
        off += n;
    }
    return fr;
}

void test_http(void)
{
    http_parser_t p;
    const char *get = "GET /api/state?x=1 HTTP/1.1\r\nHost: 192.168.144.36\r\n"
                      "Accept-Encoding: gzip, deflate\r\n\r\n";

    // Whole, byte-by-byte and 7-byte chunks must give the same result.
    size_t chunks[] = {4096, 1, 7};
    for (size_t i = 0; i < 3; i++) {
        http_parser_init(&p);
        feed_result_t fr = feed_str(&p, get, chunks[i], 0);
        CHECK_EQ_INT(fr.requests, 1);
        CHECK_EQ_INT(fr.errors, 0);
        CHECK(fr.last.method == HTTP_GET);
        CHECK(strcmp(fr.last.path, "/api/state") == 0);
        CHECK(strcmp(fr.last.query, "x=1") == 0);
        CHECK(fr.last.accept_gzip);
        CHECK(strcmp(fr.last.host, "192.168.144.36") == 0);
        CHECK(!http_parser_busy(&p));
    }

    // Host: port is dropped, an absent or oversized Host gives "", and it never leaks into the next request.
    http_parser_init(&p);
    feed_result_t fh = feed_str(&p, "GET / HTTP/1.1\r\nhost:  10.0.0.5:8080 \r\n\r\n", 4096, 0);
    CHECK(fh.requests == 1 && strcmp(fh.last.host, "10.0.0.5") == 0);
    fh = feed_str(&p, "GET / HTTP/1.1\r\n\r\n", 4096, 0);
    CHECK(fh.requests == 1 && fh.last.host[0] == '\0');
    char longhost[200] = "GET / HTTP/1.1\r\nHost: ";
    memset(longhost + strlen(longhost), 'a', HTTP_MAX_HOST + 1);
    strcat(longhost, "\r\n\r\n");
    fh = feed_str(&p, longhost, 4096, 0);
    CHECK(fh.requests == 1 && fh.last.host[0] == '\0');

    // X-Req-Id is echoed into a response header: only short alphanumeric values are kept.
    fh = feed_str(&p, "GET / HTTP/1.1\r\nx-req-id: 42abc\r\n\r\n", 4096, 0);
    CHECK(fh.requests == 1 && strcmp(fh.last.rid, "42abc") == 0);
    fh = feed_str(&p, "GET / HTTP/1.1\r\nX-Req-Id: 4 2\r\n\r\n", 4096, 0);
    CHECK(fh.requests == 1 && fh.last.rid[0] == '\0');
    fh = feed_str(&p, "GET / HTTP/1.1\r\nX-Req-Id: 12345678901234567\r\n\r\n", 4096, 0);
    CHECK(fh.requests == 1 && fh.last.rid[0] == '\0');
    fh = feed_str(&p, "GET / HTTP/1.1\r\n\r\n", 4096, 0);
    CHECK(fh.requests == 1 && fh.last.rid[0] == '\0');

    // Two pipelined requests in one chunk, then a POST whose body is split.
    http_parser_init(&p);
    feed_result_t fr = feed_str(&p,
                                "GET / HTTP/1.1\r\n\r\n"
                                "POST /api/zero HTTP/1.1\r\ncontent-length: 5\r\n\r\nch=1&",
                                4096, 0);
    CHECK_EQ_INT(fr.requests, 2);
    CHECK(fr.last.method == HTTP_POST);
    CHECK_EQ_INT(fr.last.body_len, 5);
    CHECK(strcmp(fr.last.body, "ch=1&") == 0);
    CHECK(!fr.last.accept_gzip);

    http_parser_init(&p);
    fr = feed_str(&p, "POST /api/net HTTP/1.1\r\nContent-Length: 10\r\n\r\nip=1.", 4096, 0);
    CHECK_EQ_INT(fr.requests, 0);
    CHECK(http_parser_busy(&p));
    fr = feed_str(&p, "2.3.4", 4096, 100);
    CHECK_EQ_INT(fr.requests, 1);
    CHECK(strcmp(fr.last.body, "ip=1.2.3.4") == 0);

    // Junk between requests: one error reply, then the next request parses.
    http_parser_init(&p);
    fr = feed_str(&p, "\r\n\r\n\x01\xffgarbage\r\n\r\nGET /ok HTTP/1.1\r\n\r\n", 3, 0);
    CHECK_EQ_INT(fr.errors, 1);
    CHECK_EQ_INT(fr.last_status, 400);
    CHECK_EQ_INT(fr.requests, 1);
    CHECK(strcmp(fr.last.path, "/ok") == 0);

    // Header overflow -> 431, skip to end of headers, then continue normally.
    http_parser_init(&p);
    char big[4000];
    strcpy(big, "GET / HTTP/1.1\r\nX-Pad: ");
    size_t l = strlen(big);
    memset(big + l, 'a', 2500);
    strcpy(big + l + 2500, "\r\n\r\nGET /next HTTP/1.1\r\n\r\n");
    fr = feed_str(&p, big, 64, 0);
    CHECK_EQ_INT(fr.errors, 1);
    CHECK_EQ_INT(fr.last_status, 431);
    CHECK_EQ_INT(fr.requests, 1);
    CHECK(strcmp(fr.last.path, "/next") == 0);

    // Boundary: a head of exactly HTTP_MAX_HEAD bytes is accepted.
    http_parser_init(&p);
    memset(big, 0, sizeof(big));
    strcpy(big, "GET / HTTP/1.1\r\nX-Pad: ");
    l = strlen(big);
    size_t pad = HTTP_MAX_HEAD - l - 4;
    memset(big + l, 'b', pad);
    strcpy(big + l + pad, "\r\n\r\n");
    CHECK_EQ_INT(strlen(big), HTTP_MAX_HEAD);
    fr = feed_str(&p, big, 4096, 0);
    CHECK_EQ_INT(fr.errors, 0);
    CHECK_EQ_INT(fr.requests, 1);

    // Oversized body -> 413, body skipped, next request parses.
    http_parser_init(&p);
    memset(big, 0, sizeof(big));
    strcpy(big, "POST /api/net HTTP/1.1\r\nContent-Length: 600\r\n\r\n");
    l = strlen(big);
    memset(big + l, 'z', 600);
    strcpy(big + l + 600, "GET /after HTTP/1.1\r\n\r\n");
    fr = feed_str(&p, big, 50, 0);
    CHECK_EQ_INT(fr.errors, 1);
    CHECK_EQ_INT(fr.last_status, 413);
    CHECK_EQ_INT(fr.requests, 1);
    CHECK(strcmp(fr.last.path, "/after") == 0);

    // Unknown method with a body: 501, body is not parsed as a request.
    http_parser_init(&p);
    fr = feed_str(&p, "PUT /x HTTP/1.1\r\nContent-Length: 18\r\n\r\nGET /evil HTTP/1.1"
                      "GET /good HTTP/1.1\r\n\r\n", 4096, 0);
    CHECK_EQ_INT(fr.errors, 1);
    CHECK_EQ_INT(fr.last_status, 501);
    CHECK_EQ_INT(fr.requests, 1);
    CHECK(strcmp(fr.last.path, "/good") == 0);

    // Request-line errors still skip a declared body: it must not be parsed as the next request.
    http_parser_init(&p);
    fr = feed_str(&p, "POST /aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                      " HTTP/1.1\r\nContent-Length: 30\r\n\r\nGET /api/log/clear HTTP/1.1\r\n\r\n"
                      "GET /real HTTP/1.1\r\n\r\n", 4096, 0);
    CHECK_EQ_INT(fr.last_status, 414);
    CHECK_EQ_INT(fr.errors, 1);
    CHECK_EQ_INT(fr.requests, 1);
    CHECK(strcmp(fr.last.path, "/real") == 0);
    http_parser_init(&p);
    fr = feed_str(&p, "POST /x HTTP/2.0\r\nContent-Length: 6\r\n\r\nGET / \r\nGET /real HTTP/1.1\r\n\r\n", 4096, 0);
    CHECK_EQ_INT(fr.last_status, 505);
    CHECK_EQ_INT(fr.requests, 1);
    CHECK(strcmp(fr.last.path, "/real") == 0);
    http_parser_init(&p);
    fr = feed_str(&p, "POST /x HTTP/1.1\r\nContent-Length: 6\r\nbroken line\r\n\r\nGET / GET /real HTTP/1.1\r\n\r\n",
                  4096, 0);
    CHECK_EQ_INT(fr.last_status, 400);
    CHECK_EQ_INT(fr.requests, 1);
    CHECK(strcmp(fr.last.path, "/real") == 0);

    // A huge claimed length is rejected without swallowing the following requests.
    http_parser_init(&p);
    fr = feed_str(&p, "POST /api/net HTTP/1.1\r\nContent-Length: 999999999\r\n\r\n"
                      "GET /api/state HTTP/1.1\r\n\r\n", 4096, 0);
    CHECK_EQ_INT(fr.last_status, 413);
    CHECK_EQ_INT(fr.requests, 1);
    CHECK(strcmp(fr.last.path, "/api/state") == 0);

    // HEAD is remembered for error replies, GET is not.
    http_parser_init(&p);
    feed_str(&p, "HEAD /x HTTP/9.9\r\n\r\n", 4096, 0);
    CHECK(p.error_head);
    feed_str(&p, "GET /x HTTP/9.9\r\n\r\n", 4096, 0);
    CHECK(!p.error_head);

    // Conflicting Content-Length, chunked, bad version, bad length, long URI.
    http_parser_init(&p);
    fr = feed_str(&p, "POST / HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\n", 4096, 0);
    CHECK_EQ_INT(fr.last_status, 400);
    http_parser_init(&p);
    fr = feed_str(&p, "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", 4096, 0);
    CHECK_EQ_INT(fr.last_status, 501);
    http_parser_init(&p);
    fr = feed_str(&p, "GET / HTTP/2.0\r\n\r\n", 4096, 0);
    CHECK_EQ_INT(fr.last_status, 505);
    http_parser_init(&p);
    fr = feed_str(&p, "GET / HTTP/1.1\r\nContent-Length: 1x\r\n\r\n", 4096, 0);
    CHECK_EQ_INT(fr.last_status, 400);
    http_parser_init(&p);
    fr = feed_str(&p, "GET /aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa HTTP/1.1\r\n\r\n", 4096, 0);
    CHECK_EQ_INT(fr.last_status, 414);

    // Idle timeout: a gap > 2 s drops the incomplete request.
    http_parser_init(&p);
    feed_str(&p, "GET /slow HT", 4096, 1000);
    CHECK(http_parser_busy(&p));
    CHECK(!http_parser_tick(&p, 3000));
    CHECK(http_parser_tick(&p, 3001));
    CHECK(!http_parser_busy(&p));
    CHECK_EQ_INT(p.stats.timeouts, 1);
    fr = feed_str(&p, "GET /fresh HTTP/1.1\r\n\r\n", 4096, 3100);
    CHECK_EQ_INT(fr.requests, 1);
    CHECK(strcmp(fr.last.path, "/fresh") == 0);

    // Total timeout: bytes keep dripping but the request never completes within 5 s.
    http_parser_init(&p);
    feed_str(&p, "GET /drip", 4096, 0);
    uint32_t dropped_at = 0;
    for (uint32_t t = 1000; t <= 8000 && !dropped_at; t += 1000) {
        feed_str(&p, "a", 1, t);
        if (http_parser_tick(&p, t)) {
            dropped_at = t;
        }
    }
    CHECK_EQ_INT(dropped_at, 6000); // first tick past 5 s

    // Response head.
    char head[256];
    size_t n = http_format_head(head, sizeof(head), 200, "application/json", 17, "X-A: 1\r\n");
    CHECK(n > 0);
    CHECK(strstr(head, "HTTP/1.1 200 OK\r\n") == head);
    CHECK(strstr(head, "Content-Length: 17\r\n") != NULL);
    CHECK(strstr(head, "X-A: 1\r\n\r\n") != NULL);
    CHECK_EQ_INT(http_format_head(head, 20, 200, "text/html", 1, NULL), 0);
}

void test_form(void)
{
    char v[32];
    const char *body = "ip=192.168.1.5&mask=255.255.255.0&gw=&name=a+b%21&ipx=9";
    CHECK(form_get(body, "ip", v, sizeof(v)) && strcmp(v, "192.168.1.5") == 0);
    CHECK(form_get(body, "gw", v, sizeof(v)) && v[0] == '\0');
    CHECK(form_get(body, "name", v, sizeof(v)) && strcmp(v, "a b!") == 0);
    CHECK(form_get(body, "ipx", v, sizeof(v)) && strcmp(v, "9") == 0);
    CHECK(!form_get(body, "i", v, sizeof(v)));
    CHECK(!form_get(body, "missing", v, sizeof(v)));
    CHECK(!form_get("a=%zz", "a", v, sizeof(v)));
    CHECK(!form_get("a=%4", "a", v, sizeof(v)));
    CHECK(!form_get("a=%00", "a", v, sizeof(v)));
    CHECK(!form_get("a=123456", "a", v, 4)); // does not fit
    CHECK(form_get("flag", "flag", v, sizeof(v)) && v[0] == '\0');

    long n = 0;
    CHECK(form_get_long("interval=10", "interval", 1, 3600, &n) && n == 10);
    CHECK(!form_get_long("interval=0", "interval", 1, 3600, &n));
    CHECK(!form_get_long("interval=3601", "interval", 1, 3600, &n));
    CHECK(!form_get_long("interval=10s", "interval", 1, 3600, &n));
    CHECK(!form_get_long("interval=", "interval", 1, 3600, &n));
    CHECK(!form_get_long("interval=99999999999999999999", "interval", 1, 3600, &n));
    CHECK(n == 10);
}

// Deterministic fuzz under ASan: random bytes mixed with valid requests, random chunking.
void test_http_fuzz(void)
{
    static const char *frags[] = {"GET / HTTP/1.1\r\n\r\n", "POST /api/zero HTTP/1.1\r\nContent-Length: 4\r\n\r\nch=1",
                                  "\r\n", "Content-Length: 99999\r\n", "\n\n", "HTTP/1.", "%00&=",
                                  "POST / HTTP/1.1\r\nContent-Length: 600\r\n\r\n"};
    http_parser_t p;
    http_parser_init(&p);
    uint32_t rng = 12345, now = 0;
    int requests = 0;
    uint8_t buf[256];
    for (int iter = 0; iter < 200000; iter++) {
        rng = rng * 1103515245u + 12345u;
        size_t n;
        if ((rng >> 16) % 3 == 0) {
            const char *f = frags[(rng >> 8) % (sizeof(frags) / sizeof(frags[0]))];
            n = strlen(f);
            memcpy(buf, f, n);
        } else {
            n = 1 + (rng >> 20) % 64;
            for (size_t i = 0; i < n; i++) {
                rng = rng * 1103515245u + 12345u;
                buf[i] = (uint8_t)(rng >> 24);
            }
        }
        now += (rng >> 12) % 400;
        size_t off = 0;
        while (off < n) {
            http_event_t ev;
            size_t used = http_parser_feed(&p, buf + off, n - off, now, &ev);
            if (ev == HTTP_EV_REQUEST) {
                requests++;
                if (p.req.body_len > HTTP_MAX_BODY || strlen(p.req.path) > HTTP_MAX_PATH) {
                    g_failures++;
                }
            }
            if (used == 0 && ev == HTTP_EV_NONE) {
                g_failures++; // no progress would hang the net task
                break;
            }
            off += used;
        }
        http_parser_tick(&p, now);
    }
    g_checks++;
    CHECK(requests > 0);
}
