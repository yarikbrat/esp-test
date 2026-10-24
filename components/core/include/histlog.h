#pragma once
// Ring log of barometer history in NOR flash. Not thread-safe: the owner serialises access.
// Record seq lives in slot seq % capacity. A sector is erased when its first slot is written,
// so every sector is erased once per lap (even wear). Appending to erased flash causes no wear,
// so each record is written immediately; a power cut loses at most the record being written.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HL_CHANNELS  2
#define HL_SEQ_EMPTY 0xFFFFFFFFu

// flags[ch] bits
#define HL_F_VALID 0x01u // pressure valid
#define HL_F_ZERO  0x02u // zero set, altitude defined
#define HL_F_CLEAR 0x04u // flags[0] of a clear marker: this record and everything older is cleared
#define HL_F_STATUS(f) (((f) >> 4) & 0x0Fu)
#define HL_F_MAKE(valid, zero, status) \
    (uint8_t)(((valid) ? HL_F_VALID : 0u) | ((zero) ? HL_F_ZERO : 0u) | (((status) & 0x0Fu) << 4))

typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint16_t session;           // boot counter
    uint8_t flags[HL_CHANNELS]; // 0 = BMP585, 1 = BMP581
    uint32_t t_s;               // seconds since session start
    float p_pa[HL_CHANNELS];    // NAN if invalid
    float h_m[HL_CHANNELS];     // NAN if no zero or invalid pressure
    uint32_t crc;               // CRC32 of all preceding fields
} hl_record_t;

_Static_assert(sizeof(hl_record_t) == 32, "hl_record_t must stay 32 bytes");

// Flash backend: esp_partition on target, RAM emulator on host. Functions return 0 on success.
typedef struct {
    void *ctx;
    uint32_t sector_size;
    int (*read)(void *ctx, uint32_t off, void *dst, uint32_t len);
    int (*write)(void *ctx, uint32_t off, const void *src, uint32_t len);
    int (*erase_sector)(void *ctx, uint32_t off);
} hl_flash_t;

typedef struct {
    const hl_flash_t *fl;
    uint32_t n_sectors;
    uint32_t recs_per_sector;
    uint32_t capacity;   // records in the ring
    uint32_t next_seq;
    uint32_t oldest_seq; // == next_seq when empty
    uint32_t write_errors;
    uint32_t corrupt;    // damaged slots found by the last scan
    int last_error;      // last flash error code, 0 if none
} histlog_t;

// Opens the log over n_sectors and scans it for the newest record.
int histlog_open(histlog_t *hl, const hl_flash_t *fl, uint32_t n_sectors);

// Appends a record; fills rec->seq and rec->crc.
int histlog_append(histlog_t *hl, hl_record_t *rec);

// Clears logically by appending a marker: no mass erase, so flash stays free for other tasks.
int histlog_clear(histlog_t *hl);

uint32_t histlog_count(const histlog_t *hl);

// Reads up to max records with seq < before_seq, newest first (HL_SEQ_EMPTY = from newest).
// *next_before (optional) is the cursor for the next page, HL_SEQ_EMPTY when nothing older is left;
// it comes from the walk itself, so a run of gaps never ends pagination early.
size_t histlog_read_recent(histlog_t *hl, uint32_t before_seq, hl_record_t *out, size_t max,
                           uint32_t *next_before);

#ifdef __cplusplus
}
#endif
