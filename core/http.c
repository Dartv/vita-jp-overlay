#include "http.h"

#include <stdarg.h>
#include <string.h>

#include "port.h"
#include "utf.h"

int vjo_conn_send_all(VjoConn *c, const void *p, size_t n)
{
    const uint8_t *u = (const uint8_t *)p;
    while (n) {
        int w = c->send(c->ctx, u, n);
        if (w <= 0)
            return VJO_E_NET;
        u += w;
        n -= (size_t)w;
    }
    return VJO_OK;
}

/* Appends to the request head; once it overflows, *n stays at cap. Output
 * that fills the buffer counts as an overflow, whether vsnprintf returns
 * the length it wanted (C) or the length it wrote. */
static void head_add(char *head, size_t cap, size_t *n, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (*n >= cap)
        return;
    va_start(ap, fmt);
    k = vjo_vsnprintf(head + *n, cap - *n, fmt, ap);
    va_end(ap);
    *n = k < 0 || (size_t)k >= cap - *n - 1 ? cap : *n + (size_t)k;
}

int vjo_http_send(VjoConn *c, const VjoHttpRequest *req)
{
    char head[1024];
    size_t n = 0;
    int rc;
    head_add(head, sizeof(head), &n, "%s %s HTTP/1.1\r\nHost: %s", req->method, req->path, req->host);
    if (req->host_port)
        head_add(head, sizeof(head), &n, ":%d", req->host_port);
    head_add(head, sizeof(head), &n, "\r\n");
    if (req->content_type)
        head_add(head, sizeof(head), &n, "Content-Type: %s\r\n", req->content_type);
    if (req->body_len || req->write_body)
        head_add(head, sizeof(head), &n, "Content-Length: %lu\r\n", (unsigned long)req->body_len);
    if (!req->keep_alive)
        head_add(head, sizeof(head), &n, "Connection: close\r\n");
    head_add(head, sizeof(head), &n, "%s\r\n", req->extra_headers ? req->extra_headers : "");
    if (n >= sizeof(head))
        return VJO_E_HTTP;
    rc = vjo_conn_send_all(c, head, n);
    if (rc)
        return rc;
    return req->write_body ? req->write_body(req->ud, c) : VJO_OK;
}

#define MAX_TRAILERS 32 /* more: the connection is not kept */

/* Buffered reader over a connection. */
typedef struct {
    VjoConn *c;
    uint8_t buf[2048];
    size_t pos, len;
    int eof, err;
} Rd;

static int rd_fill(Rd *r)
{
    int n;
    if (r->eof || r->err)
        return 0;
    n = r->c->recv(r->c->ctx, r->buf, sizeof(r->buf));
    if (n < 0) {
        /* A VJO_E_* code from the connection is kept, other errors are I/O. */
        r->err = n == VJO_E_HTTP ? VJO_E_HTTP : VJO_E_NET;
        return 0;
    }
    if (n == 0) {
        r->eof = 1;
        return 0;
    }
    r->pos = 0;
    r->len = (size_t)n;
    return n;
}

static int rd_byte(Rd *r)
{
    if (r->pos >= r->len && !rd_fill(r))
        return -1;
    return r->buf[r->pos++];
}

/* Reads one CRLF-terminated line (without CRLF). Returns length or -1. */
static int rd_line(Rd *r, char *out, size_t cap)
{
    size_t n = 0;
    for (;;) {
        int ch = rd_byte(r);
        if (ch < 0)
            return n ? (int)n : -1;
        if (ch == '\n')
            break;
        if (n + 1 < cap)
            out[n++] = (char)ch;
    }
    if (n && out[n - 1] == '\r')
        n--;
    out[n] = '\0';
    return (int)n;
}

static const char *hval(const char *line, size_t name_len)
{
    const char *v = line + name_len;
    while (*v == ' ' || *v == '\t')
        v++;
    return v;
}

static int body_put(VjoBuf *b, size_t max, const void *p, size_t n)
{
    if (b->len + n > max)
        return VJO_E_TOO_LARGE;
    if (vjo_buf_append(b, p, n) < 0)
        return VJO_E_OOM;
    return VJO_OK;
}

static int read_exact(Rd *r, VjoBuf *b, size_t max, size_t n)
{
    while (n) {
        size_t avail;
        int rc;
        if (r->pos >= r->len && !rd_fill(r))
            return r->err ? r->err : VJO_E_HTTP;
        avail = r->len - r->pos;
        if (avail > n)
            avail = n;
        rc = body_put(b, max, r->buf + r->pos, avail);
        if (rc)
            return rc;
        r->pos += avail;
        n -= avail;
    }
    return VJO_OK;
}

int vjo_http_recv(VjoArena *a, VjoConn *c, size_t max_body, VjoHttpResponse *resp)
{
    Rd *r;
    char line[512];
    size_t content_length = 0;
    int have_length = 0, chunked = 0, conn_close = 0, status = 0, rc = VJO_OK;
    VjoBuf body;

    memset(resp, 0, sizeof(*resp));
    /* The reader lives in the arena rather than on (small) thread stacks;
     * the body buffer then grows at the arena top after it. */
    r = (Rd *)vjo_arena_alloc(a, sizeof(Rd));
    if (!r)
        return VJO_E_OOM;
    /* Arena memory is reused and not zeroed: initialize every field. */
    r->c = c;
    r->pos = 0;
    r->len = 0;
    r->eof = 0;
    r->err = 0;

    if (rd_line(r, line, sizeof(line)) < 0)
        return r->err ? r->err : VJO_E_HTTP;
    /* "HTTP/1.x NNN ..." */
    if (!vjo_ieq_prefix(line, "http/1.") || strlen(line) < 12 || line[8] != ' ')
        return VJO_E_HTTP;
    for (int k = 9; k < 12; k++) {
        if (line[k] < '0' || line[k] > '9')
            return VJO_E_HTTP;
        status = status * 10 + (line[k] - '0');
    }
    resp->status = status;
    conn_close = line[7] != '1'; /* HTTP/1.0 */

    for (;;) {
        int n = rd_line(r, line, sizeof(line));
        if (n < 0)
            return r->err ? r->err : VJO_E_HTTP;
        if (n == 0)
            break;
        if (vjo_ieq_prefix(line, "content-length:")) {
            const char *v = hval(line, 15);
            if (*v < '0' || *v > '9')
                return VJO_E_HTTP;
            /* Stops growing past max_body, so long digit strings cannot overflow. */
            for (content_length = 0; *v >= '0' && *v <= '9'; v++)
                if (content_length <= max_body)
                    content_length = content_length * 10 + (size_t)(*v - '0');
            have_length = 1;
        } else if (vjo_ieq_prefix(line, "transfer-encoding:")) {
            if (vjo_ieq_prefix(hval(line, 18), "chunked"))
                chunked = 1;
        } else if (vjo_ieq_prefix(line, "content-encoding:")) {
            if (vjo_ieq_prefix(hval(line, 17), "gzip"))
                resp->gzip = 1;
        } else if (vjo_ieq_prefix(line, "connection:")) {
            if (vjo_ieq_prefix(hval(line, 11), "close"))
                conn_close = 1;
        }
    }

    vjo_buf_init(&body, a);

    /* These never have a body (RFC 9112 6.3), whatever the headers say. */
    if ((status >= 100 && status < 200) || status == 204 || status == 304) {
        chunked = 0;
        have_length = 1;
        content_length = 0;
    }
    if (chunked) {
        for (;;) {
            size_t sz = 0;
            const char *p;
            if (rd_line(r, line, sizeof(line)) < 0) {
                rc = r->err ? r->err : VJO_E_HTTP;
                break;
            }
            for (p = line; *p; p++) {
                int d;
                if (*p >= '0' && *p <= '9')
                    d = *p - '0';
                else if (*p >= 'a' && *p <= 'f')
                    d = *p - 'a' + 10;
                else if (*p >= 'A' && *p <= 'F')
                    d = *p - 'A' + 10;
                else
                    break;
                sz = sz * 16 + (size_t)d;
                if (sz > max_body) {
                    rc = VJO_E_TOO_LARGE;
                    break;
                }
            }
            if (rc)
                break;
            if (sz == 0) {
                /* Trailers (ignored) up to the empty line that ends them. */
                int k = 1;
                for (int n = 0; k > 0 && n < MAX_TRAILERS; n++)
                    k = rd_line(r, line, sizeof(line));
                conn_close |= k != 0;
                break;
            }
            rc = read_exact(r, &body, max_body, sz);
            if (rc)
                break;
            rd_line(r, line, sizeof(line)); /* CRLF after chunk */
        }
    } else if (have_length) {
        if (content_length > max_body)
            rc = VJO_E_TOO_LARGE;
        else
            rc = read_exact(r, &body, max_body, content_length);
    } else {
        /* Read until close. */
        conn_close = 1;
        for (;;) {
            if (r->pos >= r->len && !rd_fill(r))
                break;
            rc = body_put(&body, max_body, r->buf + r->pos, r->len - r->pos);
            if (rc)
                break;
            r->pos = r->len;
        }
        if (r->err)
            rc = r->err;
    }
    if (rc)
        return rc;
    resp->body = vjo_buf_cstr(&body);
    if (!resp->body)
        return VJO_E_OOM;
    resp->body_len = body.len;
    resp->keep_alive = !conn_close && r->pos == r->len;
    return VJO_OK;
}
