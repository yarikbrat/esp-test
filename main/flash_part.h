#pragma once
// hl_flash_t backend over the "histlog" data partition.
#include <stdint.h>

#include "histlog.h"

// Fills *ops and returns the number of 4 KB sectors, 0 if the partition is missing.
uint32_t flash_part_open(hl_flash_t *ops);
