#pragma once
// Побудова тексту JSON лише дописуванням у фіксований буфер; переповнення запам'ятовується і перевіряється раз у кінці.
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
void jw_str(jw_t *w, const char *s); // у лапках, з екрануванням; NULL -> null
void jw_num(jw_t *w, double v, int decimals); // нескінченне чи NaN -> null
void jw_bool(jw_t *w, bool v);

#ifdef __cplusplus
}
#endif
