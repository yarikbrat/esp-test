#pragma once
// Процедура "задати нуль" для одного каналу барометра. Належить задачі датчиків, не потокобезпечна.
// Успішна, коли минуло >= ZERO_CAL_MIN_MS і надійшло >= ZERO_CAL_MIN_SAMPLES дійсних вимірів;
// невдала, якщо цього не сталося до ZERO_CAL_MAX_MS. Час передається ззовні (монотонні мс).
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZERO_CAL_MIN_SAMPLES 10
#define ZERO_CAL_MIN_MS      1000u
#define ZERO_CAL_MAX_MS      3000u

typedef enum {
    ZERO_CAL_IDLE = 0,
    ZERO_CAL_RUNNING,
} zero_cal_state_t;

typedef enum {
    ZERO_CAL_NONE = 0,    // ще триває або не почата
    ZERO_CAL_DONE,        // p0 обчислено
    ZERO_CAL_ERR_TIMEOUT, // замало дійсних вимірів за ZERO_CAL_MAX_MS
} zero_cal_result_t;

typedef struct {
    zero_cal_state_t state;
    uint32_t start_ms;
    uint32_t n;
    double sum;
    float min, max;
} zero_cal_t;

void zero_cal_init(zero_cal_t *zc);

// Повертає false, якщо вже триває: повторний клік не має запускати паралельний прогін.
bool zero_cal_start(zero_cal_t *zc, uint32_t now_ms);

bool zero_cal_running(const zero_cal_t *zc);

// Подає один вимір; при ZERO_CAL_DONE пише середнє в *p0_out і max-min в *spread_out.
zero_cal_result_t zero_cal_feed(zero_cal_t *zc, bool valid, float p_pa, uint32_t now_ms,
                                float *p0_out, float *spread_out);

// Перевірка тайм-ауту, коли датчик перестав відповідати і feed() не викликається.
zero_cal_result_t zero_cal_poll(zero_cal_t *zc, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
