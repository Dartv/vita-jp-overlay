#include "tls.h"

#include <string.h>

#include "tls_roots.h"

static int low_read(void *ctx, unsigned char *buf, size_t len)
{
    VjoTls *t = (VjoTls *)ctx;
    int n = t->raw->recv(t->raw->ctx, buf, len);
    if (n == 0)
        t->raw_eof = 1;
    return n > 0 ? n : -1; /* BearSSL: -1 on error or EOF */
}

static int low_write(void *ctx, const unsigned char *buf, size_t len)
{
    VjoConn *raw = (VjoConn *)ctx;
    int n = raw->send(raw->ctx, buf, len);
    return n > 0 ? n : -1;
}

static int tls_send(void *ctx, const void *p, size_t n)
{
    VjoTls *t = (VjoTls *)ctx;
    if (br_sslio_write_all(&t->ioc, p, n) < 0)
        return -1;
    return (int)n;
}

static int tls_recv(void *ctx, void *p, size_t n)
{
    VjoTls *t = (VjoTls *)ctx;
    int r;
    /* Make sure the request is on the wire before waiting for a reply. */
    if (br_sslio_flush(&t->ioc) < 0)
        return -1;
    r = br_sslio_read(&t->ioc, p, n);
    if (r < 0) {
        /* close_notify is the only EOF. A TCP close without it may cut the
         * data short: fine after a Content-Length or chunked body (HTTP
         * stops reading), an error for a read-until-close body. */
        int err = br_ssl_engine_last_error(&t->cc.eng);
        if (br_ssl_engine_current_state(&t->cc.eng) != BR_SSL_CLOSED)
            return -1;
        if (err == BR_ERR_OK)
            return 0;
        return err == BR_ERR_IO && t->raw_eof ? VJO_E_HTTP : -1;
    }
    return r;
}

int vjo_tls_open(VjoTls *t, VjoConn *raw, const char *host, void *buf, size_t buf_len,
                 uint32_t days, uint32_t secs, const uint8_t *seed, size_t seed_len)
{
    memset(t, 0, sizeof(*t));
    t->raw = raw;
    br_ssl_client_init_full(&t->cc, &t->xc, vjo_tls_roots, vjo_tls_roots_num);
    br_x509_minimal_set_time(&t->xc, days, secs);
    br_ssl_engine_set_buffer(&t->cc.eng, buf, buf_len, buf_len >= BR_SSL_BUFSIZE_BIDI);
    br_ssl_engine_inject_entropy(&t->cc.eng, seed, seed_len);
    if (!br_ssl_client_reset(&t->cc, host, 0))
        return VJO_E_TLS;
    br_sslio_init(&t->ioc, &t->cc.eng, low_read, t, low_write, raw);
    /* Drive the handshake now so errors surface here. */
    if (br_sslio_flush(&t->ioc) < 0)
        return VJO_E_TLS;
    t->conn.ctx = t;
    t->conn.send = tls_send;
    t->conn.recv = tls_recv;
    return VJO_OK;
}

void vjo_tls_close(VjoTls *t)
{
    if (br_ssl_engine_current_state(&t->cc.eng) != BR_SSL_CLOSED)
        br_sslio_close(&t->ioc);
}

int vjo_tls_last_error(VjoTls *t)
{
    return br_ssl_engine_last_error(&t->cc.eng);
}

void vjo_tls_time_from_unix(uint64_t unix_secs, uint32_t *days, uint32_t *secs)
{
    /* 1970-01-01 is day 719528 counted from 0000-01-01 (proleptic Gregorian). */
    *days = (uint32_t)(unix_secs / 86400u) + 719528u;
    *secs = (uint32_t)(unix_secs % 86400u);
}
