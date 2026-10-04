/* Byte-stream connection used by HTTP; implemented by raw sockets
 * (host/net_posix.c, shell/net_vita.c) and by the TLS layer (tls.c). */
#ifndef VJO_CONN_H
#define VJO_CONN_H

#include <stddef.h>

typedef struct VjoConn {
    void *ctx;
    /* Returns bytes written (> 0) or < 0 on error. May write less than n. */
    int (*send)(void *ctx, const void *p, size_t n);
    /* Returns bytes read (> 0), 0 on orderly EOF, < 0 on error: VJO_E_HTTP
     * when the stream ended without an orderly close (data may be cut). */
    int (*recv)(void *ctx, void *p, size_t n);
} VjoConn;

int vjo_conn_send_all(VjoConn *c, const void *p, size_t n);

/* Result codes shared by the network/core layers. */
enum {
    VJO_OK = 0,
    VJO_E_NET = -100,       /* DNS / connect / socket I/O */
    VJO_E_TLS = -101,       /* handshake or record error */
    VJO_E_HTTP = -102,      /* malformed HTTP response */
    VJO_E_OOM = -103,       /* arena exhausted */
    VJO_E_PARSE = -104,     /* malformed protobuf / JSON body */
    VJO_E_TOO_LARGE = -105, /* response body over the limit */
    VJO_E_STATUS = -106,    /* non-2xx HTTP status */
    VJO_E_NO_KEY = -107,    /* dictionary API key not configured */
    VJO_E_SOURCE = -108,    /* JPEG source read failed */
};

#endif
