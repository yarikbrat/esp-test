#include "zero_cal.h"

#include "altitude.h"

#include <string.h>

void zero_cal_init(zero_cal_t *zc)
{
    memset(zc, 0, sizeof(*zc));
}

bool zero_cal_start(zero_cal_t *zc, uint32_t now_ms)
{
    if (zc->state == ZERO_CAL_RUNNING) {
        return false;
    }
    zero_cal_init(zc);
    zc->state = ZERO_CAL_RUNNING;
    zc->start_ms = now_ms;
    return true;
}

bool zero_cal_running(const zero_cal_t *zc)
{
    return zc->state == ZERO_CAL_RUNNING;
}

zero_cal_result_t zero_cal_poll(zero_cal_t *zc, uint32_t now_ms)
{
    if (zc->state != ZERO_CAL_RUNNING) {
        return ZERO_CAL_NONE;
    }
    // Беззнакова різниця лишається правильною при переповненні лічильника мс.
    if (now_ms - zc->start_ms > ZERO_CAL_MAX_MS) {
        zc->state = ZERO_CAL_IDLE;
        return ZERO_CAL_ERR_TIMEOUT;
    }
    return ZERO_CAL_NONE;
}

zero_cal_result_t zero_cal_feed(zero_cal_t *zc, bool valid, float p_pa, uint32_t now_ms,
                                float *p0_out, float *spread_out)
{
    if (zc->state != ZERO_CAL_RUNNING) {
        return ZERO_CAL_NONE;
    }
    uint32_t elapsed = now_ms - zc->start_ms;
    if (elapsed > ZERO_CAL_MAX_MS) {
        zc->state = ZERO_CAL_IDLE;
        return ZERO_CAL_ERR_TIMEOUT;
    }
    if (valid && baro_pressure_plausible(p_pa)) {
        if (zc->n == 0 || p_pa < zc->min) {
            zc->min = p_pa;
        }
        if (zc->n == 0 || p_pa > zc->max) {
            zc->max = p_pa;
        }
        zc->sum += p_pa;
        zc->n++;
    }
    if (elapsed >= ZERO_CAL_MIN_MS && zc->n >= ZERO_CAL_MIN_SAMPLES) {
        if (p0_out) {
            *p0_out = (float)(zc->sum / zc->n);
        }
        if (spread_out) {
            *spread_out = zc->max - zc->min;
        }
        zc->state = ZERO_CAL_IDLE;
        return ZERO_CAL_DONE;
    }
    return ZERO_CAL_NONE;
}
