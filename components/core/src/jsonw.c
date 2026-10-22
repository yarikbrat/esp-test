#include "jsonw.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void jw_init(jw_t *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = cap == 0;
    if (cap) {
        buf[0] = '\0';
    }
}

static void put(jw_t *w, const char *s, size_t n)
{
    if (w->overflow || w->len + n >= w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

void jw_raw(jw_t *w, const char *s)
{
    put(w, s, strlen(s));
}

void jw_printf(jw_t *w, const char *fmt, ...)
{
    if (w->overflow) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->len) {
        w->overflow = true;
        w->buf[w->len] = '\0';
        return;
    }
    w->len += (size_t)n;
}

void jw_str(jw_t *w, const char *s)
{
    if (!s) {
        jw_raw(w, "null");
        return;
    }
    put(w, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            char e[2] = {'\\', (char)c};
            put(w, e, 2);
        } else if (c < 0x20) {
            char e[8];
            snprintf(e, sizeof(e), "\\u%04x", c);
            put(w, e, 6);
        } else {
            put(w, (const char *)&c, 1); // UTF-8 passes through unchanged
        }
    }
    put(w, "\"", 1);
}

void jw_num(jw_t *w, double v, int decimals)
{
    if (!isfinite(v)) {
        jw_raw(w, "null");
        return;
    }
    jw_printf(w, "%.*f", decimals, v);
}

void jw_bool(jw_t *w, bool v)
{
    jw_raw(w, v ? "true" : "false");
}
