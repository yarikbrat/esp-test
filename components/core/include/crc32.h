#pragma once
// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320) for stored records.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t crc32_calc(const void *data, size_t len);

#ifdef __cplusplus
}
#endif
