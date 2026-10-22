#pragma once
// application/x-www-form-urlencoded parsing (POST body or query string).
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Decodes the value of key (%XX, '+' -> space) into out.
// False if missing, too long for out, or contains an invalid or NUL %-escape.
bool form_get(const char *src, const char *key, char *out, size_t out_len);

// Integer in [min, max]; false if missing or not a number.
bool form_get_long(const char *src, const char *key, long min, long max, long *out);

#ifdef __cplusplus
}
#endif
