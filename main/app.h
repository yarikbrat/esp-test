#pragma once
// Shared state of the firmware tasks and the web API backend.
// g_app.lock guards app_t except the histlog, which has log_lock (an erase takes seconds).
// Lock order: log_lock, then lock. Never wait for log_lock while holding lock.
#include <stdbool.h>
#include <stdint.h>

#include "api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "histlog.h"
#include "settings_codec.h"
#include "stand.h"

typedef struct {
    SemaphoreHandle_t lock;
    stand_t stand;
    uint16_t session;

    log_cfg_t log_cfg;
    st_origin_t log_origin;
    persist_t log_persist;
    bool log_cfg_changed;
    bool log_clear_req;
    bool log_clearing;
    // Copy of the histlog counters, refreshed by the logger task after every flash operation.
    uint32_t log_count, log_capacity, log_recs_per_sector, log_sectors;
    uint32_t log_write_errors, log_corrupt;
    int log_last_error;

    netcfg_t net;
    st_origin_t net_origin;
    net_apply_t net_apply;
    const char *net_apply_msg;

    http_stats_t http;

    SemaphoreHandle_t log_lock;
    histlog_t log;
    hl_flash_t flash_ops;

    QueueHandle_t persist_q; // channel numbers whose zero must be written to NVS
    TaskHandle_t logger_task;
} app_t;

extern app_t g_app;

uint32_t app_now_ms(void);

static inline void app_lock(void) { xSemaphoreTake(g_app.lock, portMAX_DELAY); }
static inline void app_unlock(void) { xSemaphoreGive(g_app.lock); }

void app_load_settings(void);
void app_request_zero_persist(int ch);
void app_refresh_log_stats(void); // caller holds log_lock
api_backend_t app_backend(void);
void app_net_apply(const netcfg_t *cfg);
void app_net_reset_default(void);

void task_sensors(void *arg);
void task_logger(void *arg);
void task_storage(void *arg);
void task_bridge(void *arg);
void task_button(void *arg);
