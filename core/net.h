/* Platform services (sockets, time, randomness) and one HTTP request over
 * them: the transport shared by the Lens/dictionary client and Anki. */
#ifndef VJO_NET_H
#define VJO_NET_H

#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "conn.h"
#include "http.h"

typedef struct {
    void *ud;
    /* Opens a TCP connection, giving up after timeout_us (DNS included
     * where the platform can), with reads and writes that each give up
     * after io_timeout_us (0 = the platform's default for either); returns
     * VJO_OK or VJO_E_NET. */
    int (*connect)(void *ud, const char *host, int port, int timeout_us, int io_timeout_us, VjoConn *out);
    void (*disconnect)(void *ud, VjoConn *c);
    void (*random)(void *ud, void *buf, size_t n);
    uint64_t (*unix_time)(void *ud);
    void (*log)(void *ud, const char *msg); /* optional */
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
 * a non-2xx status is VJO_E_STATUS (resp still filled). */
int vjo_http_request(VjoArena *a, const VjoPlatform *p, int port, int use_tls, const VjoHttpRequest *req,
                     size_t max_body, VjoHttpResponse *resp, VjoErr *err);

#endif
