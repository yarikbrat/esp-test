#include "sonar.h"

#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#define US_PER_CM 58.3f // шлях туди й назад при 343 m/s

static const char *TAG = "sonar";

static volatile int64_t s_rise_us, s_fall_us;
static volatile bool s_armed;

// Не в IRAM: сервіс ISR встановлено без ESP_INTR_FLAG_IRAM, тому під час запису у flash він замаскований.
static void echo_isr(void *arg)
{
    (void)arg;
    if (!s_armed) {
        return;
    }
    int64_t t = esp_timer_get_time();
    if (gpio_get_level(PIN_SONAR_ECHO)) {
        if (!s_rise_us) {
            s_rise_us = t;
        }
    } else if (s_rise_us && !s_fall_us) {
        s_fall_us = t;
    }
}

bool sonar_init(void)
{
    gpio_config_t trig = {.pin_bit_mask = 1ULL << PIN_SONAR_TRIG, .mode = GPIO_MODE_OUTPUT};
    gpio_config_t echo = {
        .pin_bit_mask = 1ULL << PIN_SONAR_ECHO,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE, // непід'єднаний ECHO читається як 0, а не як шум
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_set_level(PIN_SONAR_TRIG, 0);
    esp_err_t err = gpio_config(&trig);
    if (err == ESP_OK) {
        err = gpio_config(&echo);
    }
    if (err == ESP_OK) {
        err = gpio_install_isr_service(0);
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_OK; // вже встановлено кимось іншим
        }
    }
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(PIN_SONAR_ECHO, echo_isr, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void sonar_trigger(void)
{
    s_armed = false;
    s_rise_us = 0;
    s_fall_us = 0;
    if (gpio_get_level(PIN_SONAR_ECHO)) {
        return; // ECHO ще високий від попереднього імпульсу: збір результату повідомить, що відлуння немає
    }
    s_armed = true;
    gpio_set_level(PIN_SONAR_TRIG, 1);
    esp_rom_delay_us(10);
    gpio_set_level(PIN_SONAR_TRIG, 0);
}

sensor_status_t sonar_collect(float *cm)
{
    s_armed = false;
    int64_t rise = s_rise_us, fall = s_fall_us;
    if (!rise || !fall) {
        return SS_NO_ECHO;
    }
    float d = (float)(fall - rise) / US_PER_CM;
    if (d < SONAR_MIN_CM || d > SONAR_MAX_CM) {
        return SS_OUT_OF_RANGE;
    }
    *cm = d;
    return SS_OK;
}
