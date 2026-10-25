#include "app.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "flash_part.h"
#include "kv.h"

static const char *TAG = "main";

// Пріоритети задач: міст відповідає браузеру, датчики тримають ритм 20 Hz, робота з flash чекає.
#define PRIO_BRIDGE  6
#define PRIO_SENSORS 5
#define PRIO_STORAGE 4
#define PRIO_LOGGER  3
#define PRIO_BUTTON  2

// RST і DEF модуля активні низьким рівнем: тримати їх відпущеними (вхід із підтяжкою), поки ми ними не керуємо.
static void release_module_pins(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_MOD_RST) | (1ULL << PIN_MOD_DEF),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
}

static void open_log(void)
{
    uint32_t sectors = flash_part_open(&g_app.flash_ops);
    xSemaphoreTake(g_app.log_lock, portMAX_DELAY);
    if (!sectors || histlog_open(&g_app.log, &g_app.flash_ops, sectors) != 0) {
        ESP_LOGE(TAG, "history log unavailable");
    }
    ESP_LOGI(TAG, "history: %u of %u records (corrupt %u)", (unsigned)histlog_count(&g_app.log),
             (unsigned)g_app.log.capacity, (unsigned)g_app.log.corrupt);
    // Жодного дійсного запису й жодного стертого слота: розділ ніколи не форматували. Задачі ще не запущені,
    // тож довге стирання нічого не гальмує.
    if (g_app.log.capacity && histlog_count(&g_app.log) == 0 && g_app.log.corrupt == g_app.log.capacity) {
        ESP_LOGW(TAG, "history partition holds no log data, formatting it");
        if (histlog_format(&g_app.log) != 0) {
            int err = g_app.log.last_error;
            ESP_LOGE(TAG, "format failed (%d), rescanning", err);
            histlog_open(&g_app.log, &g_app.flash_ops, sectors); // реальний стан частково стертого розділу
            g_app.log.write_errors++;
            g_app.log.last_error = err;
        }
    }
    app_refresh_log_stats();
    xSemaphoreGive(g_app.log_lock);
}

void app_main(void)
{
    release_module_pins();
    g_app.lock = xSemaphoreCreateMutex();
    g_app.log_lock = xSemaphoreCreateMutex();
    g_app.persist_q = xQueueCreate(8, sizeof(uint8_t));

    stand_init(&g_app.stand);
    kv_init();
    app_load_settings();
    open_log();
    ESP_LOGI(TAG, "session %u", g_app.session);

    xTaskCreate(task_storage, "storage", 3072, NULL, PRIO_STORAGE, &g_app.tasks[0]);
    xTaskCreate(task_logger, "logger", 4096, NULL, PRIO_LOGGER, &g_app.tasks[1]);
    g_app.logger_task = g_app.tasks[1];
    xTaskCreate(task_sensors, "sensors", 4096, NULL, PRIO_SENSORS, &g_app.tasks[2]);
    xTaskCreate(task_bridge, "bridge", 6144, NULL, PRIO_BRIDGE, &g_app.tasks[3]);
    xTaskCreate(task_button, "button", 3072, NULL, PRIO_BUTTON, &g_app.tasks[4]);
}
