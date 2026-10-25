#pragma once
// Потоковий парсер запитів HTTP/1.x для потоку байтів UART.
// Міст UART-Ethernet прозорий: меж TCP немає, запити можуть приходити частинами,
// а між ними може бути сміття. Обмежені буфери, тайм-аути і самосинхронізація після помилок.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HTTP_MAX_HEAD         1536 // рядок запиту + заголовки
#define HTTP_MAX_BODY         512
#define HTTP_MAX_SKIP         2048 // найбільше відхилене тіло, яке пропускаємо замість ресинхронізації
#define HTTP_MAX_PATH         96
#define HTTP_MAX_QUERY        96
#define HTTP_MAX_HOST         64
#define HTTP_MAX_RID          16
#define HTTP_DROP_SNIPPET     48 // байти відхиленого чи простроченого запиту, що зберігаються для логу
#define HTTP_IDLE_TIMEOUT_MS  2000 // макс. пауза між байтами всередині запиту
#define HTTP_TOTAL_TIMEOUT_MS 5000 // макс. час від першого байта

typedef enum {
    HTTP_GET,
    HTTP_HEAD,
    HTTP_POST,
} http_method_t;

typedef struct {
    http_method_t method;
    char path[HTTP_MAX_PATH + 1];
    char query[HTTP_MAX_QUERY + 1]; // без '?', не декодований
    bool accept_gzip;
    char host[HTTP_MAX_HOST + 1]; // заголовок Host без порту; "", якщо немає або задовгий
    char rid[HTTP_MAX_RID + 1];   // заголовок X-Req-Id, лише літери й цифри; інакше ""
    char client[HTTP_MAX_RID + 1]; // заголовок X-Client-Id, ті ж правила: один id на відкриту сторінку
    size_t body_len;
    char body[HTTP_MAX_BODY + 1]; // завжди завершується NUL
} http_request_t;

typedef enum {
    HTTP_EV_NONE = 0,
    HTTP_EV_REQUEST, // parser->req містить повний запит
    HTTP_EV_ERROR,   // відповісти з parser->error_status
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
    uint32_t tail; // останні 4 байти під час пропуску до кінця заголовків
    int error_status;
    char drop[HTTP_DROP_SNIPPET]; // початок останнього відхиленого чи простроченого запиту, для діагностики
    size_t drop_len;
    bool error_head; // збійний запит був HEAD, тож відповідь про помилку має бути без тіла
    http_request_t req;
    http_stats_t stats;
} http_parser_t;

void http_parser_init(http_parser_t *p);

// Повертає кількість спожитих байтів; зупиняється одразу після події, щоб викликач обробив її і подав решту.
size_t http_parser_feed(http_parser_t *p, const uint8_t *data, size_t len, uint32_t now_ms,
                        http_event_t *ev);

// Повертає true, якщо неповний запит відкинуто. Без відповіді: отримувач невідомий.
bool http_parser_tick(http_parser_t *p, uint32_t now_ms);

// Відкидає неповний запит, напр. коли міст повідомляє про розрив TCP.
void http_parser_reset(http_parser_t *p);

bool http_parser_busy(const http_parser_t *p);

const char *http_status_text(int status);

// Пише рядок статусу і заголовки. Повертає довжину або 0, якщо не вміщається.
// extra: додаткові рядки заголовків, кожен закінчується "\r\n", або NULL.
size_t http_format_head(char *buf, size_t cap, int status, const char *content_type,
                        size_t content_length, const char *extra);

#ifdef __cplusplus
}
#endif
