#pragma once
// Невелика обгортка над NVS: один blob на ключ у просторі імен "stand".
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool kv_init(void);
// Повертає розмір blob, 0 якщо його немає або він не читається.
size_t kv_get(const char *key, uint8_t *buf, size_t cap);
bool kv_set(const char *key, const uint8_t *buf, size_t len);
// Видаляє ключ; відсутній ключ теж вважається успіхом.
bool kv_erase(const char *key);
