#ifndef VJO_NET_POSIX_H
#define VJO_NET_POSIX_H

#include "client.h"

typedef struct {
    int verbose;
    const char *record_dir; /* save raw responses here when set */
} PosixPlatform;

void posix_platform_init(PosixPlatform *pp, VjoPlatform *p);

#endif
