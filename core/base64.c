#include "base64.h"

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void vjo_base64_encode(const uint8_t *in, size_t n, char *out)
{
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        *out++ = b64[v >> 18];
        *out++ = b64[(v >> 12) & 63];
        *out++ = b64[(v >> 6) & 63];
        *out++ = b64[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n)
            v |= (uint32_t)in[i + 1] << 8;
        *out++ = b64[v >> 18];
        *out++ = b64[(v >> 12) & 63];
        *out++ = i + 1 < n ? b64[(v >> 6) & 63] : '=';
        *out++ = '=';
    }
}
