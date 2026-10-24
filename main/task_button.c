// BOOT button: held for BUTTON_HOLD_MS restores the default network settings (192.168.144.36/24).
#include "app.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define BUTTON_POLL_MS 50
#define BUTTON_HOLD_MS 5000

static const char *TAG = "button";

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
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
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
