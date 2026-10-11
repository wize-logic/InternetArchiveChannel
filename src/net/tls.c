#include "net/tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/ssl.h>
#include <mbedtls/threading.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include "core/log.h"
#include "core/platform.h"
#include "net/entropy.h"
#include "net/sock.h"

#define SESSION_CACHE_SIZE 8

struct tls_conn {
	int fd;
	int timeout_ms;
	char host[96];
	mbedtls_ssl_context ssl;
	struct tls_stats stats;
};

struct cached_session {
	char host[96];
	mbedtls_ssl_session session;
	int valid;
};

extern const unsigned char iac_ca_bundle_pem[];
extern const size_t iac_ca_bundle_pem_size;

static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_x509_crt g_ca;
static mbedtls_ssl_config g_conf;
static int64_t g_clock_offset;
static struct cached_session g_sessions[SESSION_CACHE_SIZE];
static int g_session_next;
static iac_mutex g_session_lock;
/* The DRBG is shared by every connection; mbedTLS doesn't lock it for us. */
static iac_mutex g_rng_lock;

static int g_suites[16];

static const int k_suites_chacha_first[] = {
	MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
	MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
	MBEDTLS_TLS1_3_AES_256_GCM_SHA384,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
	MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
	MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
	MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
	0,
};

static const int k_suites_aes_first[] = {
	MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
	MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
	MBEDTLS_TLS1_3_AES_256_GCM_SHA384,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
	MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
	MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
	MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
	0,
};

static const uint16_t k_groups[] = {
	MBEDTLS_SSL_IANA_TLS_GROUP_X25519,
	MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1,
	MBEDTLS_SSL_IANA_TLS_GROUP_NONE,
};

mbedtls_ms_time_t mbedtls_ms_time(void)
{
	return (mbedtls_ms_time_t)plat_ms();
}

static mbedtls_time_t corrected_time(mbedtls_time_t *t)
{
	mbedtls_time_t now = (mbedtls_time_t)(time(NULL) + g_clock_offset);
	if (t)
		*t = now;
	return now;
}

static int locked_rng(void *ctx, unsigned char *out, size_t len)
{
	plat_mutex_lock(&g_rng_lock);
	int rc = mbedtls_ctr_drbg_random(ctx, out, len);
	plat_mutex_unlock(&g_rng_lock);
	return rc;
}

static int load_ca(const char *override_path)
{
	if (override_path) {
		FILE *f = fopen(override_path, "rb");
		if (f) {
			fclose(f);
			int rc = mbedtls_x509_crt_parse_file(&g_ca, override_path);
			if (rc >= 0) {
				log_printf("tls: CA bundle from %s (%d unparsed)\n", override_path, rc);
				return 0;
			}
			log_printf("tls: bad CA override %s (-0x%04x), using built-in\n",
				override_path, (unsigned)-rc);
			mbedtls_x509_crt_free(&g_ca);
			mbedtls_x509_crt_init(&g_ca);
		}
	}
	int rc = mbedtls_x509_crt_parse(&g_ca, iac_ca_bundle_pem, iac_ca_bundle_pem_size);
	return rc < 0 ? rc : 0;
}

void tls_set_flags(unsigned flags)
{
	const int *src = (flags & TLS_PREFER_CHACHA) ? k_suites_chacha_first : k_suites_aes_first;
	size_t n = 0;
	for (; *src; src++) {
		if ((flags & TLS_NO_AES256) && (*src == MBEDTLS_TLS1_3_AES_256_GCM_SHA384 ||
			*src == MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384 ||
			*src == MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384))
			continue;
		g_suites[n++] = *src;
	}
	g_suites[n] = 0;
	mbedtls_ssl_conf_ciphersuites(&g_conf, g_suites);
}

static void mt_init(mbedtls_threading_mutex_t *m)
{
	plat_mutex_init(&m->mutex);
	m->valid = 1;
}

static void mt_free(mbedtls_threading_mutex_t *m)
{
	if (m->valid)
		plat_mutex_destroy(&m->mutex);
	m->valid = 0;
}

static int mt_lock(mbedtls_threading_mutex_t *m)
{
	if (!m->valid)
		return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
	plat_mutex_lock(&m->mutex);
	return 0;
}

static int mt_unlock(mbedtls_threading_mutex_t *m)
{
	if (!m->valid)
		return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
	plat_mutex_unlock(&m->mutex);
	return 0;
}

int tls_global_init(const char *ca_override_path, const char *seed_path, unsigned flags)
{
	static const char pers[] = "IAChannel TLS";
	int rc;

	/* Must come before any other mbedTLS call. */
	mbedtls_threading_set_alt(mt_init, mt_free, mt_lock, mt_unlock);
	plat_mutex_init(&g_session_lock);
	plat_mutex_init(&g_rng_lock);
	mbedtls_platform_set_time(corrected_time);

	if (seed_path)
		entropy_load_seed(seed_path);

	mbedtls_entropy_init(&g_entropy);
	mbedtls_ctr_drbg_init(&g_drbg);
	mbedtls_x509_crt_init(&g_ca);
	mbedtls_ssl_config_init(&g_conf);

	rc = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
		(const unsigned char *)pers, sizeof pers - 1);
	if (rc != 0)
		return rc;

	if (seed_path) {
		unsigned char next_seed[64];
		if (mbedtls_ctr_drbg_random(&g_drbg, next_seed, sizeof next_seed) == 0)
			entropy_save_seed(seed_path, next_seed, sizeof next_seed);
		memset(next_seed, 0, sizeof next_seed);
	}

	if (psa_crypto_init() != PSA_SUCCESS)
		return MBEDTLS_ERR_SSL_HW_ACCEL_FAILED;

	rc = load_ca(ca_override_path);
	if (rc < 0)
		return rc;

	rc = mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_CLIENT,
		MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
	if (rc != 0)
		return rc;
	mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&g_conf, &g_ca, NULL);
	mbedtls_ssl_conf_rng(&g_conf, locked_rng, &g_drbg);
	mbedtls_ssl_conf_min_tls_version(&g_conf, MBEDTLS_SSL_VERSION_TLS1_2);
	tls_set_flags(flags);
	mbedtls_ssl_conf_groups(&g_conf, k_groups);
	mbedtls_ssl_conf_session_tickets(&g_conf, MBEDTLS_SSL_SESSION_TICKETS_ENABLED);
	return 0;
}

void tls_global_shutdown(void)
{
	for (int i = 0; i < SESSION_CACHE_SIZE; i++) {
		if (g_sessions[i].valid)
			mbedtls_ssl_session_free(&g_sessions[i].session);
		g_sessions[i].valid = 0;
	}
	mbedtls_ssl_config_free(&g_conf);
	mbedtls_x509_crt_free(&g_ca);
	mbedtls_ctr_drbg_free(&g_drbg);
	mbedtls_entropy_free(&g_entropy);
	mbedtls_psa_crypto_free();
	mbedtls_threading_free_alt();
}

/* Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm);
 * newlib has no timegm(). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int64_t)doe - 719468;
}

int tls_parse_http_date(const char *s, time_t *out)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	char mon[4] = {0};
	int day, year, hh, mm, ss;
	const char *comma = strchr(s, ',');
	if (!comma)
		return -1;
	if (sscanf(comma + 1, " %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6)
		return -1;
	const char *mp = strstr(months, mon);
	if (!mp || strlen(mon) != 3 || (mp - months) % 3 != 0)
		return -1;
	unsigned month = (unsigned)((mp - months) / 3) + 1;
	if (day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60 || year < 1970)
		return -1;
	*out = (time_t)(days_from_civil(year, month, (unsigned)day) * 86400 + hh * 3600 + mm * 60 + ss);
	return 0;
}

int tls_note_server_date(const char *http_date)
{
	time_t server;
	if (tls_parse_http_date(http_date, &server) != 0)
		return -1;
	int64_t offset = (int64_t)server - (int64_t)time(NULL);
	if (offset > 60 || offset < -60) {
		/* Only log real changes, not the few seconds of jitter between responses. */
		int64_t delta = offset - g_clock_offset;
		if (delta > 60 || delta < -60)
			log_printf("tls: console clock off by %lld s, correcting\n", (long long)offset);
		g_clock_offset = offset;
	} else {
		g_clock_offset = 0;
	}
	return 0;
}

int64_t tls_clock_offset(void)
{
	return g_clock_offset;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	tls_conn *c = ctx;
	uint64_t t = plat_us();
	int rc = sock_send_all(c->fd, buf, len, c->timeout_ms);
	c->stats.io_wait_us += plat_us() - t;
	if (rc == SOCK_ERR_TIMEOUT)
		return MBEDTLS_ERR_SSL_TIMEOUT;
	return rc < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : rc;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	tls_conn *c = ctx;
	uint64_t t = plat_us();
	int rc = sock_recv(c->fd, buf, len, c->timeout_ms);
	c->stats.io_wait_us += plat_us() - t;
	if (rc == SOCK_ERR_TIMEOUT)
		return MBEDTLS_ERR_SSL_TIMEOUT;
	return rc < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : rc;
}

static void session_restore(tls_conn *c)
{
	plat_mutex_lock(&g_session_lock);
	for (int i = 0; i < SESSION_CACHE_SIZE; i++) {
		if (g_sessions[i].valid && strcmp(g_sessions[i].host, c->host) == 0) {
			c->stats.offered_session = mbedtls_ssl_set_session(&c->ssl, &g_sessions[i].session) == 0;
			break;
		}
	}
	plat_mutex_unlock(&g_session_lock);
}

static void session_save_ssl(mbedtls_ssl_context *ssl, const char *host)
{
	mbedtls_ssl_session s;
	mbedtls_ssl_session_init(&s);
	if (mbedtls_ssl_get_session(ssl, &s) != 0) {
		mbedtls_ssl_session_free(&s);
		return;
	}
	plat_mutex_lock(&g_session_lock);
	struct cached_session *slot = NULL;
	for (int i = 0; i < SESSION_CACHE_SIZE && !slot; i++)
		if (g_sessions[i].valid && strcmp(g_sessions[i].host, host) == 0)
			slot = &g_sessions[i];
	if (!slot) {
		slot = &g_sessions[g_session_next];
		g_session_next = (g_session_next + 1) % SESSION_CACHE_SIZE;
	}
	if (slot->valid)
		mbedtls_ssl_session_free(&slot->session);
	snprintf(slot->host, sizeof slot->host, "%s", host);
	slot->session = s; /* ownership moves into the cache */
	slot->valid = 1;
	plat_mutex_unlock(&g_session_lock);
}

static void session_save(tls_conn *c)
{
	session_save_ssl(&c->ssl, c->host);
}

const struct mbedtls_ssl_config *tls_shared_config(void)
{
	return &g_conf;
}

void tls_session_offer(struct mbedtls_ssl_context *ssl, const char *host)
{
	plat_mutex_lock(&g_session_lock);
	for (int i = 0; i < SESSION_CACHE_SIZE; i++) {
		if (g_sessions[i].valid && strcmp(g_sessions[i].host, host) == 0) {
			mbedtls_ssl_set_session(ssl, &g_sessions[i].session);
			break;
		}
	}
	plat_mutex_unlock(&g_session_lock);
}

void tls_session_store(struct mbedtls_ssl_context *ssl, const char *host)
{
	session_save_ssl(ssl, host);
}

tls_conn *tls_connect(const char *host, uint16_t port, int timeout_ms, int *err)
{
	tls_conn *c = calloc(1, sizeof *c);
	int rc;
	if (!c) {
		*err = MBEDTLS_ERR_SSL_ALLOC_FAILED;
		return NULL;
	}
	strncpy(c->host, host, sizeof c->host - 1);
	c->timeout_ms = timeout_ms;
	mbedtls_ssl_init(&c->ssl);

	uint64_t t0 = plat_ms();
	c->fd = sock_connect(host, port, timeout_ms);
	c->stats.connect_ms = (uint32_t)(plat_ms() - t0);
	if (c->fd < 0) {
		rc = c->fd;
		goto fail;
	}
	if ((rc = mbedtls_ssl_setup(&c->ssl, &g_conf)) != 0)
		goto fail;
	if ((rc = mbedtls_ssl_set_hostname(&c->ssl, host)) != 0)
		goto fail;
	mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
	session_restore(c);

	t0 = plat_ms();
	while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
		if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
			break;
	}
	if (rc != 0) {
		uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
		if (flags != 0 && flags != (uint32_t)-1) {
			char info[256];
			mbedtls_x509_crt_verify_info(info, sizeof info, "  ", flags);
			log_printf("tls: %s certificate rejected:\n%s", host, info);
		}
		goto fail;
	}
	c->stats.handshake_ms = (uint32_t)(plat_ms() - t0);
	return c;

fail:
	*err = rc;
	if (c->fd >= 0)
		sock_close(c->fd);
	mbedtls_ssl_free(&c->ssl);
	free(c);
	return NULL;
}

int tls_write_all(tls_conn *c, const void *buf, size_t len)
{
	const unsigned char *p = buf;
	size_t left = len;
	while (left > 0) {
		int rc = mbedtls_ssl_write(&c->ssl, p, left);
		if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (rc < 0)
			return rc;
		p += rc;
		left -= (size_t)rc;
	}
	return (int)len;
}

int tls_read(tls_conn *c, void *buf, size_t len)
{
	for (;;) {
		int rc = mbedtls_ssl_read(&c->ssl, buf, len);
		if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
		if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
			continue;
#endif
		if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_SSL_CONN_EOF)
			return 0;
		return rc;
	}
}

void tls_close(tls_conn *c)
{
	if (!c)
		return;
	session_save(c);
	mbedtls_ssl_close_notify(&c->ssl);
	sock_close(c->fd);
	mbedtls_ssl_free(&c->ssl);
	free(c);
}

void tls_get_stats(tls_conn *c, struct tls_stats *out)
{
	*out = c->stats;
}

void tls_describe(tls_conn *c, char *out, size_t n)
{
	snprintf(out, n, "%s %s", mbedtls_ssl_get_version(&c->ssl),
		mbedtls_ssl_get_ciphersuite(&c->ssl));
}

void tls_strerror(int err, char *out, size_t n)
{
	if (err <= SOCK_ERR_IO && err >= SOCK_ERR_INIT) {
		snprintf(out, n, "%s", sock_strerror(err));
		return;
	}
	char tmp[128];
	mbedtls_strerror(err, tmp, sizeof tmp);
	snprintf(out, n, "-0x%04x %s", (unsigned)-err, tmp);
}
