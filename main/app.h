#pragma once
// Спільний стан задач прошивки та бекенду веб-API.
// g_app.lock захищає app_t, крім histlog, який має свій log_lock (стирання сектора триває десятки ms).
// Порядок блокувань: спершу log_lock, потім lock. Ніколи не чекати на log_lock, утримуючи lock.
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
    // Копія лічильників histlog; задача журналу оновлює її після кожної операції з flash.
    uint32_t log_count, log_capacity, log_recs_per_sector, log_sectors;
    uint32_t log_write_errors, log_corrupt;
    int log_last_error;

    netcfg_t net;
    netcfg_t net_pending; // дійсне, поки net_apply == NET_APPLY_PENDING
    st_origin_t net_origin;
    net_apply_t net_apply;
    const char *net_apply_msg;

    http_stats_t http;

    SemaphoreHandle_t log_lock;
    histlog_t log;
    hl_flash_t flash_ops;

    QueueHandle_t persist_q; // номери каналів, чий нуль треба записати в NVS
    TaskHandle_t logger_task;
    TaskHandle_t tasks[5]; // для періодичного звіту про стек
} app_t;

extern app_t g_app;

uint32_t app_now_ms(void);

static inline void app_lock(void) { xSemaphoreTake(g_app.lock, portMAX_DELAY); }
static inline void app_unlock(void) { xSemaphoreGive(g_app.lock); }

void app_load_settings(void);
void app_request_zero_persist(int ch);
const char *app_zero_key(int ch); // ключ NVS нуля каналу: "zero0" або "zero1"
void app_refresh_log_stats(void); // викликати, утримуючи log_lock
api_backend_t app_backend(void);
void app_net_apply(const netcfg_t *cfg);
void app_net_reset_default(void);

void task_sensors(void *arg);
void task_logger(void *arg);
void task_storage(void *arg);
void task_bridge(void *arg);
void task_button(void *arg);
