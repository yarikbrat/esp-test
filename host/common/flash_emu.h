#pragma once
// Емулятор NOR flash для тестів на ПК та емулятора стенда: стирання ставить 0xFF, запис лише скидає біти.
// Підтримує імітацію вимкнення живлення та збоїв.
#include <stdbool.h>
#include <stdint.h>

#include "histlog.h"

typedef struct {
    uint8_t *mem;
    uint32_t size;
    uint32_t sector_size;
    uint32_t erase_count[256]; // стирань на кожен сектор
    int cut_after_bytes;       // >= 0: наступний запис збереже лише стільки байтів і завершиться помилкою
    bool fail_erase;
    bool fail_write;
    const char *backing_file;  // якщо задано, кожна зміна одразу записується в цей файл
    int fd;
} flash_emu_t;

void flash_emu_init(flash_emu_t *fe, uint32_t size, uint32_t sector_size, const char *backing_file);
void flash_emu_free(flash_emu_t *fe);
hl_flash_t flash_emu_ops(flash_emu_t *fe);
