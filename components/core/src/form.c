#include "form.h"

#include <stdlib.h>
#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool form_get(const char *src, const char *key, char *out, size_t out_len)
{
    if (!src || !key || !out || out_len == 0) {
        return false;
    }
    size_t klen = strlen(key);
    const char *s = src;
    while (*s) {
        const char *amp = strchr(s, '&');
        const char *pair_end = amp ? amp : s + strlen(s);
        const char *eq = memchr(s, '=', (size_t)(pair_end - s));
        const char *name_end = eq ? eq : pair_end;
        if ((size_t)(name_end - s) == klen && memcmp(s, key, klen) == 0) {
            size_t o = 0;
            const char *v = eq ? eq + 1 : pair_end;
            while (v < pair_end) {
                char c = *v++;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    if (pair_end - v < 2) {
                        return false;
                    }
                    int hi = hexval(v[0]), lo = hexval(v[1]);
                    if (hi < 0 || lo < 0) {
                        return false;
                    }
                    c = (char)(hi * 16 + lo);
                    v += 2;
                    if (c == '\0') {
                        return false;
                    }
                }
                if (o + 1 >= out_len) {
                    return false;
                }
                out[o++] = c;
            }
            out[o] = '\0';
            return true;
        }
        if (!amp) {
            break;
        }
        s = amp + 1;
    }
    return false;
}

bool form_get_long(const char *src, const char *key, long min, long max, long *out)
{
    char buf[16];
    if (!form_get(src, key, buf, sizeof(buf)) || buf[0] == '\0') {
        return false;
    }
    char *end;
    long v = strtol(buf, &end, 10);
    if (*end != '\0' || v < min || v > max) {
        return false;
    }
    *out = v;
    return true;
}
