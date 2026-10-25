#pragma once
// Модель стану стенда: три канали, застарілість, процедура нуля. Не потокобезпечна: викликачі тримають
// одне блокування навколо кожного виклику, а веб-код працює з копією (snapshot), знятою під ним.
#include <stdbool.h>
#include <stdint.h>

#include "histlog.h"
#include "settings_codec.h"
#include "zero_cal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STAND_BARO_N        2
#define STAND_BARO_STALE_MS 1000u
#define STAND_SONAR_STALE_MS 500u
#define STAND_SONAR_MEDIAN  5
#define STAND_BARO_AVG_N    20 // ковзне середнє тиску: 1 с при опитуванні 20 Гц

// Статус однієї спроби читання від драйвера; значення пишуться в записи журналу, не змінювати.
typedef enum {
    SS_INIT = 0,       // ще не прочитано
    SS_OK = 1,
    SS_NO_ECHO = 2,    // HC-SR04: ECHO не прийшов або не закінчився
    SS_OUT_OF_RANGE = 3,
    SS_NOT_FOUND = 4,  // I2C: немає ACK / хибний chip id
    SS_COMM_ERROR = 5, // I2C: помилка шини або тайм-аут
    SS_REINIT = 6,     // драйвер переініціалізує датчик
    SS_STALE = 7,      // лише для журналу: останнє читання було добрим, але старше за межу застарілості
} sensor_status_t;

typedef enum {
    ZO_NONE = 0,
    ZO_RESTORED, // завантажено з flash при старті
    ZO_NEW,      // задано в цій сесії
} zero_origin_t;

typedef enum {
    CAL_IDLE = 0,
    CAL_RUNNING,
    CAL_OK,
    CAL_FAILED,
} cal_view_t;

typedef enum {
    CAL_ERR_NONE = 0,
    CAL_ERR_NO_DATA, // не було свіжого дійсного тиску при натисканні кнопки
    CAL_ERR_TIMEOUT, // менше ZERO_CAL_MIN_SAMPLES дійсних вимірів за ZERO_CAL_MAX_MS
} cal_err_t;

typedef enum {
    PERSIST_NONE = 0,
    PERSIST_PENDING,
    PERSIST_OK,
    PERSIST_ERROR,
} persist_t;

typedef struct {
    sensor_status_t status; // остання спроба читання
    bool ever_ok;
    uint32_t last_ok_ms;
    float value; // останнє дійсне значення: Па для барометрів, см для сонара
    uint32_t errors;
} chan_t;

typedef struct {
    chan_t c; // c.value — ковзне середнє останніх STAND_BARO_AVG_N дійсних вимірів
    float avg_win[STAND_BARO_AVG_N];
    uint8_t avg_n, avg_pos;
    zero_cfg_t zero;
    zero_origin_t origin;
    st_origin_t load_origin; // яким був запис нуля при старті
    persist_t persist;
    zero_cal_t cal;
    cal_view_t cal_view;
    cal_err_t cal_err;
    float cal_spread_pa;
} baro_t;

typedef struct {
    chan_t sonar;
    float sonar_win[STAND_SONAR_MEDIAN];
    uint8_t sonar_win_n, sonar_win_pos;
    baro_t baro[STAND_BARO_N];
} stand_t;

typedef enum {
    STAND_EV_NONE = 0,
    STAND_EV_ZERO_DONE,   // збережіть baro[ch].zero, потім викличте stand_zero_persisted()
    STAND_EV_ZERO_FAILED, // попередній нуль збережено
} stand_event_t;

typedef enum {
    STAND_ZERO_STARTED = 0,
    STAND_ZERO_BUSY,
    STAND_ZERO_NO_DATA,
} stand_zero_start_t;

void stand_init(stand_t *s);

void stand_load_zero(stand_t *s, int ch, const zero_cfg_t *z, st_origin_t origin);

stand_event_t stand_baro_sample(stand_t *s, int ch, sensor_status_t st, float p_pa, uint32_t now_ms);

// Перевірка тайм-ауту калібрування для датчика, що перестав давати виміри.
stand_event_t stand_baro_poll(stand_t *s, int ch, uint32_t now_ms);

void stand_sonar_sample(stand_t *s, sensor_status_t st, float cm, uint32_t now_ms);

stand_zero_start_t stand_zero_start(stand_t *s, int ch, uint32_t now_ms);

// Скидає нуль у RAM; викликач зберігає {set=false} і звітує через stand_zero_persisted().
// Повертає false, поки триває калібрування.
bool stand_zero_reset(stand_t *s, int ch);

void stand_zero_persisted(stand_t *s, int ch, bool ok);

bool stand_chan_fresh(const chan_t *c, uint32_t stale_ms, uint32_t now_ms);

// Висота за останнім свіжим тиском; false, якщо немає нуля або свіжих даних.
bool stand_baro_altitude(const baro_t *b, uint32_t now_ms, float *out_m);

// Заповнює обидва канали з одного стану, тож обидва значення з одного циклу журналу.
void stand_fill_record(const stand_t *s, uint32_t now_ms, uint16_t session, uint32_t t_s,
                       hl_record_t *rec);

#ifdef __cplusplus
}
#endif
