/* POSIX implementation of VjoPlatform for the host CLI and tests. */
#include "net_posix.h"
#include "replay.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/random.h> /* getentropy */
#endif

/* A peer that closes mid-request must not kill the CLI with SIGPIPE
 * (Linux; macOS sets SO_NOSIGPIPE on the socket instead). */
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

static int sock_send(void *ctx, const void *p, size_t n)
{
    ssize_t w = send((int)(intptr_t)ctx, p, n, SEND_FLAGS);
    return w > 0 ? (int)w : -1;
}

static int sock_recv(void *ctx, void *p, size_t n)
{
    ssize_t r = recv((int)(intptr_t)ctx, p, n, 0);
    return r >= 0 ? (int)r : -1;
}

static int posix_connect(void *ud, const char *host, int port, VjoConn *out)
{
    struct addrinfo hints, *res, *ai;
    char portstr[8];
    int fd = -1;
    (void)ud;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return VJO_E_NET;
    for (ai = res; ai; ai = ai->ai_next) {
        struct timeval tv = {20, 0};
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
        {
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        }
#endif
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return VJO_E_NET;
    out->ctx = (void *)(intptr_t)fd;
    out->send = sock_send;
    out->recv = sock_recv;
    return VJO_OK;
}

static void posix_disconnect(void *ud, VjoConn *c)
{
    (void)ud;
    close((int)(intptr_t)c->ctx);
}

static void posix_random(void *ud, void *buf, size_t n)
{
    (void)ud;
    /* getentropy: at most 256 bytes per call; glibc and macOS both have it. */
    for (size_t off = 0; off < n; off += 256) {
        size_t k = n - off < 256 ? n - off : 256;
        if (getentropy((char *)buf + off, k) != 0)
            abort();
    }
}

static uint64_t posix_time(void *ud)
{
    (void)ud;
    return (uint64_t)time(NULL);
}

static void posix_log(void *ud, const char *msg)
{
    PosixPlatform *pp = (PosixPlatform *)ud;
    if (pp->verbose)
        fprintf(stderr, "[vjo] %s\n", msg);
}

static void posix_on_response(void *ud, const char *host, const char *body, size_t len)
{
    PosixPlatform *pp = (PosixPlatform *)ud;
    char path[1024];
    FILE *f;
    if (!pp->record_dir)
        return;
    snprintf(path, sizeof(path), "%s/%s", pp->record_dir, vjo_fixture_file(host));
    f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(body, 1, len, f);
    fclose(f);
}

void posix_platform_init(PosixPlatform *pp, VjoPlatform *p)
{
    memset(p, 0, sizeof(*p));
    p->ud = pp;
    p->connect = posix_connect;
    p->disconnect = posix_disconnect;
    p->random = posix_random;
    p->unix_time = posix_time;
    p->log = posix_log;
    p->on_response = posix_on_response;
}
