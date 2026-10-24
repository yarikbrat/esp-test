#pragma once
// HC-SR04: TRIG pulse, ECHO edges timestamped in a GPIO interrupt. Non-blocking: trigger, then
// collect the result on a later cycle (at least SONAR_CYCLE_MS later).
#include <stdbool.h>

#include "stand.h"

#define SONAR_CYCLE_MS 100 // datasheet asks for >= 60 ms between measurements
#define SONAR_MIN_CM   2.0f
#define SONAR_MAX_CM   400.0f

bool sonar_init(void);
void sonar_trigger(void);
// Result of the last trigger; *cm is set only on SS_OK.
sensor_status_t sonar_collect(float *cm);
