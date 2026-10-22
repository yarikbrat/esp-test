#include "settings_codec.h"

#include "altitude.h"
#include "crc32.h"

#include <string.h>

// Layout: [magic u16][version u8][arg u8][payload...][crc32 u32], little-endian.
#define MAGIC_NET  0x454Eu // "NE"
#define MAGIC_ZERO 0x305Au // "Z0"
#define MAGIC_LOG  0x474Cu // "LG"
#define VERSION    1

static void put16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *b, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
}
static uint16_t get16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t get32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static size_t seal(uint8_t *buf, size_t size, uint16_t magic, uint8_t arg)
{
    put16(buf, magic);
    buf[2] = VERSION;
    buf[3] = arg;
    put32(buf + size - 4, crc32_calc(buf, size - 4));
    return size;
}

static bool check(const uint8_t *buf, size_t len, size_t size, uint16_t magic, uint8_t arg)
{
    return len == size && get16(buf) == magic && buf[2] == VERSION && buf[3] == arg &&
           get32(buf + size - 4) == crc32_calc(buf, size - 4);
}

size_t st_encode_net(const netcfg_t *in, uint8_t *buf, size_t cap)
{
    if (cap < ST_NET_BLOB_SIZE) {
        return 0;
    }
    put32(buf + 4, in->ip);
    put32(buf + 8, in->mask);
    put32(buf + 12, in->gw);
    return seal(buf, ST_NET_BLOB_SIZE, MAGIC_NET, 0);
}

st_origin_t st_decode_net(const uint8_t *buf, size_t len, netcfg_t *out)
{
    netcfg_default(out);
    if (!buf || len == 0) {
        return ST_MISSING;
    }
    if (!check(buf, len, ST_NET_BLOB_SIZE, MAGIC_NET, 0)) {
        return ST_CORRUPT;
    }
    netcfg_t c = {.ip = get32(buf + 4), .mask = get32(buf + 8), .gw = get32(buf + 12)};
    if (netcfg_validate(&c) != NETCFG_OK) {
        return ST_CORRUPT;
    }
    *out = c;
    return ST_LOADED;
}

size_t st_encode_zero(const zero_cfg_t *in, uint8_t ch, uint8_t *buf, size_t cap)
{
    if (cap < ST_ZERO_BLOB_SIZE) {
        return 0;
    }
    uint32_t bits;
    float p0 = in->set ? in->p0_pa : 0.0f;
    memcpy(&bits, &p0, sizeof(bits));
    buf[4] = in->set ? 1 : 0;
    buf[5] = buf[6] = buf[7] = 0;
    put32(buf + 8, bits);
    return seal(buf, ST_ZERO_BLOB_SIZE, MAGIC_ZERO, ch);
}

st_origin_t st_decode_zero(const uint8_t *buf, size_t len, uint8_t ch, zero_cfg_t *out)
{
    out->set = false;
    out->p0_pa = 0.0f;
    if (!buf || len == 0) {
        return ST_MISSING;
    }
    // The channel number is part of the header, so one channel's blob never loads as the other's.
    if (!check(buf, len, ST_ZERO_BLOB_SIZE, MAGIC_ZERO, ch) || buf[4] > 1) {
        return ST_CORRUPT;
    }
    if (buf[4] == 1) {
        uint32_t bits = get32(buf + 8);
        float p0;
        memcpy(&p0, &bits, sizeof(p0));
        if (!baro_pressure_plausible(p0)) {
            return ST_CORRUPT;
        }
        out->set = true;
        out->p0_pa = p0;
    }
    return ST_LOADED;
}

void log_cfg_default(log_cfg_t *c)
{
    c->enabled = false;
    c->interval_s = LOG_INTERVAL_DEFAULT_S;
}

size_t st_encode_log(const log_cfg_t *in, uint8_t *buf, size_t cap)
{
    if (cap < ST_LOG_BLOB_SIZE) {
        return 0;
    }
    buf[4] = in->enabled ? 1 : 0;
    buf[5] = 0;
    put16(buf + 6, in->interval_s);
    return seal(buf, ST_LOG_BLOB_SIZE, MAGIC_LOG, 0);
}

st_origin_t st_decode_log(const uint8_t *buf, size_t len, log_cfg_t *out)
{
    log_cfg_default(out);
    if (!buf || len == 0) {
        return ST_MISSING;
    }
    if (!check(buf, len, ST_LOG_BLOB_SIZE, MAGIC_LOG, 0) || buf[4] > 1) {
        return ST_CORRUPT;
    }
    uint16_t iv = get16(buf + 6);
    if (iv < LOG_INTERVAL_MIN_S || iv > LOG_INTERVAL_MAX_S) {
        return ST_CORRUPT;
    }
    out->enabled = buf[4] == 1;
    out->interval_s = iv;
    return ST_LOADED;
}
