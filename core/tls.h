/* BearSSL client over a raw VjoConn, exposed as another VjoConn. */
#ifndef VJO_TLS_H
#define VJO_TLS_H

#include <stddef.h>
#include <stdint.h>

#include "bearssl.h"
#include "conn.h"

typedef struct {
    br_ssl_client_context cc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    VjoConn *raw;
    int raw_eof;  /* the peer closed TCP */
    VjoConn conn; /* the TLS stream */
} VjoTls;

/* Buffer for vjo_tls_open: BR_SSL_BUFSIZE_MONO (~16.7 KB, half duplex) is
 * enough for one HTTP request/response; BR_SSL_BUFSIZE_BIDI is faster. */
#define VJO_TLS_MIN_BUF BR_SSL_BUFSIZE_MONO

/* `days`/`secs`: current UTC time in BearSSL terms (days since 0000-01-01,
 * seconds in day); see vjo_tls_time_from_unix. `seed` >= 32 bytes entropy. */
int vjo_tls_open(VjoTls *t, VjoConn *raw, const char *host, void *buf, size_t buf_len,
                 uint32_t days, uint32_t secs, const uint8_t *seed, size_t seed_len);
/* Flushes and sends close_notify. */
void vjo_tls_close(VjoTls *t);
/* BearSSL error code of the engine (0 = none). */
int vjo_tls_last_error(VjoTls *t);

void vjo_tls_time_from_unix(uint64_t unix_secs, uint32_t *days, uint32_t *secs);

#endif
