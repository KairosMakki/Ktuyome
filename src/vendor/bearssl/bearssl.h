/*
 * bearssl.h — BearSSL TLS 1.2 client amalgam for ktuyome
 *
 * Implements a minimal TLS 1.2 client sufficient for HTTPS fetches:
 *   Cipher suites: TLS_RSA_WITH_AES_128_CBC_SHA256
 *                  TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256
 *   Certificate validation: X.509 chain (or trust-anchor bypass)
 *   Key exchange: RSA + ECDHE (P-256)
 *   Hash: SHA-1, SHA-256
 *   Symmetric: AES-128-CBC, pure-C constant-time (no SSE/AES-NI)
 *   PRF: TLS 1.2 SHA-256-based PRF
 *   MAC: HMAC-SHA-256, HMAC-SHA-1
 *
 * All crypto is pure portable C — no SSE, no hardware acceleration.
 * Designed to compile under: -m32 -ffreestanding -fno-builtin -mno-sse -O2
 *
 * Usage:
 *   1. Allocate a br_ssl_client_context on the heap or as a static global.
 *   2. Call br_ssl_client_init_full(&cc, &xc, trust_anchors, ta_count).
 *      Pass trust_anchors=NULL, ta_count=0 to skip cert validation
 *      (acceptable for a bare-metal hobby OS browser).
 *   3. Set up I/O callbacks via br_sslio_context.
 *   4. Drive the engine from your polling loop.
 */
#pragma once
#include "../ktuyome_compat.h"

/* ------------------------------------------------------------------ */
/*  Forward declarations & opaque types                                */
/* ------------------------------------------------------------------ */
typedef struct br_ssl_engine_context   br_ssl_engine_context;
typedef struct br_ssl_client_context   br_ssl_client_context;
typedef struct br_ssl_server_context   br_ssl_server_context;
typedef struct br_x509_minimal_context br_x509_minimal_context;
typedef struct br_x509_knownkey_context br_x509_knownkey_context;
typedef struct br_sslio_context        br_sslio_context;
typedef struct br_hash_class           br_hash_class;
typedef struct br_sha256_context       br_sha256_context;
typedef struct br_sha1_context         br_sha1_context;
typedef struct br_hmac_key_context     br_hmac_key_context;
typedef struct br_hmac_context         br_hmac_context;
typedef struct br_aes_ct_cbcenc_keys   br_aes_ct_cbcenc_keys;
typedef struct br_aes_ct_cbcdec_keys   br_aes_ct_cbcdec_keys;
typedef struct br_ec_public_key        br_ec_public_key;
typedef struct br_rsa_public_key       br_rsa_public_key;
typedef struct br_x509_trust_anchor    br_x509_trust_anchor;

/* ------------------------------------------------------------------ */
/*  Engine state flags (returned by br_ssl_engine_current_state)       */
/* ------------------------------------------------------------------ */
#define BR_SSL_CLOSED    0x0001  /* engine is closed (done or error) */
#define BR_SSL_SENDREC   0x0002  /* engine has data to send to network */
#define BR_SSL_RECVREC   0x0004  /* engine wants data from network */
#define BR_SSL_SENDAPP   0x0008  /* application can write plaintext */
#define BR_SSL_RECVAPP   0x0010  /* application plaintext is ready */

/* ------------------------------------------------------------------ */
/*  Error codes                                                        */
/* ------------------------------------------------------------------ */
#define BR_ERR_OK                    0
#define BR_ERR_BAD_PARAM            -1
#define BR_ERR_BAD_STATE            -2
#define BR_ERR_UNSUPPORTED_VERSION  -3
#define BR_ERR_BAD_VERSION          -4
#define BR_ERR_BAD_LENGTH           -5
#define BR_ERR_TOO_LARGE            -6
#define BR_ERR_BAD_MAC              -7
#define BR_ERR_NO_RANDOM            -8
#define BR_ERR_UNKNOWN_TYPE         -9
#define BR_ERR_UNEXPECTED           -10
#define BR_ERR_BAD_CCS              -12
#define BR_ERR_BAD_ALERT            -13
#define BR_ERR_BAD_HANDSHAKE        -14
#define BR_ERR_OVERSIZED_ID         -15
#define BR_ERR_BAD_CIPHER_SUITE     -16
#define BR_ERR_BAD_COMPRESSION      -17
#define BR_ERR_BAD_FRAGLEN          -18
#define BR_ERR_BAD_SECRENEG         -19
#define BR_ERR_EXTRA_EXTENSION      -20
#define BR_ERR_BAD_SNI              -21
#define BR_ERR_BAD_HELLO_DONE      -22
#define BR_ERR_LIMIT_EXCEEDED       -23
#define BR_ERR_BAD_FINISHED         -24
#define BR_ERR_RESUME_MISMATCH      -25
#define BR_ERR_INVALID_ALGORITHM    -26
#define BR_ERR_BAD_SIGNATURE        -27
#define BR_ERR_WRONG_KEY_USAGE      -28
#define BR_ERR_NO_CLIENT_AUTH       -29
#define BR_ERR_IO                   -30
#define BR_ERR_X509_OK              32
#define BR_ERR_X509_INVALID_VALUE   (BR_ERR_X509_OK+ 1)
#define BR_ERR_X509_EXPIRED         (BR_ERR_X509_OK+16)

/* ------------------------------------------------------------------ */
/*  SHA-256                                                            */
/* ------------------------------------------------------------------ */
#define BR_SHA256_SIZE 32
struct br_sha256_context {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buf[64];
    int      buflen;
};
void br_sha256_init(br_sha256_context *ctx);
void br_sha256_update(br_sha256_context *ctx, const void *data, size_t len);
void br_sha256_out(const br_sha256_context *ctx, void *out);   /* non-destructive */

#define BR_SHA1_SIZE 20
struct br_sha1_context {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buf[64];
    int      buflen;
};
void br_sha1_init(br_sha1_context *ctx);
void br_sha1_update(br_sha1_context *ctx, const void *data, size_t len);
void br_sha1_out(const br_sha1_context *ctx, void *out);

/* ------------------------------------------------------------------ */
/*  HMAC                                                               */
/* ------------------------------------------------------------------ */
#define BR_HMAC_MAX_OUT 32
struct br_hmac_key_context {
    uint8_t kpad[64];
    int     out_len;
    int     is_sha256;
};
struct br_hmac_context {
    br_hmac_key_context key;
    union { br_sha256_context sha256; br_sha1_context sha1; } h;
};
void br_hmac_key_init_sha256(br_hmac_key_context *kc, const void *key, size_t klen);
void br_hmac_key_init_sha1(br_hmac_key_context *kc, const void *key, size_t klen);
void br_hmac_init(br_hmac_context *ctx, const br_hmac_key_context *kc, size_t out_len);
void br_hmac_update(br_hmac_context *ctx, const void *data, size_t len);
size_t br_hmac_out(br_hmac_context *ctx, void *out);

/* ------------------------------------------------------------------ */
/*  AES-128 CBC (constant-time, pure C)                                */
/* ------------------------------------------------------------------ */
struct br_aes_ct_cbcenc_keys { uint32_t skey[60]; int num_rounds; };
struct br_aes_ct_cbcdec_keys { uint32_t skey[60]; int num_rounds; };
void br_aes_ct_cbcenc_init(br_aes_ct_cbcenc_keys *k, const void *key, size_t klen);
void br_aes_ct_cbcdec_init(br_aes_ct_cbcdec_keys *k, const void *key, size_t klen);
void br_aes_ct_cbcenc_run(const br_aes_ct_cbcenc_keys *k, void *iv, void *data, size_t len);
void br_aes_ct_cbcdec_run(const br_aes_ct_cbcdec_keys *k, void *iv, void *data, size_t len);

/* ------------------------------------------------------------------ */
/*  RSA (for certificate parsing — verify only, 2048-bit max)         */
/* ------------------------------------------------------------------ */
struct br_rsa_public_key {
    uint8_t *n;  size_t nlen;
    uint8_t *e;  size_t elen;
};
int br_rsa_pkcs1_vrfy(const uint8_t *x, size_t xlen,
                      const uint8_t *hash_oid, size_t hash_len,
                      const br_rsa_public_key *pk,
                      uint8_t *hash_out);

/* ------------------------------------------------------------------ */
/*  EC P-256 (for ECDHE key exchange)                                  */
/* ------------------------------------------------------------------ */
#define BR_EC_P256  23
struct br_ec_public_key { int curve; uint8_t *q; size_t qlen; };

/* ------------------------------------------------------------------ */
/*  X.509 trust anchor (simplified)                                    */
/* ------------------------------------------------------------------ */
struct br_x509_trust_anchor {
    struct { uint8_t *data; size_t datalen; } dn;
    unsigned flags;
    br_rsa_public_key pkey;
};
#define BR_X509_TA_CA  0x0001

/* ------------------------------------------------------------------ */
/*  I/O context — wraps ktuyome TCP send/recv                          */
/* ------------------------------------------------------------------ */
typedef int (*br_ssl_recv_fn)(void *ctx, uint8_t *buf, size_t len);
typedef int (*br_ssl_send_fn)(void *ctx, const uint8_t *buf, size_t len);

/* ------------------------------------------------------------------ */
/*  TLS record-layer buffers                                           */
/* ------------------------------------------------------------------ */
#define BR_SSL_BUFSIZE_BIDI  (16384 + 325 + 85)  /* one record each way */
#define BR_SSL_BUFSIZE_MONO  (16384 + 325)

/* ------------------------------------------------------------------ */
/*  SSL engine (internal state — treat as opaque from outside)        */
/* ------------------------------------------------------------------ */
typedef enum {
    TLS_HS_NONE = 0,
    TLS_HS_CLIENT_HELLO,
    TLS_HS_SERVER_HELLO,
    TLS_HS_CERTIFICATE,
    TLS_HS_SERVER_KEY_EXCHANGE,
    TLS_HS_SERVER_HELLO_DONE,
    TLS_HS_CLIENT_KEY_EXCHANGE,
    TLS_HS_CHANGE_CIPHER_SPEC_OUT,
    TLS_HS_FINISHED_OUT,
    TLS_HS_CHANGE_CIPHER_SPEC_IN,
    TLS_HS_FINISHED_IN,
    TLS_HS_DONE,
    TLS_HS_ERROR
} tls_hs_state_t;

/* Maximum sizes */
#define TLS_MAX_RECORD   (16384 + 2048)
#define TLS_CERT_BUF     4096
#define TLS_SKEY_BUF     512
#define TLS_PRE_MASTER   48
#define TLS_MASTER       48
#define TLS_RANDOM_SIZE  32
#define TLS_MAX_SNI      128
#define TLS_KEY_BLOCK    (2*16 + 2*16 + 2*20)   /* AES-128 + HMAC-SHA-1 */
#define TLS_KB2          (2*16 + 2*32 + 2*16)   /* AES-128-CBC-SHA256 variant */

struct br_ssl_engine_context {
    /* handshake state */
    tls_hs_state_t   hs;
    int              err;
    int              flags;   /* BR_SSL_* */

    /* SNI */
    char  sni[TLS_MAX_SNI];

    /* random bytes */
    uint8_t  client_random[TLS_RANDOM_SIZE];
    uint8_t  server_random[TLS_RANDOM_SIZE];

    /* pre-master / master secret */
    uint8_t  pre_master[TLS_PRE_MASTER];
    uint8_t  master[TLS_MASTER];

    /* negotiated cipher */
    uint16_t cipher_suite;
    uint8_t  server_version[2];

    /* key material */
    uint8_t  key_block[TLS_KB2];
    br_aes_ct_cbcenc_keys enc_key;
    br_aes_ct_cbcdec_keys dec_key;
    uint8_t  client_write_mac[32];
    uint8_t  server_write_mac[32];
    uint8_t  client_write_iv[16];
    uint8_t  server_write_iv[16];

    /* GCM: implicit 4-byte IVs + GHASH subkeys (computed from AES key) */
    uint8_t  gcm_h_enc[16];   /* H for client→server encryption */
    uint8_t  gcm_h_dec[16];   /* H for server→client decryption */
    uint8_t  gcm_iv_enc[4];   /* fixed IV for writing */
    uint8_t  gcm_iv_dec[4];   /* fixed IV for reading */

    /* server public key (from certificate) */
    uint8_t  server_n[256];  size_t   server_nlen;
    uint8_t  server_e[4];    size_t   server_elen;

    /* ECDHE ephemeral server public key (P-256, uncompressed) */
    uint8_t  ecdhe_server_pub[65];
    uint8_t  ecdhe_client_priv[32];
    uint8_t  ecdhe_client_pub[65];
    uint8_t  ecdhe_shared[32];

    /* handshake transcript for Finished */
    br_sha256_context hs_sha256;
    br_sha1_context   hs_sha1;

    /* certificate buffer (we only buffer one leaf cert) */
    uint8_t  cert_buf[TLS_CERT_BUF];
    size_t   cert_len;

    /* receive record reassembly */
    uint8_t  rec_hdr[5];
    int      rec_hdr_got;
    uint8_t *rec_buf;      /* points into net_buf */
    size_t   rec_buf_cap;
    size_t   rec_buf_len;
    size_t   rec_buf_pos;

    /* network I/O buffers */
    uint8_t  net_send_buf[TLS_MAX_RECORD + 128];
    size_t   net_send_len;
    size_t   net_send_pos;

    uint8_t  net_recv_buf[TLS_MAX_RECORD + 128];
    size_t   net_recv_cap;
    size_t   net_recv_len;
    size_t   net_recv_pos;

    /* application plaintext output (decrypted records) */
    uint8_t  app_buf[TLS_MAX_RECORD];
    size_t   app_len;
    size_t   app_pos;

    /* sequence numbers */
    uint64_t seq_write;
    uint64_t seq_read;

    /* trust anchors (for optional cert validation) */
    const br_x509_trust_anchor *ta;
    size_t   ta_count;
    int      skip_verify;   /* 1 = accept any cert */

    /* I/O callbacks */
    br_ssl_recv_fn  recv_fn;
    br_ssl_send_fn  send_fn;
    void           *io_ctx;
};

struct br_ssl_client_context {
    br_ssl_engine_context eng;
};

struct br_sslio_context {
    br_ssl_engine_context *eng;
    br_ssl_recv_fn  low_read;
    br_ssl_send_fn  low_write;
    void           *read_ctx;
    void           *write_ctx;
};

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Initialise a TLS 1.2 client context.
 * Pass ta=NULL / ta_count=0 to skip certificate verification. */
void br_ssl_client_init_full(br_ssl_client_context *cc,
                             const br_x509_trust_anchor *ta,
                             size_t ta_count);

/* Reset for a new connection to the given hostname (SNI). */
int  br_ssl_client_reset(br_ssl_client_context *cc,
                         const char *server_name,
                         int resume_session);

/* Attach low-level I/O callbacks */
void br_ssl_engine_set_io(br_ssl_engine_context *eng,
                          br_ssl_recv_fn recv_fn,
                          br_ssl_send_fn send_fn,
                          void *io_ctx);

/* Engine state machine — call every polling iteration.
 * Returns the new state flags (combination of BR_SSL_*). */
int  br_ssl_engine_current_state(const br_ssl_engine_context *eng);

/* Drive the engine one step (send/recv records, advance handshake).
 * Must be called repeatedly until state stabilises. */
void br_ssl_engine_run(br_ssl_engine_context *eng);

/* Application data: send plaintext */
int  br_ssl_engine_sendapp_buf(br_ssl_engine_context *eng,
                               const uint8_t *data, size_t len);
/* Flush pending plaintext as a TLS record */
void br_ssl_engine_flush(br_ssl_engine_context *eng, int close);

/* Application data: receive plaintext */
int  br_ssl_engine_recvapp_buf(br_ssl_engine_context *eng,
                               uint8_t *data, size_t len);

/* Close the TLS session (send close_notify) */
void br_ssl_engine_close(br_ssl_engine_context *eng);

/* Error code (0 = ok) */
int  br_ssl_engine_last_error(const br_ssl_engine_context *eng);

/* Convenience: high-level I/O (blocks implicitly via caller's polling) */
void br_sslio_init(br_sslio_context *ctx,
                   br_ssl_engine_context *eng,
                   br_ssl_recv_fn low_read,  void *read_ctx,
                   br_ssl_send_fn low_write, void *write_ctx);
int  br_sslio_read(br_sslio_context *ctx, void *dst, size_t len);
int  br_sslio_write(br_sslio_context *ctx, const void *src, size_t len);
int  br_sslio_flush(br_sslio_context *ctx);
int  br_sslio_close(br_sslio_context *ctx);

/* ------------------------------------------------------------------ */
/*  Pseudo-random: seeded from rdtsc + ticks                           */
/* ------------------------------------------------------------------ */
void br_prng_init(void);
void br_prng_bytes(uint8_t *out, size_t len);
