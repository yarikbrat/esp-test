#pragma once
// Relative barometric altitude: dh = 44330 * (1 - (p / p0)^0.1903), metres, p and p0 in Pa.
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// BMP581/BMP585 operating range per datasheet: 30..125 kPa.
#define BARO_P_MIN_PA 30000.0f
#define BARO_P_MAX_PA 125000.0f

bool baro_pressure_plausible(float p_pa);

// Returns false and leaves *out_m untouched if p or p0 is not plausible.
bool altitude_relative_m(float p_pa, float p0_pa, float *out_m);

#ifdef __cplusplus
}
#endif
