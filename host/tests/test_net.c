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

    // Помилки не змінюють out.
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

// Подає s шматками розміру chunk і збирає події.
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

    // Цілком, по байту і шматками по 7 байтів — результат має бути однаковим.
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

    // Host: порт відкидається, відсутній чи задовгий Host дає "", і він ніколи не потрапляє в наступний запит.
    http_parser_init(&p);
    feed_result_t fh = feed_str(&p, "GET / HTTP/1.1\r\nhost:  10.0.0.5:8080 \r\n\r\n", 4096, 0);
    CHECK(fh.requests == 1 && strcmp(fh.last.host, "10.0.0.5") == 0);
    fh = feed_str(&p, "GET / HTTP/1.1\r\n\r\n", 4096, 0);
    CHECK(fh.requests == 1 && fh.last.host[0] == '\0');
    fh = feed_str(&p, "GET / HTTP/1.1\r\nHost: [fe80::1]:80\r\n\r\n", 4096, 0);
    CHECK(fh.requests == 1 && fh.last.host[0] == '\0'); // літерал IPv6 не обрізається до "["
    fh = feed_str(&p, "GET / HTTP/1.1\r\nHost: 10.0.0.1\r\nHost: 10.0.0.2\r\n\r\n", 4096, 0);
    CHECK(fh.requests == 1 && strcmp(fh.last.host, "10.0.0.1") == 0);
    char longhost[200] = "GET / HTTP/1.1\r\nHost: ";
    memset(longhost + strlen(longhost), 'a', HTTP_MAX_HOST + 1);
    strcat(longhost, "\r\n\r\n");
    fh = feed_str(&p, longhost, 4096, 0);
    CHECK(fh.requests == 1 && fh.last.host[0] == '\0');

    // Відхилений запит лишає свої перші байти для журналу, хоча парсер скинув свій буфер.
    fh = feed_str(&p, "BREW /pot HTTP/1.1\r\n\r\n", 4096, 0);
    CHECK(fh.errors == 1 && p.drop_len > 0 && memcmp(p.drop, "BREW /pot", 9) == 0 && p.head_len == 0);
    http_parser_init(&p);
    fh = feed_str(&p, "GET /slow HT", 4096, 0);
    CHECK(http_parser_tick(&p, HTTP_IDLE_TIMEOUT_MS + 1) && memcmp(p.drop, "GET /slow HT", 12) == 0);

    // X-Req-Id повертається в заголовку відповіді: зберігаються лише короткі буквено-цифрові значення.
    fh = feed_str(&p, "GET / HTTP/1.1\r\nx-req-id: 42abc\r\n\r\n", 4096, 0);
    CHECK(fh.requests == 1 && strcmp(fh.last.rid, "42abc") == 0);
    fh = feed_str(&p, "GET / HTTP/1.1\r\nX-Req-Id: 4 2\r\n\r\n", 4096, 0);
    CHECK(fh.requests == 1 && fh.last.rid[0] == '\0');
    fh = feed_str(&p, "GET / HTTP/1.1\r\nX-Req-Id: 12345678901234567\r\n\r\n", 4096, 0);
    CHECK(fh.requests == 1 && fh.last.rid[0] == '\0');
    fh = feed_str(&p, "GET / HTTP/1.1\r\n\r\n", 4096, 0);
    CHECK(fh.requests == 1 && fh.last.rid[0] == '\0');
    fh = feed_str(&p, "GET / HTTP/1.1\r\nX-Client-Id: k9Q\r\n\r\n", 4096, 0);
    CHECK(fh.requests == 1 && strcmp(fh.last.client, "k9Q") == 0 && fh.last.rid[0] == '\0');

    // Два конвеєрні запити в одному шматку, потім POST із розділеним тілом.
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

    // Сміття, яким не може починатися метод, тихо пропускається між запитами.
    http_parser_init(&p);
    fr = feed_str(&p, "\r\n\r\n\x01\xffgarbage\r\n\r\nGET /ok HTTP/1.1\r\n\r\n", 3, 0);
    CHECK_EQ_INT(fr.errors, 0);
    CHECK_EQ_INT(fr.requests, 1);
    CHECK(strcmp(fr.last.path, "/ok") == 0);
    CHECK(p.stats.skipped_bytes == 9);

    // Сміття у верхньому регістрі схоже на метод: одна відповідь з помилкою, потім наступний запит розбирається.
    http_parser_init(&p);
    fr = feed_str(&p, "GARBAGE\r\n\r\nGET /ok HTTP/1.1\r\n\r\n", 3, 0);
    CHECK_EQ_INT(fr.errors, 1);
    CHECK_EQ_INT(fr.last_status, 400);
    CHECK_EQ_INT(fr.requests, 1);
    CHECK(strcmp(fr.last.path, "/ok") == 0);

    // Шум з байтами A-Z, приліплений перед рядком запиту: запит відновлюється з рядка.
    http_parser_init(&p);
    fr = feed_str(&p, "\x01QZ\xff" "AGET /api/state HTTP/1.1\r\nX-Req-Id: 5\r\n\r\n", 4096, 0);
    CHECK(fr.requests == 1 && fr.errors == 0 && strcmp(fr.last.path, "/api/state") == 0);
    CHECK(strcmp(fr.last.rid, "5") == 0);
    // Шум із символом нового рядка: справжній запит у другому рядку буфера.
    http_parser_init(&p);
    fr = feed_str(&p, "\x02ZQ\nXGET /api/state HTTP/1.1\r\n\r\n", 4096, 0);
    CHECK(fr.requests == 1 && fr.errors == 0 && strcmp(fr.last.path, "/api/state") == 0);
    // Валідний запит зберігає свій рядок, навіть якщо заголовок згадує метод.
    fr = feed_str(&p, "GET /a HTTP/1.1\r\nX-Note: POST /b HTTP/1.1\r\n\r\n", 4096, 0);
    CHECK(fr.requests == 1 && strcmp(fr.last.path, "/a") == 0);
    // Тіло, яке так і не приходить: тайм-аут зберігає початок прийнятого запиту для журналу.
    http_parser_init(&p);
    fr = feed_str(&p, "POST /api/net HTTP/1.1\r\nContent-Length: 30\r\n\r\nip=1", 4096, 0);
    CHECK(http_parser_tick(&p, HTTP_IDLE_TIMEOUT_MS + 1) && p.drop_len > 0 && memcmp(p.drop, "POST /api/net", 13) == 0);

    // Тайм-аут під час пропуску тіла відхиленого запиту не перезаписує фрагмент цього запиту сміттям.
    http_parser_init(&p);
    fr = feed_str(&p, "BREW /x HTTP/1.1\r\nContent-Length: 10\r\n\r\nab", 4096, 0);
    CHECK(fr.errors == 1 && p.drop_len > 0);
    CHECK(http_parser_tick(&p, HTTP_IDLE_TIMEOUT_MS + 1) && p.drop_len == 0);
    // 431: заголовки не розібрано, тож id попереднього запиту не можна повертати.
    http_parser_init(&p);
    fr = feed_str(&p, "GET / HTTP/1.1\r\nX-Req-Id: 7\r\n\r\n", 4096, 0);
    CHECK(strcmp(p.req.rid, "7") == 0);
    char huge[HTTP_MAX_HEAD + 64] = "GET / HTTP/1.1\r\nX-Pad: ";
    memset(huge + strlen(huge), 'b', HTTP_MAX_HEAD);
    huge[sizeof(huge) - 1] = '\0';
    fr = feed_str(&p, huge, 4096, 0);
    CHECK(fr.errors == 1 && fr.last_status == 431 && p.req.rid[0] == '\0');

    // Чистий невідомий метод все одно повідомляється, а не вважається шумом.
    http_parser_init(&p);
    fr = feed_str(&p, "PATCH /x HTTP/1.1\r\n\r\n", 4096, 0);
    CHECK(fr.errors == 1 && fr.last_status == 501);

    // 32 байти, які UART TO ETH (B) пише в UART після застосування нових налаштувань, приліплені до запиту.
    static const uint8_t module_junk[] = {0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0xfe, 0x00, 0xfe, 0x00,
                                          0x00, 0xff, 0x00, 0x00, 0x40, 0xff, 0x00, 0x00, 0xa0, 0xfd, 0x01,
                                          0x00, 0x00, 0x08, 0xa9, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00};
    http_parser_init(&p);
    http_event_t ev;
    size_t used = 0;
    while (used < sizeof(module_junk)) {
        used += http_parser_feed(&p, module_junk + used, sizeof(module_junk) - used, 0, &ev);
        CHECK(ev == HTTP_EV_NONE);
    }
    fr = feed_str(&p, "GET /api/state HTTP/1.1\r\n\r\n", 4096, 0);
    CHECK(fr.requests == 1 && fr.errors == 0 && strcmp(fr.last.path, "/api/state") == 0);

    // Переповнення заголовків -> 431, пропуск до кінця заголовків, далі звичайна робота.
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

    // Межа: заголовкова частина рівно HTTP_MAX_HEAD байтів приймається.
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

    // Завелике тіло -> 413, тіло пропускається, наступний запит розбирається.
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

    // Невідомий метод із тілом: 501, тіло не розбирається як запит.
    http_parser_init(&p);
    fr = feed_str(&p, "PUT /x HTTP/1.1\r\nContent-Length: 18\r\n\r\nGET /evil HTTP/1.1"
                      "GET /good HTTP/1.1\r\n\r\n", 4096, 0);
    CHECK_EQ_INT(fr.errors, 1);
    CHECK_EQ_INT(fr.last_status, 501);
    CHECK_EQ_INT(fr.requests, 1);
    CHECK(strcmp(fr.last.path, "/good") == 0);

    // Помилки в рядку запиту все одно пропускають оголошене тіло: його не можна розбирати як наступний запит.
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

    // Величезна заявлена довжина відхиляється без поглинання наступних запитів.
    http_parser_init(&p);
    fr = feed_str(&p, "POST /api/net HTTP/1.1\r\nContent-Length: 999999999\r\n\r\n"
                      "GET /api/state HTTP/1.1\r\n\r\n", 4096, 0);
    CHECK_EQ_INT(fr.last_status, 413);
    CHECK_EQ_INT(fr.requests, 1);
    CHECK(strcmp(fr.last.path, "/api/state") == 0);

    // HEAD запам'ятовується для відповідей з помилкою, GET — ні.
    http_parser_init(&p);
    feed_str(&p, "HEAD /x HTTP/9.9\r\n\r\n", 4096, 0);
    CHECK(p.error_head);
    feed_str(&p, "GET /x HTTP/9.9\r\n\r\n", 4096, 0);
    CHECK(!p.error_head);

    // Суперечливий Content-Length, chunked, погана версія, погана довжина, задовгий URI.
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

    // Тайм-аут простою: пауза > 2 с відкидає неповний запит.
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

    // Загальний тайм-аут: байти продовжують крапати, але запит не завершується за 5 с.
    http_parser_init(&p);
    feed_str(&p, "GET /drip", 4096, 0);
    uint32_t dropped_at = 0;
    for (uint32_t t = 1000; t <= 8000 && !dropped_at; t += 1000) {
        feed_str(&p, "a", 1, t);
        if (http_parser_tick(&p, t)) {
            dropped_at = t;
        }
    }
    CHECK_EQ_INT(dropped_at, 6000); // перший тік після 5 с

    // Заголовкова частина відповіді.
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
    CHECK(!form_get("a=123456", "a", v, 4)); // не влазить
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

// Детермінований фазинг під ASan: випадкові байти впереміш із валідними запитами, випадкове дроблення.
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
                g_failures++; // відсутність прогресу повісила б мережеву задачу
                break;
            }
            off += used;
        }
        http_parser_tick(&p, now);
    }
    g_checks++;
    CHECK(requests > 0);
}
