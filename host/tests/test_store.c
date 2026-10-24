#include "check.h"
#include "flash_emu.h"
#include "histlog.h"
#include "settings_codec.h"

#include <string.h>

#define SECTOR 4096u
#define RPS    (SECTOR / 32u)

static hl_record_t make_rec(uint32_t i)
{
    hl_record_t r;
    memset(&r, 0, sizeof(r));
    r.session = 7;
    r.t_s = i * 10;
    r.p_pa[0] = 100000.0f + (float)i;
    r.p_pa[1] = NAN; // second channel failed
    r.h_m[0] = 1.5f;
    r.h_m[1] = NAN;
    r.flags[0] = HL_F_MAKE(true, true, 0);
    r.flags[1] = HL_F_MAKE(false, false, 3);
    return r;
}

static void append_n(histlog_t *hl, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        hl_record_t r = make_rec(hl->next_seq);
        CHECK_EQ_INT(histlog_append(hl, &r), 0);
    }
}

void test_histlog(void)
{
    flash_emu_t fe;
    flash_emu_init(&fe, 8 * SECTOR, SECTOR, NULL);
    hl_flash_t ops = flash_emu_ops(&fe);
    histlog_t hl;
    hl_record_t out[600];

    CHECK_EQ_INT(histlog_open(&hl, &ops, 1), -1);

    // Empty log.
    CHECK_EQ_INT(histlog_open(&hl, &ops, 4), 0);
    CHECK_EQ_INT(hl.capacity, 4 * RPS);
    CHECK_EQ_INT(histlog_count(&hl), 0);
    CHECK_EQ_INT(histlog_read_recent(&hl, HL_SEQ_EMPTY, out, 10, NULL), 0);

    // Write, then "reboot".
    append_n(&hl, 200);
    CHECK_EQ_INT(histlog_count(&hl), 200);
    CHECK_EQ_INT(histlog_open(&hl, &ops, 4), 0);
    CHECK_EQ_INT(hl.next_seq, 200);
    CHECK_EQ_INT(hl.oldest_seq, 0);
    CHECK_EQ_INT(hl.corrupt, 0);

    size_t n = histlog_read_recent(&hl, HL_SEQ_EMPTY, out, 3, NULL);
    CHECK_EQ_INT(n, 3);
    CHECK_EQ_INT(out[0].seq, 199);
    CHECK_EQ_INT(out[2].seq, 197);
    CHECK_NEAR(out[0].p_pa[0], 100199.0, 0.01);
    CHECK(isnan(out[0].p_pa[1]) && isnan(out[0].h_m[1]));
    CHECK_EQ_INT(HL_F_STATUS(out[0].flags[1]), 3);
    CHECK(!(out[0].flags[1] & HL_F_VALID));
    n = histlog_read_recent(&hl, 2, out, 10, NULL); // page "before seq 2"
    CHECK_EQ_INT(n, 2);
    CHECK_EQ_INT(out[0].seq, 1);
    CHECK_EQ_INT(out[1].seq, 0);

    // Three full laps over 4 sectors; guaranteed capacity is capacity - RPS.
    uint32_t total = 3 * 4 * RPS;
    while (hl.next_seq < total) {
        uint32_t written = hl.next_seq + 1;
        append_n(&hl, 1);
        CHECK(histlog_count(&hl) <= hl.capacity);
        CHECK(histlog_count(&hl) >= (written < hl.capacity - RPS ? written : hl.capacity - RPS));
    }
    for (int s = 0; s < 4; s++) {
        CHECK_EQ_INT(fe.erase_count[s], 3); // even wear
    }
    uint32_t count_before = histlog_count(&hl);
    uint32_t oldest_before = hl.oldest_seq;
    CHECK_EQ_INT(histlog_open(&hl, &ops, 4), 0);
    CHECK_EQ_INT(hl.next_seq, total);
    CHECK_EQ_INT(hl.oldest_seq, oldest_before);
    CHECK_EQ_INT(histlog_count(&hl), count_before);
    n = histlog_read_recent(&hl, HL_SEQ_EMPTY, out, 600, NULL);
    CHECK_EQ_INT(n, histlog_count(&hl));
    CHECK_EQ_INT(out[0].seq, total - 1);
    CHECK_EQ_INT(out[n - 1].seq, hl.oldest_seq);

    // Power cut mid-write, away from a sector start (a sector start is erased before writing).
    append_n(&hl, 5);
    uint32_t cut_seq = hl.next_seq;
    fe.cut_after_bytes = 16;
    hl_record_t r = make_rec(9999);
    CHECK(histlog_append(&hl, &r) != 0);
    CHECK_EQ_INT(hl.write_errors, 1);
    CHECK_EQ_INT(histlog_open(&hl, &ops, 4), 0);
    CHECK_EQ_INT(hl.corrupt, 1);
    CHECK_EQ_INT(hl.next_seq, cut_seq); // the torn record does not count
    r = make_rec(10000);
    CHECK_EQ_INT(histlog_append(&hl, &r), 0); // dirty slot is skipped
    CHECK_EQ_INT(r.seq, cut_seq + 1);
    n = histlog_read_recent(&hl, HL_SEQ_EMPTY, out, 2, NULL);
    CHECK_EQ_INT(n, 2);
    CHECK_EQ_INT(out[0].seq, cut_seq + 1);
    CHECK_EQ_INT(out[1].seq, cut_seq - 1); // gap skipped

    // A long run of gaps (failed writes burn seqs) must not end pagination early.
    uint32_t before_gap = hl.next_seq - 1;
    fe.fail_write = true;
    for (int i = 0; i < 100 && hl.next_seq % RPS != 0; i++) {
        r = make_rec(0);
        histlog_append(&hl, &r);
    }
    fe.fail_write = false;
    append_n(&hl, 1);
    uint32_t cursor = HL_SEQ_EMPTY;
    n = histlog_read_recent(&hl, HL_SEQ_EMPTY, out, 2, &cursor);
    CHECK_EQ_INT(n, 1); // only the record after the gap fits in the walk budget
    CHECK(cursor != HL_SEQ_EMPTY);
    size_t pages = 0;
    while (cursor != HL_SEQ_EMPTY && pages++ < 20) {
        n = histlog_read_recent(&hl, cursor, out, 2, &cursor);
        if (n) {
            break;
        }
    }
    CHECK(n > 0 && out[0].seq == before_gap); // older history still reachable
    n = histlog_read_recent(&hl, 1 + hl.oldest_seq, out, 5, &cursor);
    CHECK(n <= 1 && cursor == HL_SEQ_EMPTY);

    // Erase failure: visible error, log keeps working afterwards.
    while (hl.next_seq % RPS != 0) {
        append_n(&hl, 1);
    }
    fe.fail_erase = true;
    uint32_t errs = hl.write_errors;
    r = make_rec(0);
    CHECK(histlog_append(&hl, &r) != 0);
    CHECK_EQ_INT(hl.write_errors, errs + 1);
    CHECK(hl.last_error != 0);
    fe.fail_erase = false;
    CHECK_EQ_INT(histlog_append(&hl, &r), 0);
    CHECK_EQ_INT(hl.last_error, 0);

    // A failed clear is counted as a write error (the marker lands on a sector start here).
    while (hl.next_seq % RPS != 0) {
        append_n(&hl, 1);
    }
    fe.fail_erase = true;
    errs = hl.write_errors;
    CHECK(histlog_clear(&hl) != 0);
    CHECK_EQ_INT(hl.write_errors, errs + 1);
    fe.fail_erase = false;

    // Write failure is reported.
    fe.fail_write = true;
    r = make_rec(0);
    CHECK(histlog_append(&hl, &r) != 0);
    fe.fail_write = false;

    // Clear: a marker record, at most one sector erase, survives a reboot.
    append_n(&hl, 5);
    uint32_t erases = 0, erases_after = 0;
    for (uint32_t i = 0; i < 4; i++) {
        erases += fe.erase_count[i];
    }
    uint32_t mark_seq = hl.next_seq;
    CHECK_EQ_INT(histlog_clear(&hl), 0);
    for (uint32_t i = 0; i < 4; i++) {
        erases_after += fe.erase_count[i];
    }
    CHECK(erases_after - erases <= 1);
    CHECK_EQ_INT(histlog_count(&hl), 0);
    CHECK_EQ_INT(histlog_read_recent(&hl, HL_SEQ_EMPTY, out, 10, NULL), 0);
    CHECK_EQ_INT(histlog_open(&hl, &ops, 4), 0);
    CHECK_EQ_INT(histlog_count(&hl), 0);
    CHECK_EQ_INT(hl.next_seq, mark_seq + 1);
    r = make_rec(1);
    CHECK_EQ_INT(histlog_append(&hl, &r), 0);
    CHECK_EQ_INT(r.seq, mark_seq + 1);
    CHECK_EQ_INT(histlog_count(&hl), 1);
    CHECK_EQ_INT(histlog_open(&hl, &ops, 4), 0);
    CHECK_EQ_INT(histlog_count(&hl), 1);
    n = histlog_read_recent(&hl, HL_SEQ_EMPTY, out, 10, NULL);
    CHECK(n == 1 && out[0].seq == mark_seq + 1);

    // Two clears in a row, then a full lap over the markers.
    CHECK_EQ_INT(histlog_clear(&hl), 0);
    CHECK_EQ_INT(histlog_clear(&hl), 0);
    CHECK_EQ_INT(histlog_open(&hl, &ops, 4), 0);
    CHECK_EQ_INT(histlog_count(&hl), 0);
    append_n(&hl, 4 * RPS + 3);
    CHECK_EQ_INT(histlog_open(&hl, &ops, 4), 0);
    CHECK(histlog_count(&hl) > 3 * RPS && histlog_count(&hl) <= 4 * RPS); // markers fell off the ring
    n = histlog_read_recent(&hl, HL_SEQ_EMPTY, out, 600, NULL);
    CHECK_EQ_INT(n, histlog_count(&hl));
    for (size_t i = 0; i < n; i++) {
        CHECK((out[i].flags[0] & HL_F_CLEAR) == 0);
    }

    // A clear that cannot write its marker fails and keeps the records.
    uint32_t before_fail = histlog_count(&hl);
    fe.fail_write = true;
    CHECK(histlog_clear(&hl) != 0);
    fe.fail_write = false;
    CHECK_EQ_INT(histlog_open(&hl, &ops, 4), 0);
    CHECK_EQ_INT(histlog_count(&hl), before_fail);

    // Changing the ring size without clearing must not resurrect records in the wrong slots.
    append_n(&hl, 3 * RPS);
    CHECK_EQ_INT(histlog_open(&hl, &ops, 2), 0);
    n = histlog_read_recent(&hl, HL_SEQ_EMPTY, out, 600, NULL);
    for (size_t i = 0; i < n; i++) {
        CHECK(out[i].seq % hl.capacity == out[i].seq % (2 * RPS));
    }

    // Garbage everywhere (neither 0xFF nor valid records).
    memset(fe.mem, 0x5A, fe.size);
    CHECK_EQ_INT(histlog_open(&hl, &ops, 4), 0);
    CHECK_EQ_INT(histlog_count(&hl), 0);
    CHECK_EQ_INT(hl.corrupt, 4 * RPS);
    r = make_rec(1);
    CHECK_EQ_INT(histlog_append(&hl, &r), 0); // slot 0 starts a sector, gets erased
    CHECK_EQ_INT(histlog_count(&hl), 1);

    // Formatting garbage: every slot blank again, nothing corrupt after a reboot; a failed erase is reported.
    memset(fe.mem, 0x5A, fe.size);
    CHECK_EQ_INT(histlog_open(&hl, &ops, 4), 0);
    fe.fail_erase = true;
    CHECK(histlog_format(&hl) != 0);
    CHECK(hl.last_error != 0);
    fe.fail_erase = false;
    CHECK_EQ_INT(histlog_format(&hl), 0);
    CHECK_EQ_INT(histlog_open(&hl, &ops, 4), 0);
    CHECK_EQ_INT(hl.corrupt, 0);
    CHECK_EQ_INT(histlog_count(&hl), 0);
    append_n(&hl, 3);
    CHECK_EQ_INT(histlog_count(&hl), 3);

    flash_emu_free(&fe);
}

void test_settings(void)
{
    uint8_t buf[32];

    netcfg_t net = {.ip = 0x0A000005u, .mask = 0xFF000000u, .gw = 0x0A000001u}, got;
    size_t n = st_encode_net(&net, buf, sizeof(buf));
    CHECK_EQ_INT(n, ST_NET_BLOB_SIZE);
    CHECK_EQ_INT(st_decode_net(buf, n, &got), ST_LOADED);
    CHECK(memcmp(&net, &got, sizeof(net)) == 0);
    CHECK_EQ_INT(st_decode_net(NULL, 0, &got), ST_MISSING);
    CHECK(got.ip == NETCFG_DEFAULT_IP);
    for (size_t i = 0; i < n; i++) { // any single bit flip is detected
        buf[i] ^= 0x01;
        CHECK_EQ_INT(st_decode_net(buf, n, &got), ST_CORRUPT);
        CHECK(got.ip == NETCFG_DEFAULT_IP);
        buf[i] ^= 0x01;
    }
    CHECK_EQ_INT(st_decode_net(buf, n - 1, &got), ST_CORRUPT);
    netcfg_t bad = {.ip = 0x0A000000u, .mask = 0xFF000000u, .gw = 0};
    n = st_encode_net(&bad, buf, sizeof(buf)); // valid CRC, invalid address
    CHECK_EQ_INT(st_decode_net(buf, n, &got), ST_CORRUPT);
    CHECK_EQ_INT(st_encode_net(&net, buf, 4), 0);

    // Zero: channels are independent; one channel's blob never loads as the other's.
    zero_cfg_t z = {.set = true, .p0_pa = 100123.5f}, zg;
    n = st_encode_zero(&z, 0, buf, sizeof(buf));
    CHECK_EQ_INT(st_decode_zero(buf, n, 0, &zg), ST_LOADED);
    CHECK(zg.set && zg.p0_pa == 100123.5f);
    CHECK_EQ_INT(st_decode_zero(buf, n, 1, &zg), ST_CORRUPT);
    CHECK(!zg.set);
    zero_cfg_t reset = {.set = false};
    n = st_encode_zero(&reset, 1, buf, sizeof(buf));
    CHECK_EQ_INT(st_decode_zero(buf, n, 1, &zg), ST_LOADED);
    CHECK(!zg.set);
    zero_cfg_t absurd = {.set = true, .p0_pa = 5.0f};
    n = st_encode_zero(&absurd, 0, buf, sizeof(buf));
    CHECK_EQ_INT(st_decode_zero(buf, n, 0, &zg), ST_CORRUPT);
    CHECK_EQ_INT(st_decode_zero(NULL, 0, 0, &zg), ST_MISSING);

    log_cfg_t lc = {.enabled = true, .interval_s = 5}, lg;
    n = st_encode_log(&lc, buf, sizeof(buf));
    CHECK_EQ_INT(st_decode_log(buf, n, &lg), ST_LOADED);
    CHECK(lg.enabled && lg.interval_s == 5);
    lc.interval_s = 0;
    n = st_encode_log(&lc, buf, sizeof(buf));
    CHECK_EQ_INT(st_decode_log(buf, n, &lg), ST_CORRUPT);
    CHECK(!lg.enabled && lg.interval_s == LOG_INTERVAL_DEFAULT_S);
    lc.interval_s = 3601;
    n = st_encode_log(&lc, buf, sizeof(buf));
    CHECK_EQ_INT(st_decode_log(buf, n, &lg), ST_CORRUPT);
}
