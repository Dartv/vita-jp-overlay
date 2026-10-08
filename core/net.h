/* Platform services (sockets, time, randomness) and one HTTP request over
 * them: the transport shared by the Lens/dictionary client and Anki; with a
 * VjoNetPool, connections are kept for the next request to the host. */
#ifndef VJO_NET_H
#define VJO_NET_H

#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "conn.h"
#include "http.h"

/* A kept-alive connection (net.c). */
typedef struct VjoNetSlot VjoNetSlot;

/* Connections kept for reuse, one per host, port and TLS, for the requests
 * of one thread (the slots live in memory the caller provides). */
typedef struct {
    VjoNetSlot *slots;
    int n;
    int idle_us; /* a connection idle longer is closed, not reused */
} VjoNetPool;

typedef struct {
    void *ud;
    /* Opens a TCP connection, giving up after timeout_us (DNS included
     * where the platform can), with reads and writes that each give up
     * after io_timeout_us (0 = the platform's default for either); returns
     * VJO_OK, VJO_E_NET or VJO_E_CANCELLED. */
    int (*connect)(void *ud, const char *host, int port, int timeout_us, int io_timeout_us, VjoConn *out);
    void (*disconnect)(void *ud, VjoConn *c);
    void (*random)(void *ud, void *buf, size_t n);
    uint64_t (*unix_time)(void *ud);
    void (*log)(void *ud, const char *msg); /* optional */
    /* optional: a monotonic clock in microseconds (request phase times in
     * the log) */
    uint64_t (*now_us)(void *ud);
    /* optional: requests reuse connections from it (needs now_us) */
    VjoNetPool *pool;
    /* optional, with pool: a kept connection is to carry a request; returns
     * VJO_OK, VJO_E_NET (the peer closed it) or VJO_E_CANCELLED */
    int (*acquire)(void *ud, VjoConn *c);
    /* optional, with pool: a connection is kept, idle */
    void (*release)(void *ud, VjoConn *c);
    /* optional: raw response bodies (host CLI --record) */
    void (*on_response)(void *ud, const char *host, const char *body, size_t len);
#ifdef VJO_HOST
    /* Plain HTTP over connect()'s stream instead of TLS (host replay of
     * recorded responses); random/unix_time are then unused. Host-only:
     * the Vita build has no way to skip TLS. */
    int plain_http;
#endif
} VjoPlatform;

typedef struct {
    int rc;               /* VJO_OK or VJO_E_* */
    int http_status;      /* when rc == VJO_E_STATUS */
    int tls_error;        /* BearSSL error code when rc == VJO_E_TLS */
    const char *detail;   /* server-provided message, may be NULL */
    int dict;             /* VJO_DICT_* for VJO_STAGE_DICT errors */
} VjoErr;

/* One HTTP request to req->host:port, over TLS when use_tls is set (except
 * host replay: VjoPlatform.plain_http). The response body is in the arena;
 * a non-2xx status is VJO_E_STATUS (resp still filled). With p->pool, a
 * request reuses the host's kept connection (retried once on a new one when
 * that gives no reply: closed meanwhile) and keeps its connection. */
int vjo_http_request(VjoArena *a, const VjoPlatform *p, int port, int use_tls, const VjoHttpRequest *req,
                     size_t max_body, VjoHttpResponse *resp, VjoErr *err);

/* Bytes for n kept connections. */
size_t vjo_net_pool_size(int n);
/* The pool's slots: as many as fit in mem (aligned for any type). */
void vjo_net_pool_init(VjoNetPool *pool, void *mem, size_t size, int idle_us);
/* Opens a kept TLS connection to host:port in p->pool unless one is
 * usable, so the next request skips the handshakes. Returns VJO_OK or the
 * error. */
int vjo_net_warm(const VjoPlatform *p, const char *host, int port, int timeout_us);
/* Closes the kept connections (sockets only: no TLS close_notify). */
void vjo_net_pool_close(const VjoPlatform *p);

#endif
