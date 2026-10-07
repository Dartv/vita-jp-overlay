#include "net.h"

#include <stdarg.h>
#include <string.h>

#include "port.h"
#include "tls.h"

static void plog(const VjoPlatform *p, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    if (!p->log)
        return;
    va_start(ap, fmt);
    vjo_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    p->log(p->ud, line);
}

/* The response body ends up at the arena position the TLS state occupied,
 * so the ~20 KB of TLS buffers are not retained. */
int vjo_http_request(VjoArena *a, const VjoPlatform *p, int port, int use_tls, const VjoHttpRequest *req,
                     size_t max_body, VjoHttpResponse *resp, VjoErr *err)
{
    size_t mark = vjo_arena_mark(a);
    VjoHttpRequest r = *req;
    VjoConn raw, *conn;
    VjoTls *tls = NULL;
    void *tbuf = NULL;
    uint8_t seed[32];
    uint32_t days, secs;
    int rc;

    memset(resp, 0, sizeof(*resp));
    if (port != (use_tls ? 443 : 80))
        r.host_port = port;
#ifdef VJO_HOST
    if (p->plain_http)
        use_tls = 0;
#endif
    if (use_tls) {
        tls = (VjoTls *)vjo_arena_alloc(a, sizeof(VjoTls));
        tbuf = vjo_arena_alloc(a, VJO_TLS_MIN_BUF);
        if (!tls || !tbuf) {
            vjo_arena_release(a, mark);
            return err->rc = VJO_E_OOM;
        }
    }
    rc = p->connect(p->ud, req->host, port, req->connect_timeout_us, req->io_timeout_us, &raw);
    if (rc) {
        vjo_arena_release(a, mark);
        return err->rc = VJO_E_NET;
    }
    if (!use_tls) {
        conn = &raw;
        rc = VJO_OK;
    } else {
        p->random(p->ud, seed, sizeof(seed));
        vjo_tls_time_from_unix(p->unix_time(p->ud), &days, &secs);
        rc = vjo_tls_open(tls, &raw, req->host, tbuf, VJO_TLS_MIN_BUF, days, secs, seed, sizeof(seed));
        conn = &tls->conn;
    }
    if (rc == VJO_OK)
        rc = vjo_http_send(conn, &r);
    if (rc == VJO_OK)
        rc = vjo_http_recv(a, conn, max_body, resp);
    if (use_tls) {
        if (rc == VJO_E_TLS || rc == VJO_E_NET)
            err->tls_error = vjo_tls_last_error(tls);
        if (err->tls_error && rc == VJO_E_NET)
            rc = VJO_E_TLS;
        vjo_tls_close(tls);
    }
    plog(p, "%s:%d %s -> rc=%d status=%d tls=%d body=%lu", req->host, port, req->path, rc,
         resp->status, err->tls_error, (unsigned long)resp->body_len);
    p->disconnect(p->ud, &raw);
    if (rc) {
        vjo_arena_release(a, mark);
        return err->rc = rc;
    }
    /* Compact: move the body down over the TLS state (dst <= body, and
     * nothing is allocated in between, so memmove is safe). */
    {
        uint8_t *dst;
        vjo_arena_release(a, mark);
        dst = (uint8_t *)vjo_arena_alloc(a, resp->body_len + 1);
        memmove(dst, resp->body, resp->body_len + 1);
        resp->body = (char *)dst;
    }
    if (p->on_response)
        p->on_response(p->ud, req->host, resp->body, resp->body_len);
    if (resp->status < 200 || resp->status > 299) {
        err->http_status = resp->status;
        return err->rc = VJO_E_STATUS;
    }
    return err->rc = VJO_OK;
}

