#include "utf.h"

#include <string.h>

uint32_t vjo_utf8_next(const char *str, size_t len, size_t *i)
{
    const uint8_t *s = (const uint8_t *)str;
    size_t p = *i;
    uint32_t c = s[p], cp;
    int n;

    if (c < 0x80) {
        *i = p + 1;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        n = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 3;
        cp = c & 0x07;
    } else {
        *i = p + 1;
        return 0xFFFD;
    }
    if (p + (size_t)n >= len) {
        *i = p + 1;
        return 0xFFFD;
    }
    for (int k = 1; k <= n; k++) {
        uint8_t cc = s[p + (size_t)k];
        if ((cc & 0xC0) != 0x80) {
            *i = p + 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *i = p + 1 + (size_t)n;
    return cp;
}

int vjo_utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

char *vjo_java_trim(char *s)
{
    size_t n = strlen(s);
    while (n && (uint8_t)s[n - 1] <= 0x20)
        s[--n] = '\0';
    while (*s && (uint8_t)*s <= 0x20)
        s++;
    return s;
}
