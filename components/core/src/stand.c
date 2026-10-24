#include "stand.h"

#include "altitude.h"

#include <math.h>
#include <string.h>

void stand_init(stand_t *s)
{
    memset(s, 0, sizeof(*s));
    for (int ch = 0; ch < STAND_BARO_N; ch++) {
        zero_cal_init(&s->baro[ch].cal);
        s->baro[ch].load_origin = ST_MISSING;
    }
}

void stand_load_zero(stand_t *s, int ch, const zero_cfg_t *z, st_origin_t origin)
{
    baro_t *b = &s->baro[ch];
    b->zero = *z;
    b->load_origin = origin;
    b->origin = z->set ? ZO_RESTORED : ZO_NONE;
}

bool stand_chan_fresh(const chan_t *c, uint32_t stale_ms, uint32_t now_ms)
{
    return c->ever_ok && now_ms - c->last_ok_ms <= stale_ms;
}

static stand_event_t finish_cal(baro_t *b, zero_cal_result_t r, float p0, float spread)
{
    if (r == ZERO_CAL_DONE) {
        b->zero.set = true;
        b->zero.p0_pa = p0;
        b->origin = ZO_NEW;
        b->persist = PERSIST_PENDING;
        b->cal_view = CAL_OK;
        b->cal_spread_pa = spread;
        return STAND_EV_ZERO_DONE;
    }
    if (r == ZERO_CAL_ERR_TIMEOUT) {
        b->cal_view = CAL_FAILED;
        b->cal_err = CAL_ERR_TIMEOUT;
        return STAND_EV_ZERO_FAILED;
    }
    return STAND_EV_NONE;
}

stand_event_t stand_baro_sample(stand_t *s, int ch, sensor_status_t st, float p_pa, uint32_t now_ms)
{
    baro_t *b = &s->baro[ch];
    bool ok = st == SS_OK && baro_pressure_plausible(p_pa);
    if (ok) {
        // After an outage the window restarts, so pre-outage samples never mix with new ones.
        if (!stand_chan_fresh(&b->c, STAND_BARO_STALE_MS, now_ms)) {
            b->avg_n = 0;
            b->avg_pos = 0;
        }
        b->avg_win[b->avg_pos] = p_pa;
        b->avg_pos = (uint8_t)((b->avg_pos + 1) % STAND_BARO_AVG_N);
        if (b->avg_n < STAND_BARO_AVG_N) {
            b->avg_n++;
        }
        double sum = 0; // recomputed each time, so rounding never accumulates
        for (int i = 0; i < b->avg_n; i++) {
            sum += b->avg_win[i];
        }
        b->c.status = SS_OK;
        b->c.ever_ok = true;
        b->c.last_ok_ms = now_ms;
        b->c.value = (float)(sum / b->avg_n);
    } else {
        b->c.status = st == SS_OK ? SS_OUT_OF_RANGE : st;
        b->c.errors++;
    }
    float p0 = 0, spread = 0;
    zero_cal_result_t r = zero_cal_feed(&b->cal, ok, p_pa, now_ms, &p0, &spread);
    return finish_cal(b, r, p0, spread);
}

stand_event_t stand_baro_poll(stand_t *s, int ch, uint32_t now_ms)
{
    return finish_cal(&s->baro[ch], zero_cal_poll(&s->baro[ch].cal, now_ms), 0, 0);
}

static float median(const float *v, int n)
{
    float t[STAND_SONAR_MEDIAN];
    memcpy(t, v, sizeof(float) * (size_t)n);
    for (int i = 1; i < n; i++) { // insertion sort, n <= 5
        float x = t[i];
        int j = i - 1;
        while (j >= 0 && t[j] > x) {
            t[j + 1] = t[j];
            j--;
        }
        t[j + 1] = x;
    }
    return (n % 2) ? t[n / 2] : 0.5f * (t[n / 2 - 1] + t[n / 2]);
}

void stand_sonar_sample(stand_t *s, sensor_status_t st, float cm, uint32_t now_ms)
{
    if (st != SS_OK || !isfinite(cm)) {
        s->sonar.status = st == SS_OK ? SS_OUT_OF_RANGE : st;
        s->sonar.errors++;
        // A long outage must not let pre-outage samples dominate the median afterwards.
        if (!stand_chan_fresh(&s->sonar, STAND_SONAR_STALE_MS, now_ms)) {
            s->sonar_win_n = 0;
            s->sonar_win_pos = 0;
        }
        return;
    }
    s->sonar_win[s->sonar_win_pos] = cm;
    s->sonar_win_pos = (uint8_t)((s->sonar_win_pos + 1) % STAND_SONAR_MEDIAN);
    if (s->sonar_win_n < STAND_SONAR_MEDIAN) {
        s->sonar_win_n++;
    }
    s->sonar.status = SS_OK;
    s->sonar.ever_ok = true;
    s->sonar.last_ok_ms = now_ms;
    s->sonar.value = median(s->sonar_win, s->sonar_win_n);
}

stand_zero_start_t stand_zero_start(stand_t *s, int ch, uint32_t now_ms)
{
    baro_t *b = &s->baro[ch];
    if (zero_cal_running(&b->cal)) {
        return STAND_ZERO_BUSY;
    }
    if (!stand_chan_fresh(&b->c, STAND_BARO_STALE_MS, now_ms) || b->c.status != SS_OK) {
        b->cal_view = CAL_FAILED;
        b->cal_err = CAL_ERR_NO_DATA;
        return STAND_ZERO_NO_DATA;
    }
    zero_cal_start(&b->cal, now_ms);
    b->cal_view = CAL_RUNNING;
    b->cal_err = CAL_ERR_NONE;
    return STAND_ZERO_STARTED;
}

bool stand_zero_reset(stand_t *s, int ch)
{
    baro_t *b = &s->baro[ch];
    if (zero_cal_running(&b->cal)) {
        return false;
    }
    b->zero.set = false;
    b->zero.p0_pa = 0;
    b->origin = ZO_NONE;
    b->cal_view = CAL_IDLE;
    b->persist = PERSIST_PENDING;
    return true;
}

void stand_zero_persisted(stand_t *s, int ch, bool ok)
{
    s->baro[ch].persist = ok ? PERSIST_OK : PERSIST_ERROR;
}

bool stand_baro_altitude(const baro_t *b, uint32_t now_ms, float *out_m)
{
    if (!b->zero.set || !stand_chan_fresh(&b->c, STAND_BARO_STALE_MS, now_ms)) {
        return false;
    }
    return altitude_relative_m(b->c.value, b->zero.p0_pa, out_m);
}

void stand_fill_record(const stand_t *s, uint32_t now_ms, uint16_t session, uint32_t t_s,
                       hl_record_t *rec)
{
    memset(rec, 0, sizeof(*rec));
    rec->session = session;
    rec->t_s = t_s;
    for (int ch = 0; ch < STAND_BARO_N; ch++) {
        const baro_t *b = &s->baro[ch];
        bool valid = stand_chan_fresh(&b->c, STAND_BARO_STALE_MS, now_ms);
        float h;
        bool has_h = valid && stand_baro_altitude(b, now_ms, &h);
        rec->p_pa[ch] = valid ? b->c.value : NAN;
        rec->h_m[ch] = has_h ? h : NAN;
        sensor_status_t st = !valid && b->c.status == SS_OK ? SS_STALE : b->c.status;
        rec->flags[ch] = HL_F_MAKE(valid, b->zero.set, st);
    }
}
