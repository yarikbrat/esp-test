#include "histlog.h"

#include "crc32.h"

#include <string.h>

#define REC_SIZE ((uint32_t)sizeof(hl_record_t))
#define CRC_SPAN (offsetof(hl_record_t, crc))

static bool rec_blank(const hl_record_t *r)
{
    const uint8_t *b = (const uint8_t *)r;
    for (uint32_t i = 0; i < REC_SIZE; i++) {
        if (b[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

static bool rec_valid_at(const histlog_t *hl, const hl_record_t *r, uint32_t slot)
{
    return r->seq != HL_SEQ_EMPTY && r->seq % hl->capacity == slot &&
           crc32_calc(r, CRC_SPAN) == r->crc;
}

static int read_slot(histlog_t *hl, uint32_t slot, hl_record_t *r)
{
    return hl->fl->read(hl->fl->ctx, slot * REC_SIZE, r, REC_SIZE);
}

int histlog_open(histlog_t *hl, const hl_flash_t *fl, uint32_t n_sectors)
{
    memset(hl, 0, sizeof(*hl));
    if (!fl || n_sectors < 2 || fl->sector_size < REC_SIZE || fl->sector_size % REC_SIZE) {
        return -1;
    }
    hl->fl = fl;
    hl->n_sectors = n_sectors;
    hl->recs_per_sector = fl->sector_size / REC_SIZE;
    hl->capacity = hl->recs_per_sector * n_sectors;

    bool any = false;
    uint32_t max_seq = 0, min_seq = 0;
    for (uint32_t slot = 0; slot < hl->capacity; slot++) {
        hl_record_t r;
        int err = read_slot(hl, slot, &r);
        if (err) {
            hl->last_error = err;
            return err;
        }
        if (rec_blank(&r)) {
            continue;
        }
        if (!rec_valid_at(hl, &r, slot)) {
            hl->corrupt++;
            continue;
        }
        if (!any || r.seq > max_seq) {
            max_seq = r.seq;
        }
        if (!any || r.seq < min_seq) {
            min_seq = r.seq;
        }
        any = true;
    }
    hl->next_seq = any ? max_seq + 1 : 0;
    hl->oldest_seq = any ? min_seq : 0;
    return 0;
}

uint32_t histlog_count(const histlog_t *hl)
{
    return hl->next_seq - hl->oldest_seq;
}

int histlog_append(histlog_t *hl, hl_record_t *rec)
{
    // A slot may hold leftovers of an interrupted write: skip it (a seq gap), within one sector.
    for (uint32_t attempt = 0; attempt < hl->recs_per_sector; attempt++) {
        uint32_t seq = hl->next_seq;
        uint32_t slot = seq % hl->capacity;
        uint32_t off = slot * REC_SIZE;
        int err;

        if (slot % hl->recs_per_sector == 0) {
            err = hl->fl->erase_sector(hl->fl->ctx, off);
            if (err) {
                hl->write_errors++;
                hl->last_error = err;
                return err;
            }
            // Erased seq - capacity .. seq - capacity + rps - 1.
            if (seq + hl->recs_per_sector > hl->capacity) {
                uint32_t survived = seq + hl->recs_per_sector - hl->capacity;
                if (hl->oldest_seq < survived) {
                    hl->oldest_seq = survived;
                }
            }
        } else {
            hl_record_t cur;
            err = read_slot(hl, slot, &cur);
            if (err || !rec_blank(&cur)) {
                if (hl->oldest_seq == hl->next_seq) {
                    hl->oldest_seq++; // keep an empty log empty
                }
                hl->next_seq++;
                hl->write_errors += err ? 1 : 0;
                continue;
            }
        }

        rec->seq = seq;
        rec->crc = crc32_calc(rec, CRC_SPAN);
        hl->next_seq = seq + 1;

        err = hl->fl->write(hl->fl->ctx, off, rec, REC_SIZE);
        hl_record_t back;
        if (!err) {
            err = read_slot(hl, slot, &back);
        }
        if (!err && memcmp(&back, rec, REC_SIZE) != 0) {
            err = -2; // read-back mismatch
        }
        if (err) {
            hl->write_errors++;
            hl->last_error = err;
            return err;
        }
        hl->last_error = 0;
        return 0;
    }
    hl->write_errors++;
    hl->last_error = -3;
    return -3;
}

int histlog_clear(histlog_t *hl)
{
    for (uint32_t s = 0; s < hl->n_sectors; s++) {
        int err = hl->fl->erase_sector(hl->fl->ctx, s * hl->fl->sector_size);
        if (err) {
            hl->last_error = err;
            return err;
        }
    }
    hl->next_seq = 0;
    hl->oldest_seq = 0;
    hl->corrupt = 0;
    hl->last_error = 0;
    return 0;
}

size_t histlog_read_recent(histlog_t *hl, uint32_t before_seq, hl_record_t *out, size_t max)
{
    size_t n = 0;
    uint32_t seq = before_seq < hl->next_seq ? before_seq : hl->next_seq;
    uint32_t budget = (uint32_t)max * 4u + 8u; // gaps are rare; bound the walk
    while (n < max && seq > hl->oldest_seq && budget--) {
        seq--;
        uint32_t slot = seq % hl->capacity;
        hl_record_t r;
        if (read_slot(hl, slot, &r) == 0 && r.seq == seq && rec_valid_at(hl, &r, slot)) {
            out[n++] = r;
        }
    }
    return n;
}
