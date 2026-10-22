#pragma once
// Append-only JSON text builder over a fixed buffer; overflow is sticky and checked once at the end.
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} jw_t;

void jw_init(jw_t *w, char *buf, size_t cap);
void jw_raw(jw_t *w, const char *s);
void jw_printf(jw_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void jw_str(jw_t *w, const char *s); // quoted and escaped; NULL -> null
void jw_num(jw_t *w, double v, int decimals); // non-finite -> null
void jw_bool(jw_t *w, bool v);

#ifdef __cplusplus
}
#endif
