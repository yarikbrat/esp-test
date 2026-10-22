#pragma once
// NOR flash emulator for host tests and the bench emulator: erase sets 0xFF, write only clears bits.
// Supports power-cut and failure injection.
#include <stdbool.h>
#include <stdint.h>

#include "histlog.h"

typedef struct {
    uint8_t *mem;
    uint32_t size;
    uint32_t sector_size;
    uint32_t erase_count[256]; // erases per sector
    int cut_after_bytes;       // >= 0: next write stores only this many bytes and fails
    bool fail_erase;
    bool fail_write;
    const char *backing_file;  // if set, every change is written through to this file
    int fd;
} flash_emu_t;

void flash_emu_init(flash_emu_t *fe, uint32_t size, uint32_t sector_size, const char *backing_file);
void flash_emu_free(flash_emu_t *fe);
hl_flash_t flash_emu_ops(flash_emu_t *fe);
