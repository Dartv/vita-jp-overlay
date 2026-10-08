/* HTTP/1.1 client: one request per connection (Connection: close), or
 * several in turn on a kept-alive one. */
#ifndef VJO_HTTP_H
#define VJO_HTTP_H

#include <stddef.h>

#include "arena.h"
#include "conn.h"

typedef struct {
    const char *method;        /* "POST", "GET" */
    const char *host;
    const char *path;
    const char *content_type;  /* NULL: no Content-Type (GET) */
    const char *extra_headers; /* "Name: value\r\n"... or NULL */
    size_t body_len;           /* 0 without write_body: no body, no Content-Length */
    /* Writes exactly body_len bytes to c; returns 0 or a VJO_E_* code. */
    int (*write_body)(void *ud, VjoConn *c);
    void *ud;
    int connect_timeout_us;    /* 0 = the platform's default */
    int io_timeout_us;         /* each read or write, not the whole reply; 0 = the platform's default */
    int host_port;             /* added to the Host header when not 0 (vjo_http_request
                                * sets it for a port other than 80 / 443) */
    int keep_alive;            /* no "Connection: close" (set by vjo_http_request) */
} VjoHttpRequest;

typedef struct {
    int status;
    char *body;                /* NUL-terminated (arena) */
    size_t body_len;
    int gzip;                  /* Content-Encoding: gzip */
    int keep_alive;            /* the connection can take the next request: an
                                * HTTP/1.1 reply with a framed body, no
                                * "Connection: close", nothing after it */
} VjoHttpResponse;

int vjo_http_send(VjoConn *c, const VjoHttpRequest *req);
/* Reads the full response; the body (up to max_body bytes) lives in the
 * arena. resp->status stays 0 until the status line is read. */
int vjo_http_recv(VjoArena *a, VjoConn *c, size_t max_body, VjoHttpResponse *resp);

#endif
