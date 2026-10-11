/* TLS client on top of net/sock, using mbedTLS 3.6 (TLS 1.2 and 1.3, verified chains). */
#ifndef IAC_TLS_H
#define IAC_TLS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct tls_conn tls_conn;

enum {
	TLS_PREFER_CHACHA = 1 << 0, /* put ChaCha20-Poly1305 ahead of AES-GCM */
	/* Don't offer AES-256 suites. archive.org picks AES-256-GCM by server preference,
	 * which is the slowest option in software on Broadway. */
	TLS_NO_AES256 = 1 << 1,
	/* Default policy: in Dolphin, software AES-GCM ran at ~2.2 MB/s and ChaCha20-Poly1305
	 * at ~15.9 MB/s. */
	TLS_FLAGS_DEFAULT = TLS_PREFER_CHACHA | TLS_NO_AES256,
};

struct tls_stats {
	uint32_t connect_ms;   /* DNS + TCP connect */
	uint32_t handshake_ms; /* TLS handshake only */
	uint64_t io_wait_us;   /* time blocked in socket send/recv since connect */
	int offered_session;   /* a cached session was offered for resumption */
};

/* Loads the CA bundle (override file if present, else the embedded one), seeds the DRBG.
 * Both paths may be NULL. Returns 0 or a negative mbedTLS error. */
int tls_global_init(const char *ca_override_path, const char *seed_path, unsigned flags);
void tls_global_shutdown(void);
/* Changes cipher preferences (TLS_* flags) for connections opened afterwards. */
void tls_set_flags(unsigned flags);

/* Feed an HTTP Date: header value; corrects the clock used for certificate validity
 * when the console clock is off by more than a minute. Returns 0 if parsed. */
int tls_note_server_date(const char *http_date);
/* Seconds added to time() to get UTC (0 until a server date has been seen). */
int64_t tls_clock_offset(void);
/* Parses an RFC 7231 IMF-fixdate ("Sun, 06 Nov 1994 08:49:37 GMT"). */
int tls_parse_http_date(const char *s, time_t *out);

/* TCP connect + handshake. Reuses a cached session for host when one exists.
 * Returns NULL on failure and stores a negative error code in *err. */
tls_conn *tls_connect(const char *host, uint16_t port, int timeout_ms, int *err);
/* Writes everything. Returns len or a negative error. */
int tls_write_all(tls_conn *c, const void *buf, size_t len);
/* Returns bytes read (>0), 0 on clean close, or a negative error. */
int tls_read(tls_conn *c, void *buf, size_t len);
/* Caches the session for resumption, then closes the socket and frees c. */
void tls_close(tls_conn *c);

void tls_get_stats(tls_conn *c, struct tls_stats *out);
/* For non-blocking users (net/http.c) that run their own mbedtls_ssl_context. */
struct mbedtls_ssl_config;
struct mbedtls_ssl_context;
const struct mbedtls_ssl_config *tls_shared_config(void);
/* Offers a cached session for `host` (resumption); stores one after a completed exchange. */
void tls_session_offer(struct mbedtls_ssl_context *ssl, const char *host);
void tls_session_store(struct mbedtls_ssl_context *ssl, const char *host);

/* "TLSv1.3 TLS1-3-AES-128-GCM-SHA256" style description of the negotiated parameters. */
void tls_describe(tls_conn *c, char *out, size_t n);
/* Human-readable text for a negative error returned by this module. */
void tls_strerror(int err, char *out, size_t n);

#endif
