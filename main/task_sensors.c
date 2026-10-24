// Sensor task: barometers at 20 Hz, HC-SR04 every SONAR_CYCLE_MS.
#include <math.h>

#include "app.h"
#include "board.h"
#include "bmp58x.h"
#include "esp_log.h"
#include "sonar.h"

#define SENSOR_PERIOD_MS 50

static const char *TAG = "sensors";

static bmp58x_t s_baro[STAND_BARO_N] = {
    {.name = "BMP585", .port = I2C_NUM_0, .sda = PIN_I2C0_SDA, .scl = PIN_I2C0_SCL, .expect_id = BMP585_CHIP_ID},
    {.name = "BMP581", .port = I2C_NUM_1, .sda = PIN_I2C1_SDA, .scl = PIN_I2C1_SCL, .expect_id = BMP581_CHIP_ID},
};

void task_sensors(void *arg)
{
    (void)arg;
    for (int ch = 0; ch < STAND_BARO_N; ch++) {
        bmp58x_bus_init(&s_baro[ch]);
    }
    bool sonar_ok = sonar_init();
    uint32_t next_sonar = app_now_ms();
    bool sonar_pending = false;

    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        uint32_t now = app_now_ms();
        sensor_status_t st[STAND_BARO_N];
        float p[STAND_BARO_N];
        for (int ch = 0; ch < STAND_BARO_N; ch++) {
            p[ch] = NAN;
            st[ch] = bmp58x_poll(&s_baro[ch], now, &p[ch]);
        }

        bool sonar_due = (int32_t)(now - next_sonar) >= 0;
        sensor_status_t sst = SS_NO_ECHO;
        float cm = NAN;
        if (sonar_due && sonar_ok && sonar_pending) {
            sst = sonar_collect(&cm);
        }

        stand_event_t ev[STAND_BARO_N];
        app_lock();
        for (int ch = 0; ch < STAND_BARO_N; ch++) {
            ev[ch] = stand_baro_sample(&g_app.stand, ch, st[ch], p[ch], now);
        }
        if (sonar_due && (sonar_pending || !sonar_ok)) {
            stand_sonar_sample(&g_app.stand, sst, cm, now);
        }
        app_unlock();

        if (sonar_due) {
            next_sonar = now + SONAR_CYCLE_MS;
            if (sonar_ok) {
                sonar_trigger();
                sonar_pending = true;
            }
        }
        for (int ch = 0; ch < STAND_BARO_N; ch++) {
            if (ev[ch] == STAND_EV_ZERO_DONE) {
                app_request_zero_persist(ch);
            } else if (ev[ch] == STAND_EV_ZERO_FAILED) {
                ESP_LOGW(TAG, "%s: zero calibration failed, previous zero kept", s_baro[ch].name);
            }
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }
}
