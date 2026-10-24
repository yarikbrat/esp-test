// Bench bring-up only (not the product firmware): HTTP over the UART TO ETH bridge, I2C scan, raw sensor reads.
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "bringup";

#define BRIDGE_UART UART_NUM_2
#define BRIDGE_TX_PIN 17
#define BRIDGE_RX_PIN 16
#define BRIDGE_BAUD 115200
#define MOD_RST_PIN 25
#define MOD_DEF_PIN 26

#define REQ_MAX 2048
#define REQ_IDLE_US 2000000
#define MAX_FOUND 8
#define SONAR_TRIG_PIN 33
#define SONAR_ECHO_PIN 32
#define SONAR_TIMEOUT_US 40000

// BMP58x registers and fields, as in Bosch BMP5_SensorAPI bmp5_defs.h.
#define BMP5_REG_CHIP_ID 0x01
#define BMP5_REG_TEMP_DATA_XLSB 0x1D
#define BMP5_REG_INT_STATUS 0x27
#define BMP5_REG_OSR_CONFIG 0x36
#define BMP5_REG_ODR_CONFIG 0x37
#define BMP5_REG_CMD 0x7E
#define BMP5_SOFT_RESET 0xB6
#define BMP5_PRESS_EN 0x40
#define BMP5_OSR_P_8X (0x03 << 3)
#define BMP5_OSR_T_1X 0x00
#define BMP5_DEEP_DIS 0x80
#define BMP5_ODR_10HZ (0x17 << 2)
#define BMP5_MODE_NORMAL 0x01

typedef struct {
    const char *name;
    i2c_port_num_t port;
    int sda;
    int scl;
} bus_def_t;

typedef struct {
    bool pullup_sda;
    bool pullup_scl;
    bool bus_ok;
    int n_found;
    uint8_t addr[MAX_FOUND];
    int chip_id[MAX_FOUND]; // -1 when not read
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t baro; // first BMP58x found on the bus, kept for reads
} bus_report_t;

static const bus_def_t BUSES[2] = {
    {"I2C0 (BMP585)", I2C_NUM_0, 21, 22},
    {"I2C1 (BMP581)", I2C_NUM_1, 18, 19},
};

static bus_report_t g_report[2];
static uint32_t g_requests;
static char g_page[3072];
static char g_head[256];

// With only the internal pull-down on, the line reads high only if something external pulls it up.
static bool has_external_pullup(int pin)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(2));
    bool high = gpio_get_level(pin) == 1;
    gpio_reset_pin(pin);
    return high;
}

static esp_err_t bmp_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(dev, b, sizeof(b), 50);
}

static esp_err_t baro_init(i2c_master_dev_handle_t dev)
{
    bmp_write(dev, BMP5_REG_CMD, BMP5_SOFT_RESET); // may be NACKed while the chip resets
    vTaskDelay(pdMS_TO_TICKS(5));
    uint8_t reg = BMP5_REG_INT_STATUS, v = 0;
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, &v, 1, 50); // clears the power-on flag
    if (err == ESP_OK) {
        err = bmp_write(dev, BMP5_REG_ODR_CONFIG, BMP5_DEEP_DIS); // standby before changing the config
    }
    vTaskDelay(pdMS_TO_TICKS(3));
    if (err == ESP_OK) {
        err = bmp_write(dev, BMP5_REG_OSR_CONFIG, BMP5_PRESS_EN | BMP5_OSR_P_8X | BMP5_OSR_T_1X);
    }
    if (err == ESP_OK) {
        err = bmp_write(dev, BMP5_REG_ODR_CONFIG, BMP5_DEEP_DIS | BMP5_ODR_10HZ | BMP5_MODE_NORMAL);
    }
    return err;
}

static esp_err_t baro_read(i2c_master_dev_handle_t dev, float *p_pa, float *t_c)
{
    uint8_t reg = BMP5_REG_TEMP_DATA_XLSB, d[6];
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, d, sizeof(d), 50);
    if (err != ESP_OK) {
        return err;
    }
    int32_t t = (int32_t)(((uint32_t)d[2] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[0] << 8)) >> 8;
    uint32_t p = ((uint32_t)d[5] << 16) | ((uint32_t)d[4] << 8) | d[3];
    *t_c = (float)t / 65536.0f;
    *p_pa = (float)p / 64.0f;
    return ESP_OK;
}

static void scan_bus(const bus_def_t *def, bus_report_t *rep)
{
    if (rep->baro) {
        i2c_master_bus_rm_device(rep->baro);
    }
    if (rep->bus) {
        i2c_del_master_bus(rep->bus);
    }
    memset(rep, 0, sizeof(*rep));
    rep->pullup_sda = has_external_pullup(def->sda);
    rep->pullup_scl = has_external_pullup(def->scl);

    i2c_master_bus_config_t cfg = {
        .i2c_port = def->port,
        .sda_io_num = def->sda,
        .scl_io_num = def->scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "%s: bus init failed", def->name);
        return;
    }
    rep->bus_ok = true;

    for (uint16_t a = 0x08; a <= 0x77 && rep->n_found < MAX_FOUND; a++) {
        if (i2c_master_probe(bus, a, 20) != ESP_OK) {
            continue;
        }
        int idx = rep->n_found++;
        rep->addr[idx] = (uint8_t)a;
        rep->chip_id[idx] = -1;
        if (a != 0x46 && a != 0x47) {
            continue;
        }
        i2c_device_config_t dcfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = a,
            .scl_speed_hz = 100000,
        };
        i2c_master_dev_handle_t dev;
        if (i2c_master_bus_add_device(bus, &dcfg, &dev) == ESP_OK) {
            uint8_t reg = BMP5_REG_CHIP_ID, id = 0;
            if (i2c_master_transmit_receive(dev, &reg, 1, &id, 1, 50) == ESP_OK) {
                rep->chip_id[idx] = id;
            }
            bool keep = !rep->baro && (id == 0x50 || id == 0x51) && baro_init(dev) == ESP_OK;
            if (keep) {
                rep->baro = dev;
            } else {
                i2c_master_bus_rm_device(dev);
            }
        }
    }
    rep->bus = bus;

    ESP_LOGI(TAG, "%s SDA%d/SCL%d: ext pull-up sda=%d scl=%d, found %d", def->name, def->sda, def->scl,
             rep->pullup_sda, rep->pullup_scl, rep->n_found);
    for (int i = 0; i < rep->n_found; i++) {
        ESP_LOGI(TAG, "  0x%02x chip_id=%d (0x%02x)", rep->addr[i], rep->chip_id[i], rep->chip_id[i] & 0xff);
    }
}

static void scan_all(void)
{
    for (int i = 0; i < 2; i++) {
        scan_bus(&BUSES[i], &g_report[i]);
    }
}

static void sonar_init(void)
{
    gpio_config_t trig = {.pin_bit_mask = 1ULL << SONAR_TRIG_PIN, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&trig);
    gpio_set_level(SONAR_TRIG_PIN, 0);
    // Pull-down keeps an unconnected ECHO at 0 instead of floating.
    gpio_config_t echo = {
        .pin_bit_mask = 1ULL << SONAR_ECHO_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&echo);
}

// Echo pulse width in microseconds; -1 no echo started, -2 echo never ended, -3 echo stuck high.
static int32_t sonar_measure_us(void)
{
    if (gpio_get_level(SONAR_ECHO_PIN)) {
        return -3;
    }
    gpio_set_level(SONAR_TRIG_PIN, 1);
    esp_rom_delay_us(10);
    gpio_set_level(SONAR_TRIG_PIN, 0);
    int64_t t0 = esp_timer_get_time();
    while (!gpio_get_level(SONAR_ECHO_PIN)) {
        if (esp_timer_get_time() - t0 > SONAR_TIMEOUT_US) {
            return -1;
        }
    }
    int64_t rise = esp_timer_get_time();
    while (gpio_get_level(SONAR_ECHO_PIN)) {
        if (esp_timer_get_time() - rise > SONAR_TIMEOUT_US) {
            return -2;
        }
    }
    return (int32_t)(esp_timer_get_time() - rise);
}

static const char *chip_name(int id)
{
    if (id == 0x50) {
        return "BMP581";
    }
    if (id == 0x51) {
        return "BMP585";
    }
    return "невідомий";
}

static size_t g_page_len;

// Appends to g_page; output past the end is cut off instead of overflowing.
static void put(const char *fmt, ...)
{
    if (g_page_len >= sizeof(g_page) - 1) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(g_page + g_page_len, sizeof(g_page) - g_page_len, fmt, ap);
    va_end(ap);
    if (w > 0) {
        g_page_len += (size_t)w;
        if (g_page_len > sizeof(g_page) - 1) {
            g_page_len = sizeof(g_page) - 1;
        }
    }
}

static size_t render_page(void)
{
    g_page_len = 0;
    put("<!doctype html><html lang=\"uk\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta http-equiv=\"refresh\" content=\"2;url=/\">"
        "<link rel=\"icon\" href=\"data:,\"><title>Перевірка стенда</title>"
        "<style>body{font-family:sans-serif;margin:16px;max-width:720px}td,th{padding:4px 8px;text-align:left}"
        "table{border-collapse:collapse}tr{border-bottom:1px solid #ccc}</style></head><body>"
        "<h1>Зв'язок з ESP32 працює</h1>"
        "<p>Браузер &rarr; модуль Ethernet &rarr; ESP32 &rarr; назад.</p>"
        "<p>Час роботи: %lld с. Запитів: %lu.</p>"
        "<h2>Шини I2C</h2><table><tr><th>Шина</th><th>Виводи</th><th>Підтяжка SDA / SCL</th><th>Пристрої</th></tr>",
        (long long)(esp_timer_get_time() / 1000000), (unsigned long)g_requests);

    for (int b = 0; b < 2; b++) {
        const bus_report_t *r = &g_report[b];
        put("<tr><td>%s</td><td>SDA %d, SCL %d</td><td>%s / %s</td><td>", BUSES[b].name, BUSES[b].sda,
            BUSES[b].scl, r->pullup_sda ? "є" : "немає", r->pullup_scl ? "є" : "немає");
        if (!r->bus_ok) {
            put("помилка шини");
        } else if (r->n_found == 0) {
            put("не знайдено");
        }
        for (int i = 0; i < r->n_found; i++) {
            if (r->chip_id[i] >= 0) {
                put("0x%02x: CHIP_ID 0x%02x (%s)<br>", r->addr[i], r->chip_id[i], chip_name(r->chip_id[i]));
            } else {
                put("0x%02x<br>", r->addr[i]);
            }
        }
        put("</td></tr>");
    }
    put("</table><h2>Показання</h2><table>");
    for (int b = 0; b < 2; b++) {
        float p, t;
        if (!g_report[b].baro) {
            put("<tr><td>%s</td><td>датчик не ініціалізовано</td></tr>", BUSES[b].name);
        } else if (baro_read(g_report[b].baro, &p, &t) == ESP_OK) {
            put("<tr><td>%s</td><td>тиск %.1f Па, температура %.2f &deg;C</td></tr>", BUSES[b].name, p, t);
        } else {
            put("<tr><td>%s</td><td>помилка читання</td></tr>", BUSES[b].name);
        }
    }
    int32_t us = sonar_measure_us();
    if (us >= 0) {
        put("<tr><td>HC-SR04</td><td>%.1f см (відлуння %ld мкс)</td></tr>", us / 58.3f, (long)us);
    } else {
        put("<tr><td>HC-SR04</td><td>%s</td></tr>",
            us == -1 ? "немає відлуння" : us == -2 ? "відлуння задовге" : "ECHO постійно 1");
    }
    put("</table><p>Сторінка оновлюється кожні 2 с. <a href=\"/rescan\">Пересканувати шини</a></p></body></html>");
    return g_page_len;
}

static void send_response(int status, const char *reason, const char *body, size_t body_len)
{
    int hn = snprintf(g_head, sizeof(g_head),
                      "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\n"
                      "Cache-Control: no-store\r\nConnection: keep-alive\r\n\r\n",
                      status, reason, (unsigned)body_len);
    uart_write_bytes(BRIDGE_UART, g_head, (size_t)hn);
    if (body_len) {
        uart_write_bytes(BRIDGE_UART, body, body_len);
    }
}

// Only GET/HEAD are expected here; request bodies are not supported by this test tool.
static void handle_request(const char *req)
{
    char method[8] = "", path[64] = "";
    sscanf(req, "%7s %63s", method, path);
    g_requests++;
    ESP_LOGI(TAG, "request %lu: %s %s", (unsigned long)g_requests, method, path);

    bool head_only = strcmp(method, "HEAD") == 0;
    if (strcmp(method, "GET") != 0 && !head_only) {
        static const char msg[] = "Підтримуються лише GET і HEAD";
        send_response(405, "Method Not Allowed", msg, sizeof(msg) - 1);
        return;
    }
    if (strcmp(path, "/rescan") == 0) {
        scan_all();
    } else if (strcmp(path, "/") != 0) {
        static const char msg[] = "Не знайдено";
        send_response(404, "Not Found", msg, sizeof(msg) - 1);
        return;
    }
    size_t len = render_page();
    send_response(200, "OK", g_page, head_only ? 0 : len);
}

static void bridge_task(void *arg)
{
    static char req[REQ_MAX + 1];
    static uint8_t buf[256];
    size_t len = 0;
    int64_t last_rx = 0;

    for (;;) {
        int n = uart_read_bytes(BRIDGE_UART, buf, sizeof(buf), pdMS_TO_TICKS(100));
        int64_t now = esp_timer_get_time();
        if (n <= 0) {
            if (len && now - last_rx > REQ_IDLE_US) {
                ESP_LOGW(TAG, "dropping %u stale bytes", (unsigned)len);
                len = 0;
            }
            continue;
        }
        last_rx = now;
        if (len + (size_t)n > REQ_MAX) {
            ESP_LOGW(TAG, "request too large, dropped");
            len = 0;
            continue;
        }
        memcpy(req + len, buf, (size_t)n);
        len += (size_t)n;
        req[len] = '\0';

        char *end;
        while ((end = strstr(req, "\r\n\r\n")) != NULL) {
            *end = '\0';
            handle_request(req);
            size_t used = (size_t)(end + 4 - req);
            memmove(req, req + used, len - used + 1);
            len -= used;
        }
    }
}

// RST and DEF of the module are active low: keep them as inputs with a weak pull-up, i.e. released.
static void release_module_pins(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << MOD_RST_PIN) | (1ULL << MOD_DEF_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io);
}

void app_main(void)
{
    release_module_pins();
    sonar_init();
    scan_all();

    uart_config_t ucfg = {
        .baud_rate = BRIDGE_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(BRIDGE_UART, 2048, 4096, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BRIDGE_UART, &ucfg));
    ESP_ERROR_CHECK(uart_set_pin(BRIDGE_UART, BRIDGE_TX_PIN, BRIDGE_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "bridge UART%d TX%d RX%d %d 8N1 ready", BRIDGE_UART, BRIDGE_TX_PIN, BRIDGE_RX_PIN, BRIDGE_BAUD);

    xTaskCreate(bridge_task, "bridge", 4096, NULL, 5, NULL);
}
