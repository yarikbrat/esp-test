// BOOT button: held for BUTTON_HOLD_MS restores the default network settings (192.168.144.36/24).
// The same low-priority task logs a memory report every HEALTH_PERIOD_MS.
#include <stdio.h>

#include "app.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define BUTTON_POLL_MS   50
#define BUTTON_HOLD_MS   5000
#define HEALTH_PERIOD_MS 60000

static const char *TAG = "button";

// Free heap now and its all-time minimum, plus the unused stack of every task (bytes).
static void log_health(void)
{
    char line[160];
    int n = snprintf(line, sizeof(line), "heap free %u, min %u; stack free:", (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size());
    for (int i = 0; i < 5 && n > 0 && n < (int)sizeof(line); i++) {
        if (g_app.tasks[i]) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %s %u", pcTaskGetName(g_app.tasks[i]),
                          (unsigned)uxTaskGetStackHighWaterMark(g_app.tasks[i]));
        }
    }
    ESP_LOGI("health", "%s", line);
}

void task_button(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BOOT_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    uint32_t held_ms = 0;
    bool fired = false;
    uint32_t next_health = app_now_ms() + 10000;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        if ((int32_t)(app_now_ms() - next_health) >= 0) {
            next_health += HEALTH_PERIOD_MS;
            log_health();
        }
        if (gpio_get_level(PIN_BOOT_BTN) != 0) {
            held_ms = 0;
            fired = false;
            continue;
        }
        held_ms += BUTTON_POLL_MS;
        if (!fired && held_ms >= BUTTON_HOLD_MS) {
            fired = true; // once per press
            ESP_LOGW(TAG, "BOOT held %u ms: restoring default network settings", (unsigned)held_ms);
            app_net_reset_default();
        }
    }
}
