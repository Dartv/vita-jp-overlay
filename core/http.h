/* HTTP/1.1 client: one request per connection (Connection: close). */
#ifndef VJO_HTTP_H
#define VJO_HTTP_H

#include <stddef.h>

#include "arena.h"
#include "conn.h"

typedef struct {
    const char *method;        /* "POST" */
    const char *host;
    const char *path;
    const char *content_type;
    const char *extra_headers; /* "Name: value\r\n"... or NULL */
    size_t body_len;
    /* Writes exactly body_len bytes to c; returns 0 or a VJO_E_* code. */
    int (*write_body)(void *ud, VjoConn *c);
    void *ud;
    int connect_timeout_us;    /* 0 = the platform's default */
} VjoHttpRequest;

typedef struct {
    int status;
    char *body;                /* NUL-terminated (arena) */
    size_t body_len;
    int gzip;                  /* Content-Encoding: gzip */
} VjoHttpResponse;

int vjo_http_send(VjoConn *c, const VjoHttpRequest *req);
/* Reads the full response; the body (up to max_body bytes) lives in the arena. */
int vjo_http_recv(VjoArena *a, VjoConn *c, size_t max_body, VjoHttpResponse *resp);

#endif
