/* SceShell plugins run without newlib: route the few libc functions the core
 * and BearSSL use to SceLibKernel's sceClib. */
#include <psp2/kernel/clib.h>
#include <stddef.h>

void *memcpy(void *d, const void *s, size_t n) { return sceClibMemcpy(d, s, n); }
void *memmove(void *d, const void *s, size_t n) { return sceClibMemmove(d, s, n); }
void *memset(void *d, int c, size_t n) { return sceClibMemset(d, c, n); }
int memcmp(const void *a, const void *b, size_t n) { return sceClibMemcmp(a, b, n); }
void *memchr(const void *s, int c, size_t n) { return sceClibMemchr(s, c, n); }
size_t strlen(const char *s) { return sceClibStrnlen(s, 0x7fffffff); }
int strcmp(const char *a, const char *b) { return sceClibStrcmp(a, b); }
int strncmp(const char *a, const char *b, size_t n) { return sceClibStrncmp(a, b, n); }
char *strchr(const char *s, int c) { return sceClibStrchr(s, c); }
char *strstr(const char *h, const char *n) { return sceClibStrstr(h, n); }
