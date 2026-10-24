// Bridge task: HTTP over the UART of the UART TO ETH module. All TCP clients share one byte stream,
// and the module copies every reply to all of them; see docs/uart-to-eth-b.md.
#include "app.h"
#include "board.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "http_parser.h"

#define UART_RX_BUF 4096
#define UART_TX_BUF 8192

static const char *TAG = "bridge";

static http_parser_t s_parser;
static char s_scratch[8192];

// One log line with the start of a dropped request, non-printable bytes as \xNN.
static void log_drop(const char *what)
{
    char line[4 * HTTP_DROP_SNIPPET + 1];
    size_t n = 0;
    for (size_t i = 0; i < s_parser.drop_len && n + 5 < sizeof(line); i++) {
        unsigned char c = (unsigned char)s_parser.drop[i];
        n += (size_t)snprintf(line + n, sizeof(line) - n, (c >= 0x20 && c < 0x7f) ? "%c" : "\\x%02x", c);
    }
    line[n] = '\0';
    ESP_LOGW(TAG, "%s: %s", what, line);
}

static int uart_sink(void *ctx, const void *data, size_t len)
{
    (void)ctx;
    int n = uart_write_bytes(BRIDGE_UART_NUM, data, len);
    return n == (int)len ? 0 : -1;
}

void task_bridge(void *arg)
{
    (void)arg;
    uart_config_t cfg = {
        .baud_rate = BRIDGE_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(BRIDGE_UART_NUM, UART_RX_BUF, UART_TX_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BRIDGE_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(BRIDGE_UART_NUM, PIN_BRIDGE_TX, PIN_BRIDGE_RX, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    http_parser_init(&s_parser);
    api_backend_t be = app_backend();
    api_out_t out = {.write = uart_sink};
    ESP_LOGI(TAG, "UART%d %d 8N1, serving HTTP", BRIDGE_UART_NUM, BRIDGE_BAUD);

    static uint8_t buf[512];
    for (;;) {
        int got = uart_read_bytes(BRIDGE_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(20));
        size_t off = 0;
        while (got > 0 && off < (size_t)got) {
            http_event_t ev;
            off += http_parser_feed(&s_parser, buf + off, (size_t)got - off, app_now_ms(), &ev);
            if (ev == HTTP_EV_REQUEST) {
                api_after_t after = api_handle(&be, &s_parser.req, &out, s_scratch, sizeof(s_scratch));
                if (after.net_apply) {
                    uart_wait_tx_done(BRIDGE_UART_NUM, pdMS_TO_TICKS(1000));
                    app_net_apply(&after.net);
                }
            } else if (ev == HTTP_EV_ERROR) {
                ESP_LOGW(TAG, "bad request, status %d", s_parser.error_status);
                log_drop("rejected request starts with");
                api_send_error(&out, s_parser.error_status, s_parser.error_head, s_scratch, sizeof(s_scratch));
            }
        }
        if (http_parser_tick(&s_parser, app_now_ms())) {
            log_drop("incomplete request dropped (timeout), it started with");
        }
        app_lock();
        g_app.http = s_parser.stats;
        app_unlock();
    }
}
