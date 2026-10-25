#pragma once
// HC-SR04: імпульс TRIG, фронти ECHO отримують мітку часу в перериванні GPIO. Без блокування: запуск, а
// результат забираємо в одному з наступних циклів (не раніше ніж через SONAR_CYCLE_MS).
#include <stdbool.h>

#include "stand.h"

#define SONAR_CYCLE_MS 100 // за datasheet між вимірюваннями має бути >= 60 ms
#define SONAR_MIN_CM   2.0f
#define SONAR_MAX_CM   400.0f

bool sonar_init(void);
void sonar_trigger(void);
// Результат останнього запуску; *cm записується лише при SS_OK.
sensor_status_t sonar_collect(float *cm);
