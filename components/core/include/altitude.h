#pragma once
// Відносна барометрична висота: dh = 44330 * (1 - (p / p0)^0.1903), у метрах, p і p0 у Па.
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Робочий діапазон BMP581/BMP585 за даташитом: 30..125 кПа.
#define BARO_P_MIN_PA 30000.0f
#define BARO_P_MAX_PA 125000.0f

bool baro_pressure_plausible(float p_pa);

// Повертає false і не змінює *out_m, якщо p або p0 неправдоподібні.
bool altitude_relative_m(float p_pa, float p0_pa, float *out_m);

#ifdef __cplusplus
}
#endif
