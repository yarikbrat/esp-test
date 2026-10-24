#pragma once
// Bench state model: three channels, staleness, zero procedure. Not thread-safe: callers hold
// one lock around every call, and web code works on a copy (snapshot) taken under that lock.
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
#define STAND_BARO_AVG_N    20 // moving average of pressure: 1 s at the 20 Hz poll rate

// Driver-reported status of one read attempt; values are stored in log records, keep stable.
typedef enum {
    SS_INIT = 0,       // not read yet
    SS_OK = 1,
    SS_NO_ECHO = 2,    // HC-SR04: ECHO never came or never ended
    SS_OUT_OF_RANGE = 3,
    SS_NOT_FOUND = 4,  // I2C: no ACK / wrong chip id
    SS_COMM_ERROR = 5, // I2C: bus error or timeout
    SS_REINIT = 6,     // driver is re-initialising the sensor
    SS_STALE = 7,      // log only: last read was fine but is older than the stale limit
} sensor_status_t;

typedef enum {
    ZO_NONE = 0,
    ZO_RESTORED, // loaded from flash at boot
    ZO_NEW,      // set in this session
} zero_origin_t;

typedef enum {
    CAL_IDLE = 0,
    CAL_RUNNING,
    CAL_OK,
    CAL_FAILED,
} cal_view_t;

typedef enum {
    CAL_ERR_NONE = 0,
    CAL_ERR_NO_DATA, // no fresh valid pressure when the button was pressed
    CAL_ERR_TIMEOUT, // fewer than ZERO_CAL_MIN_SAMPLES valid samples within ZERO_CAL_MAX_MS
} cal_err_t;

typedef enum {
    PERSIST_NONE = 0,
    PERSIST_PENDING,
    PERSIST_OK,
    PERSIST_ERROR,
} persist_t;

typedef struct {
    sensor_status_t status; // last read attempt
    bool ever_ok;
    uint32_t last_ok_ms;
    float value; // last valid value: Pa for barometers, cm for the sonar
    uint32_t errors;
} chan_t;

typedef struct {
    chan_t c; // c.value is the moving average of the last STAND_BARO_AVG_N valid samples
    float avg_win[STAND_BARO_AVG_N];
    uint8_t avg_n, avg_pos;
    zero_cfg_t zero;
    zero_origin_t origin;
    st_origin_t load_origin; // how the zero entry looked at boot
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
    STAND_EV_ZERO_DONE,   // persist baro[ch].zero, then call stand_zero_persisted()
    STAND_EV_ZERO_FAILED, // previous zero kept
} stand_event_t;

typedef enum {
    STAND_ZERO_STARTED = 0,
    STAND_ZERO_BUSY,
    STAND_ZERO_NO_DATA,
} stand_zero_start_t;

void stand_init(stand_t *s);

void stand_load_zero(stand_t *s, int ch, const zero_cfg_t *z, st_origin_t origin);

stand_event_t stand_baro_sample(stand_t *s, int ch, sensor_status_t st, float p_pa, uint32_t now_ms);

// Calibration timeout check for a sensor that stopped producing samples.
stand_event_t stand_baro_poll(stand_t *s, int ch, uint32_t now_ms);

void stand_sonar_sample(stand_t *s, sensor_status_t st, float cm, uint32_t now_ms);

stand_zero_start_t stand_zero_start(stand_t *s, int ch, uint32_t now_ms);

// Clears the zero in RAM; caller persists {set=false} and reports via stand_zero_persisted().
// Returns false while a calibration is running.
bool stand_zero_reset(stand_t *s, int ch);

void stand_zero_persisted(stand_t *s, int ch, bool ok);

bool stand_chan_fresh(const chan_t *c, uint32_t stale_ms, uint32_t now_ms);

// Altitude from the latest fresh pressure; false if no zero or no fresh data.
bool stand_baro_altitude(const baro_t *b, uint32_t now_ms, float *out_m);

// Fills both channels from one state, so the two values belong to the same log cycle.
void stand_fill_record(const stand_t *s, uint32_t now_ms, uint16_t session, uint32_t t_s,
                       hl_record_t *rec);

#ifdef __cplusplus
}
#endif
