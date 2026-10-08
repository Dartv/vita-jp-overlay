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

static uint64_t now(const VjoPlatform *p)
{
    return p->now_us ? p->now_us(p->ud) : 0;
}

/* Request phase times for the log: connect and TLS are 0 on a kept
 * connection. */
typedef struct {
    uint64_t start, connected, secured;
    int kept;
} Phases;

static int tls_start(const VjoPlatform *p, VjoTls *tls, VjoConn *raw, const char *host, void *buf)
{
    uint8_t seed[32];
    uint32_t days, secs;
    p->random(p->ud, seed, sizeof(seed));
    vjo_tls_time_from_unix(p->unix_time(p->ud), &days, &secs);
    return vjo_tls_open(tls, raw, host, buf, VJO_TLS_MIN_BUF, days, secs, seed, sizeof(seed));
}

/* Sends the request and reads the reply over conn (TLS: tls, else NULL). */
static int exchange(VjoArena *a, VjoConn *conn, VjoTls *tls, const VjoHttpRequest *r, size_t max_body,
                    VjoHttpResponse *resp, VjoErr *err)
{
    int rc = vjo_http_send(conn, r);
    if (rc == VJO_OK)
        rc = vjo_http_recv(a, conn, max_body, resp);
    if (tls) {
        if (rc == VJO_E_TLS || rc == VJO_E_NET)
            err->tls_error = vjo_tls_last_error(tls);
        if (err->tls_error && rc == VJO_E_NET)
            rc = VJO_E_TLS;
    }
    return rc;
}

/* One request on its own connection, closed after it. The TLS state is in
 * the arena (released by the caller). */
static int single_request(VjoArena *a, const VjoPlatform *p, int port, int use_tls, const VjoHttpRequest *r,
                          size_t max_body, VjoHttpResponse *resp, VjoErr *err, Phases *ph)
{
    VjoConn raw, *conn = &raw;
    VjoTls *tls = NULL;
    void *tbuf = NULL;
    int rc;

    if (use_tls) {
        tls = (VjoTls *)vjo_arena_alloc(a, sizeof(VjoTls));
        tbuf = vjo_arena_alloc(a, VJO_TLS_MIN_BUF);
        if (!tls || !tbuf)
            return VJO_E_OOM;
    }
    rc = p->connect(p->ud, r->host, port, r->connect_timeout_us, r->io_timeout_us, &raw);
    ph->connected = ph->secured = now(p);
    if (rc)
        return rc;
    if (use_tls) {
        rc = tls_start(p, tls, &raw, r->host, tbuf);
        conn = &tls->conn;
        ph->secured = now(p);
    }
    if (rc == VJO_OK)
        rc = exchange(a, conn, tls, r, max_body, resp, err);
    else if (tls)
        err->tls_error = vjo_tls_last_error(tls);
    if (tls)
        vjo_tls_close(tls);
    p->disconnect(p->ud, &raw);
    return rc;
}

/* ---- kept connections (p->pool) ---- */

struct VjoNetSlot {
    char host[64];
    int port, use_tls, open;
    uint64_t used_us; /* now_us when its last request ended */
    VjoConn raw;
    VjoTls tls;
    uint8_t buf[VJO_TLS_MIN_BUF];
};

static int pool_usable(const VjoPlatform *p, const char *host)
{
    return p->pool && p->pool->n && p->now_us && strlen(host) < sizeof(p->pool->slots[0].host);
}

static VjoConn *slot_conn(VjoNetSlot *s)
{
    return s->use_tls ? &s->tls.conn : &s->raw;
}

static void slot_close(const VjoPlatform *p, VjoNetSlot *s)
{
    if (s->open)
        p->disconnect(p->ud, &s->raw);
    s->open = 0;
}

/* The kept connection to host:port, acquired (*out = NULL: none usable).
 * One idle longer than max_idle_us, or closed by the peer, is closed.
 * Returns VJO_OK or VJO_E_CANCELLED. */
static int slot_acquire(const VjoPlatform *p, const char *host, int port, int use_tls, uint64_t max_idle_us,
                        VjoNetSlot **out)
{
    VjoNetPool *pool = p->pool;
    *out = NULL;
    for (int i = 0; i < pool->n; i++) {
        VjoNetSlot *s = &pool->slots[i];
        int rc;
        if (!s->open || s->port != port || s->use_tls != use_tls || strcmp(s->host, host))
            continue;
        if (now(p) - s->used_us > max_idle_us) {
            slot_close(p, s);
            return VJO_OK;
        }
        rc = p->acquire ? p->acquire(p->ud, &s->raw) : VJO_OK;
        if (rc == VJO_E_CANCELLED)
            return rc;
        if (rc) {
            plog(p, "%s:%d: kept connection closed by the server", host, port);
            slot_close(p, s);
            return VJO_OK;
        }
        *out = s;
        return VJO_OK;
    }
    return VJO_OK;
}

/* A new connection to host:port in a free slot, else the least recently
 * used one. It is acquired (as connect leaves it). */
static int slot_open(const VjoPlatform *p, const char *host, int port, int use_tls, int timeout_us,
                     int io_timeout_us, VjoErr *err, Phases *ph, VjoNetSlot **out)
{
    VjoNetPool *pool = p->pool;
    VjoNetSlot *s = &pool->slots[0];
    int rc;
    for (int i = 1; i < pool->n && s->open; i++)
        if (!pool->slots[i].open || pool->slots[i].used_us < s->used_us)
            s = &pool->slots[i];
    slot_close(p, s);
    rc = p->connect(p->ud, host, port, timeout_us, io_timeout_us, &s->raw);
    ph->connected = ph->secured = now(p);
    if (rc)
        return rc;
    if (use_tls) {
        rc = tls_start(p, &s->tls, &s->raw, host, s->buf);
        ph->secured = now(p);
        if (rc) {
            err->tls_error = vjo_tls_last_error(&s->tls);
            p->disconnect(p->ud, &s->raw);
            return rc;
        }
    }
    vjo_snprintf(s->host, sizeof(s->host), "%s", host);
    s->port = port;
    s->use_tls = use_tls;
    s->open = 1;
    s->used_us = ph->secured;
    *out = s;
    return VJO_OK;
}

/* The slot's request is over: kept for the next one, or closed. */
static void slot_release(const VjoPlatform *p, VjoNetSlot *s, int keep)
{
    if (!keep) {
        slot_close(p, s);
        return;
    }
    s->used_us = now(p);
    if (p->release)
        p->release(p->ud, &s->raw);
}

static int slot_exchange(VjoArena *a, VjoNetSlot *s, const VjoHttpRequest *r, size_t max_body,
                         VjoHttpResponse *resp, VjoErr *err)
{
    return exchange(a, slot_conn(s), s->use_tls ? &s->tls : NULL, r, max_body, resp, err);
}

static int pooled_request(VjoArena *a, const VjoPlatform *p, int port, int use_tls, const VjoHttpRequest *r,
                          size_t max_body, VjoHttpResponse *resp, VjoErr *err, Phases *ph)
{
    size_t mark = vjo_arena_mark(a);
    VjoNetSlot *s;
    int rc = slot_acquire(p, r->host, port, use_tls, (uint64_t)p->pool->idle_us, &s);

    if (rc)
        return rc;
    if (s) {
        ph->kept = 1;
        rc = slot_exchange(a, s, r, max_body, resp, err);
        if (rc == VJO_OK || resp->status) {
            slot_release(p, s, rc == VJO_OK && resp->keep_alive);
            return rc;
        }
        /* No reply: the server dropped the connection meanwhile (or the
         * request was cancelled: then the new one fails to connect). */
        plog(p, "%s:%d: no reply on the kept connection (rc=%d)", r->host, port, rc);
        slot_close(p, s);
        vjo_arena_release(a, mark);
        memset(resp, 0, sizeof(*resp));
        err->tls_error = 0;
        ph->kept = 0;
        ph->start = now(p);
    }
    rc = slot_open(p, r->host, port, use_tls, r->connect_timeout_us, r->io_timeout_us, err, ph, &s);
    if (rc)
        return rc;
    rc = slot_exchange(a, s, r, max_body, resp, err);
    slot_release(p, s, rc == VJO_OK && resp->keep_alive);
    return rc;
}

size_t vjo_net_pool_size(int n)
{
    return (size_t)n * sizeof(VjoNetSlot);
}

void vjo_net_pool_init(VjoNetPool *pool, void *mem, size_t size, int idle_us)
{
    pool->slots = (VjoNetSlot *)mem;
    pool->n = mem ? (int)(size / sizeof(VjoNetSlot)) : 0;
    pool->idle_us = idle_us;
    for (int i = 0; i < pool->n; i++)
        pool->slots[i].open = 0;
}

int vjo_net_warm(const VjoPlatform *p, const char *host, int port, int timeout_us)
{
    VjoNetSlot *s;
    VjoErr err;
    Phases ph;
    int rc;

    if (!pool_usable(p, host))
        return VJO_OK;
    /* One past half its idle time is replaced: the request it is for may
     * come a while later. */
    rc = slot_acquire(p, host, port, 1, (uint64_t)p->pool->idle_us / 2, &s);
    if (rc)
        return rc;
    if (!s) {
        memset(&err, 0, sizeof(err));
        ph.start = now(p);
        rc = slot_open(p, host, port, 1, timeout_us, 0, &err, &ph, &s);
        plog(p, "%s:%d: connection ready rc=%d tls=%d (connect %d ms, tls %d ms)", host, port, rc, err.tls_error,
             (int)((ph.connected - ph.start) / 1000), (int)((ph.secured - ph.connected) / 1000));
        if (rc)
            return rc;
    }
    slot_release(p, s, 1);
    return VJO_OK;
}

void vjo_net_pool_close(const VjoPlatform *p)
{
    if (!p->pool)
        return;
    for (int i = 0; i < p->pool->n; i++)
        slot_close(p, &p->pool->slots[i]);
}

/* The response body ends up at the arena position the request's state
 * (a TLS state too, without a pool) occupied, so that is not retained. */
int vjo_http_request(VjoArena *a, const VjoPlatform *p, int port, int use_tls, const VjoHttpRequest *req,
                     size_t max_body, VjoHttpResponse *resp, VjoErr *err)
{
    size_t mark = vjo_arena_mark(a);
    VjoHttpRequest r = *req;
    Phases ph;
    uint64_t t_end;
    char phases[64];
    int rc;

    memset(resp, 0, sizeof(*resp));
    memset(&ph, 0, sizeof(ph));
    ph.start = ph.connected = ph.secured = now(p);
    if (port != (use_tls ? 443 : 80))
        r.host_port = port;
#ifdef VJO_HOST
    if (p->plain_http)
        use_tls = 0;
#endif
    if (pool_usable(p, r.host)) {
        r.keep_alive = 1;
        rc = pooled_request(a, p, port, use_tls, &r, max_body, resp, err, &ph);
    } else {
        rc = single_request(a, p, port, use_tls, &r, max_body, resp, err, &ph);
    }
    t_end = now(p);
    if (ph.kept)
        vjo_snprintf(phases, sizeof(phases), "kept connection, request %d ms", (int)((t_end - ph.start) / 1000));
    else
        vjo_snprintf(phases, sizeof(phases), "connect %d ms, tls %d ms, request %d ms",
                     (int)((ph.connected - ph.start) / 1000), (int)((ph.secured - ph.connected) / 1000),
                     (int)((t_end - ph.secured) / 1000));
    plog(p, "%s:%d %s -> rc=%d status=%d tls=%d body=%lu (%s)", req->host, port, req->path, rc, resp->status,
         err->tls_error, (unsigned long)resp->body_len, phases);
    if (rc) {
        vjo_arena_release(a, mark);
        return err->rc = rc;
    }
    /* Compact: move the body down over the request's state (dst <= body,
     * and nothing is allocated in between, so memmove is safe). */
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
