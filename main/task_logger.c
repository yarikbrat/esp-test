// Logger task: appends a history record every interval and runs log clears.
#include "app.h"
#include "esp_log.h"

static const char *TAG = "logger";

static void run_clear(void)
{
    xSemaphoreTake(g_app.log_lock, portMAX_DELAY);
    int err = histlog_clear(&g_app.log);
    if (err) {
        ESP_LOGE(TAG, "clear failed (%d), rescanning", err);
        uint32_t errors = g_app.log.write_errors; // open() resets the counters; keep the history
        histlog_open(&g_app.log, &g_app.flash_ops, g_app.log.n_sectors);
        g_app.log.write_errors = errors;
        g_app.log.last_error = err;
    } else {
        ESP_LOGI(TAG, "cleared");
    }
    app_refresh_log_stats();
    xSemaphoreGive(g_app.log_lock);
    app_lock();
    g_app.log_clearing = false;
    app_unlock();
}

void task_logger(void *arg)
{
    (void)arg;
    app_lock();
    uint32_t next = app_now_ms() + (uint32_t)g_app.log_cfg.interval_s * 1000u;
    app_unlock();
    for (;;) {
        app_lock();
        bool clear = g_app.log_clear_req;
        if (clear) {
            g_app.log_clear_req = false;
            g_app.log_clearing = true;
        }
        bool changed = g_app.log_cfg_changed;
        g_app.log_cfg_changed = false;
        log_cfg_t cfg = g_app.log_cfg;
        app_unlock();

        if (clear) {
            run_clear();
            continue;
        }
        uint32_t period = (uint32_t)cfg.interval_s * 1000u;
        uint32_t now = app_now_ms();
        if (changed) {
            next = now + period; // a new interval starts when it is set
        }
        if ((int32_t)(now - next) >= 0) {
            if (cfg.enabled) {
                hl_record_t rec;
                app_lock();
                stand_fill_record(&g_app.stand, now, g_app.session, now / 1000u, &rec);
                app_unlock();
                xSemaphoreTake(g_app.log_lock, portMAX_DELAY);
                if (histlog_append(&g_app.log, &rec) != 0) {
                    ESP_LOGE(TAG, "write error %d", g_app.log.last_error);
                }
                app_refresh_log_stats();
                xSemaphoreGive(g_app.log_lock);
            }
            next += period; // from the previous deadline, so intervals do not drift
            if ((int32_t)(next - now) <= 0) {
                next = now + period;
            }
            continue;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(next - now) + 1);
    }
}
