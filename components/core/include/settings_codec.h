#pragma once
// NVS blob format for settings. Each group is a separate blob with magic, version and CRC32,
// so missing, truncated or damaged entries fall back to defaults. One blob per barometer keeps
// writing one channel's zero from touching the other.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "netcfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_INTERVAL_MIN_S     1
#define LOG_INTERVAL_MAX_S     3600
#define LOG_INTERVAL_DEFAULT_S 10

typedef enum {
    ST_LOADED = 0, // read and verified
    ST_MISSING,    // no entry, defaults used
    ST_CORRUPT,    // magic/version/CRC/range check failed, defaults used
} st_origin_t;

typedef struct {
    bool set;
    float p0_pa;
} zero_cfg_t;

typedef struct {
    bool enabled;
    uint16_t interval_s;
} log_cfg_t;

#define ST_NET_BLOB_SIZE  20
#define ST_ZERO_BLOB_SIZE 16
#define ST_LOG_BLOB_SIZE  12

size_t st_encode_net(const netcfg_t *in, uint8_t *buf, size_t cap);
size_t st_encode_zero(const zero_cfg_t *in, uint8_t ch, uint8_t *buf, size_t cap);
size_t st_encode_log(const log_cfg_t *in, uint8_t *buf, size_t cap);

// buf == NULL or len == 0 means "no entry". *out is always filled.
st_origin_t st_decode_net(const uint8_t *buf, size_t len, netcfg_t *out);
st_origin_t st_decode_zero(const uint8_t *buf, size_t len, uint8_t ch, zero_cfg_t *out);
st_origin_t st_decode_log(const uint8_t *buf, size_t len, log_cfg_t *out);

void log_cfg_default(log_cfg_t *c);

#ifdef __cplusplus
}
#endif
