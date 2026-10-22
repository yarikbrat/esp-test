#include "crc32.h"

// Bitwise, no table: records are short and a 1 KB table is not worth the RAM.
uint32_t crc32_calc(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}
