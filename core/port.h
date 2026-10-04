/* Portability shims for the core. On the Vita the SceShell plugin is linked
 * without newlib, so formatted output goes through sceClib. The remaining
 * libc functions the core uses (memcpy, memmove, memset, memcmp, strlen,
 * strncmp, strchr) are provided by shell/libc_shim.c there. */
#ifndef VJO_PORT_H
#define VJO_PORT_H

#include <stdarg.h>
#include <stddef.h>

#if defined(__vita__) && !defined(VJO_HOST)
int sceClibVsnprintf(char *dst, size_t dst_max_size, const char *fmt, va_list args);
int sceClibSnprintf(char *dst, size_t dst_max_size, const char *fmt, ...);
#define vjo_vsnprintf sceClibVsnprintf
#define vjo_snprintf sceClibSnprintf
#else
#include <stdio.h>
#define vjo_vsnprintf vsnprintf
#define vjo_snprintf snprintf
#endif

#endif
