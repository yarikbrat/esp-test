#pragma once
// "Set zero" procedure for one barometer channel. Owned by the sensor task, not thread-safe.
// Succeeds once >= ZERO_CAL_MIN_MS elapsed and >= ZERO_CAL_MIN_SAMPLES valid samples arrived;
// fails if that has not happened by ZERO_CAL_MAX_MS. Time is passed in (monotonic ms).
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
    ZERO_CAL_NONE = 0,    // still running or not started
    ZERO_CAL_DONE,        // p0 computed
    ZERO_CAL_ERR_TIMEOUT, // not enough valid samples within ZERO_CAL_MAX_MS
} zero_cal_result_t;

typedef struct {
    zero_cal_state_t state;
    uint32_t start_ms;
    uint32_t n;
    double sum;
    float min, max;
} zero_cal_t;

void zero_cal_init(zero_cal_t *zc);

// Returns false if already running: a repeated click must not start a parallel run.
bool zero_cal_start(zero_cal_t *zc, uint32_t now_ms);

bool zero_cal_running(const zero_cal_t *zc);

// Feeds one sample; on ZERO_CAL_DONE writes the mean to *p0_out and max-min to *spread_out.
zero_cal_result_t zero_cal_feed(zero_cal_t *zc, bool valid, float p_pa, uint32_t now_ms,
                                float *p0_out, float *spread_out);

// Timeout check for when the sensor stopped answering and feed() is not called.
zero_cal_result_t zero_cal_poll(zero_cal_t *zc, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
