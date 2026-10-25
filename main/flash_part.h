#pragma once
// бекенд hl_flash_t поверх розділу даних "histlog".
#include <stdint.h>

#include "histlog.h"

// Заповнює *ops і повертає кількість секторів по 4 KB, 0 якщо розділу немає.
uint32_t flash_part_open(hl_flash_t *ops);
