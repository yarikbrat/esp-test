#define _GNU_SOURCE // pread/pwrite при -std=c11
#include "flash_emu.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Записує назад лише змінений діапазон, тож додавання 32 байтів коштує 32 байти файлового I/O.
static void persist(flash_emu_t *fe, uint32_t off, uint32_t len)
{
    if (fe->fd < 0) {
        return;
    }
    if (pwrite(fe->fd, fe->mem + off, len, (off_t)off) != (ssize_t)len) {
        perror("flash_emu pwrite");
    }
}

void flash_emu_init(flash_emu_t *fe, uint32_t size, uint32_t sector_size, const char *backing_file)
{
    memset(fe, 0, sizeof(*fe));
    fe->mem = malloc(size);
    fe->size = size;
    fe->sector_size = sector_size;
    fe->cut_after_bytes = -1;
    fe->backing_file = backing_file;
    fe->fd = -1;
    memset(fe->mem, 0xFF, size);
    if (backing_file) {
        fe->fd = open(backing_file, O_RDWR | O_CREAT, 0644);
        if (fe->fd < 0) {
            perror("flash_emu open");
        } else {
            ssize_t got = pread(fe->fd, fe->mem, size, 0);
            if (got >= 0 && got < (ssize_t)size) {
                persist(fe, 0, size); // новий або закороткий файл: один раз записати стерте зображення
            }
        }
    }
}

void flash_emu_free(flash_emu_t *fe)
{
    if (fe->fd >= 0) {
        close(fe->fd);
        fe->fd = -1;
    }
    free(fe->mem);
    fe->mem = NULL;
}

static int emu_read(void *ctx, uint32_t off, void *dst, uint32_t len)
{
    flash_emu_t *fe = ctx;
    if (off + len > fe->size) {
        return -10;
    }
    memcpy(dst, fe->mem + off, len);
    return 0;
}

static int emu_write(void *ctx, uint32_t off, const void *src, uint32_t len)
{
    flash_emu_t *fe = ctx;
    if (off + len > fe->size) {
        return -10;
    }
    if (fe->fail_write) {
        return -11;
    }
    uint32_t n = len;
    int ret = 0;
    if (fe->cut_after_bytes >= 0) {
        n = (uint32_t)fe->cut_after_bytes < len ? (uint32_t)fe->cut_after_bytes : len;
        fe->cut_after_bytes = -1;
        ret = -12;
    }
    const uint8_t *s = src;
    for (uint32_t i = 0; i < n; i++) {
        fe->mem[off + i] &= s[i]; // NOR: біти змінюються лише 1 -> 0
    }
    persist(fe, off, n);
    return ret;
}

static int emu_erase(void *ctx, uint32_t off)
{
    flash_emu_t *fe = ctx;
    if (off % fe->sector_size || off + fe->sector_size > fe->size) {
        return -10;
    }
    if (fe->fail_erase) {
        return -13;
    }
    memset(fe->mem + off, 0xFF, fe->sector_size);
    persist(fe, off, fe->sector_size);
    uint32_t idx = off / fe->sector_size;
    if (idx < sizeof(fe->erase_count) / sizeof(fe->erase_count[0])) {
        fe->erase_count[idx]++;
    }
    return 0;
}

hl_flash_t flash_emu_ops(flash_emu_t *fe)
{
    hl_flash_t ops = {
        .ctx = fe,
        .sector_size = fe->sector_size,
        .read = emu_read,
        .write = emu_write,
        .erase_sector = emu_erase,
    };
    return ops;
}
