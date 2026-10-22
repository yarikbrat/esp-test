#include "altitude.h"

#include <math.h>

bool baro_pressure_plausible(float p_pa)
{
    return isfinite(p_pa) && p_pa >= BARO_P_MIN_PA && p_pa <= BARO_P_MAX_PA;
}

bool altitude_relative_m(float p_pa, float p0_pa, float *out_m)
{
    if (!out_m || !baro_pressure_plausible(p_pa) || !baro_pressure_plausible(p0_pa)) {
        return false;
    }
    *out_m = 44330.0f * (1.0f - powf(p_pa / p0_pa, 0.1903f));
    return true;
}
