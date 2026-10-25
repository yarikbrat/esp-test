#pragma once
// Розбір application/x-www-form-urlencoded (тіло POST або рядок запиту).
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Декодує значення key (%XX, '+' -> пробіл) в out.
// False, якщо ключа немає, значення задовге для out або має хибний чи NUL %-код.
bool form_get(const char *src, const char *key, char *out, size_t out_len);

// Ціле в [min, max]; false, якщо немає або не число.
bool form_get_long(const char *src, const char *key, long min, long max, long *out);

#ifdef __cplusplus
}
#endif
