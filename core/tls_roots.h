#ifndef VJO_TLS_ROOTS_H
#define VJO_TLS_ROOTS_H

#include <stddef.h>

#include "bearssl.h"

/* GTS Root R1-R4, GlobalSign Root CA (Google), ISRG Root X1/X2 (jpdb.io,
 * Let's Encrypt), and for audio sources (anki_audio_url): Amazon Root CA 1,
 * DigiCert Global Root G2, USERTrust RSA/ECC (Sectigo), SSL.com roots. */
extern const br_x509_trust_anchor *vjo_tls_roots;
extern const size_t vjo_tls_roots_num;

#endif
