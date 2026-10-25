#pragma once
// Формат NVS-блобів для налаштувань. Кожна група — окремий блоб із magic, версією і CRC32,
// тож відсутні, обрізані чи пошкоджені записи замінюються типовими. Окремий блоб на барометр не дає
// запису нуля одного каналу зачепити інший.
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
    ST_LOADED = 0, // прочитано і перевірено
    ST_MISSING,    // запису немає, взято типові
    ST_CORRUPT,    // перевірка magic/версії/CRC/діапазону не пройшла, взято типові
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

// buf == NULL або len == 0 означає "запису немає". *out заповнюється завжди.
st_origin_t st_decode_net(const uint8_t *buf, size_t len, netcfg_t *out);
st_origin_t st_decode_zero(const uint8_t *buf, size_t len, uint8_t ch, zero_cfg_t *out);
st_origin_t st_decode_log(const uint8_t *buf, size_t len, log_cfg_t *out);

void log_cfg_default(log_cfg_t *c);

#ifdef __cplusplus
}
#endif
