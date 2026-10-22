#include "altitude.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>

static const char *TAG = "main";

extern const uint8_t page_start[] asm("_binary_index_html_start");
extern const uint8_t page_end[] asm("_binary_index_html_end");
extern const uint8_t page_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t page_gz_end[] asm("_binary_index_html_gz_end");

// Placeholder until the hardware arrives: proves the toolchain, the core and the embedded page link.
void app_main(void)
{
    float h = 0;
    altitude_relative_m(100129.4f, 101325.0f, &h);
    ESP_LOGI(TAG, "core linked, reference altitude %.3f m", h);
    ESP_LOGI(TAG, "page %u B, gzip %u B", (unsigned)(page_end - page_start),
             (unsigned)(page_gz_end - page_gz_start));
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
