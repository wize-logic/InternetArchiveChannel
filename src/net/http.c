#include "net/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <mbedtls/ssl.h>

#include "core/log.h"
#include "core/platform.h"
#include "net/http_parse.h"
#include "net/sock.h"
#include "net/tls.h"
#include "net/url.h"

#define MAX_CONNS 6
#define MAX_PER_HOST 4
#define IDLE_KEEP_MS 15000
#define MAX_REDIRECTS 8
#define MAX_ATTEMPTS 3 /* network errors, 429/503, and 500/502/504 */
#define HEAD_MAX 16384
#define USER_AGENT "IAChannel/0.2 (Nintendo Wii; +https://github.com/wize-logic/InternetArchiveChannel)"

enum conn_state { C_FREE = 0, C_CONNECTING, C_HANDSHAKE, C_READY };
enum req_phase { R_QUEUED = 0, R_SEND, R_HEAD, R_BODY };

struct conn {
	enum conn_state state;
	int fd;
	int https;
	char host[URL_HOST_MAX];
	uint16_t port;
	uint32_t addr;
	mbedtls_ssl_context ssl;
	int ssl_ready;
	int responses; /* completed on this connection */
	uint64_t idle_since;
	struct http_req *req;
};

struct http_req {
	struct http_result res;
	volatile int state; /* enum http_state; written under g.lock */
	int released;       /* app let go while pending; the net thread frees it */

	struct http_opts opts;
	struct url url;
	struct url first; /* as asked for, before redirects: a server-error retry starts over here */
	int redirects, attempts;
	uint64_t start_ms, deadline_ms, not_before_ms;
	enum req_phase phase;
	struct conn *conn;
	char reqbuf[2048];
	size_t req_len, req_sent;
	char head[HEAD_MAX];
	size_t head_len;
	struct http_head hh;
	struct chunked ch;
	int64_t body_remaining; /* -1: until the connection closes */
	int discard;            /* redirect/retry: body ignored */
	int throttle_wait;      /* waiting out a 429/503 before the retry */
	size_t cap;
	struct http_req *next;
};

static struct {
	iac_mutex lock;
	iac_thread thread;
	volatile int running;
	struct http_req *incoming; /* app -> net thread, under lock */
	struct http_req *active;   /* net thread only */
	struct conn conns[MAX_CONNS];
	struct http_stats stats;
	uint8_t rx[16384];
} g;

void http_opts_default(struct http_opts *o)
{
	o->range_start = -1;
	o->range_end = -1;
	o->max_body = 4u << 20;
	o->timeout_ms = 60000;
	o->priority = 0;
}

const char *http_strerror(int err)
{
	static const char *const names[] = {"ok", "bad URL", "DNS lookup failed", "could not connect",
		"TLS error", "network error", "bad response", "response too large", "timed out",
		"too many redirects", "out of memory", "shutting down"};
	return err >= 0 && err < (int)(sizeof names / sizeof names[0]) ? names[err] : "unknown error";
}

/* ---- connection I/O -------------------------------------------------------------------- */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	struct conn *c = ctx;
	int n = sock_nb_send(c->fd, buf, len);
	if (n == SOCK_WOULD_BLOCK)
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	return n < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	struct conn *c = ctx;
	int n = sock_nb_recv(c->fd, buf, len);
	if (n == SOCK_WOULD_BLOCK)
		return MBEDTLS_ERR_SSL_WANT_READ;
	return n < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : n;
}

/* Returns bytes (>0), 0 on close, SOCK_WOULD_BLOCK, or <0 on error. */
static int conn_write(struct conn *c, const void *buf, size_t len)
{
	if (!c->https)
		return sock_nb_send(c->fd, buf, len);
	int rc = mbedtls_ssl_write(&c->ssl, buf, len);
	if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
		return SOCK_WOULD_BLOCK;
	return rc < 0 ? SOCK_ERR_IO : rc;
}

static int conn_read(struct conn *c, void *buf, size_t len)
{
	if (!c->https) {
		int n = sock_nb_recv(c->fd, buf, len);
		if (n > 0)
			g.stats.bytes_in += (uint32_t)n;
		return n;
	}
	for (;;) {
		int rc = mbedtls_ssl_read(&c->ssl, buf, len);
		if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
			return SOCK_WOULD_BLOCK;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
		if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
			continue;
#endif
		if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_SSL_CONN_EOF)
			return 0;
		if (rc > 0)
			g.stats.bytes_in += (uint32_t)rc;
		return rc < 0 ? SOCK_ERR_IO : rc;
	}
}

static void conn_close(struct conn *c)
{
	if (c->state == C_FREE)
		return;
	if (c->ssl_ready) {
		if (c->responses > 0)
			tls_session_store(&c->ssl, c->host);
		mbedtls_ssl_free(&c->ssl);
	}
	if (c->fd >= 0)
		sock_close(c->fd);
	memset(c, 0, sizeof *c);
	c->fd = -1;
}

static int conns_for_host(const struct url *u, int *total)
{
	int n = 0;
	*total = 0;
	for (int i = 0; i < MAX_CONNS; i++) {
		struct conn *c = &g.conns[i];
		if (c->state == C_FREE)
			continue;
		(*total)++;
		if (c->port == u->port && c->https == u->https && strcasecmp(c->host, u->host) == 0)
			n++;
	}
	return n;
}

/* ---- request lifecycle ----------------------------------------------------------------- */

static void free_req(struct http_req *r)
{
	free(r->res.body);
	free(r);
}

static void finish(struct http_req *r, enum http_state st, int err)
{
	r->res.error = err;
	r->res.status = r->hh.status;
	r->res.elapsed_ms = (uint32_t)(plat_ms() - r->start_ms);
	char full[URL_HOST_MAX + URL_PATH_MAX + 16];
	snprintf(full, sizeof full, "%s://%s%s", r->url.https ? "https" : "http", r->url.host, r->url.path);
	memcpy(r->res.final_url, full, sizeof r->res.final_url - 1);
	r->res.final_url[sizeof r->res.final_url - 1] = '\0';
	if (r->res.body)
		r->res.body[r->res.body_len] = '\0';
	if (st == HTTP_DONE)
		g.stats.completed++;
	else
		g.stats.failed++;
	plat_mutex_lock(&g.lock);
	if (r->released) {
		plat_mutex_unlock(&g.lock);
		free_req(r);
		return;
	}
	r->state = (int)st;
	plat_mutex_unlock(&g.lock);
}

static void remove_active(struct http_req *r)
{
	for (struct http_req **p = &g.active; *p; p = &(*p)->next) {
		if (*p == r) {
			*p = r->next;
			return;
		}
	}
}

static void reset_response(struct http_req *r)
{
	r->head_len = 0;
	memset(&r->hh, 0, sizeof r->hh);
	r->res.body_len = 0;
	r->res.total_size = -1;
	r->res.content_type[0] = '\0';
	r->discard = 0;
	r->phase = R_QUEUED;
	r->conn = NULL;
}

/* Detaches r from its connection; the connection is closed (mid-transfer) or kept (idle). */
static void release_conn(struct http_req *r, int keep)
{
	struct conn *c = r->conn;
	if (!c)
		return;
	c->req = NULL;
	r->conn = NULL;
	if (keep) {
		c->responses++;
		c->idle_since = plat_ms();
	} else {
		conn_close(c);
	}
}

/* Per-host cooldown after a 429: every request to that host waits it out, rather than each one
 * retrying on its own (which keeps a rate limit going). Only touched by the network thread. */
static struct {
	char host[URL_HOST_MAX];
	uint64_t until;
} g_cool[4];

static uint64_t cooldown_until(const char *host)
{
	for (int i = 0; i < 4; i++)
		if (g_cool[i].until && strcmp(g_cool[i].host, host) == 0)
			return g_cool[i].until;
	return 0;
}

static void cooldown_set(const char *host, uint64_t until)
{
	int slot = 0;
	for (int i = 0; i < 4; i++) {
		if (strcmp(g_cool[i].host, host) == 0) {
			slot = i;
			break;
		}
		if (g_cool[i].until < g_cool[slot].until)
			slot = i;
	}
	snprintf(g_cool[slot].host, sizeof g_cool[slot].host, "%s", host);
	if (until > g_cool[slot].until)
		g_cool[slot].until = until;
}

/* Network-level failure: retry quietly if nothing was received yet, else give up. */
static void fail_or_retry(struct http_req *r, int err)
{
	int fresh = r->conn && r->conn->responses > 0; /* a stale keep-alive connection */
	release_conn(r, 0);
	if (r->phase <= R_HEAD && r->res.body_len == 0 && (fresh || r->attempts + 1 < MAX_ATTEMPTS)) {
		if (!fresh)
			r->attempts++;
		reset_response(r);
		r->not_before_ms = plat_ms() + (fresh ? 0 : 500u * (unsigned)r->attempts);
		return;
	}
	remove_active(r);
	finish(r, HTTP_FAILED, err);
}

static void build_request(struct http_req *r)
{
	char range[64] = "";
	if (r->opts.range_start >= 0) {
		if (r->opts.range_end >= 0)
			snprintf(range, sizeof range, "Range: bytes=%lld-%lld\r\n", (long long)r->opts.range_start,
				(long long)r->opts.range_end);
		else
			snprintf(range, sizeof range, "Range: bytes=%lld-\r\n", (long long)r->opts.range_start);
	}
	int n = snprintf(r->reqbuf, sizeof r->reqbuf,
		"GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\nAccept: */*\r\n"
		"Accept-Encoding: identity\r\nConnection: keep-alive\r\n%s\r\n",
		r->url.path, r->url.host, range);
	r->req_len = n < (int)sizeof r->reqbuf ? (size_t)n : sizeof r->reqbuf - 1;
	r->req_sent = 0;
}

static int body_append(struct http_req *r, const uint8_t *data, size_t n)
{
	if (r->discard || n == 0)
		return 0;
	size_t need = r->res.body_len + n + 1;
	if (need - 1 > r->opts.max_body)
		return HTTP_ERR_TOO_LARGE;
	if (need > r->cap) {
		size_t cap = r->cap ? r->cap : 16384;
		while (cap < need)
			cap *= 2;
		if (r->hh.content_length > 0 && (size_t)r->hh.content_length + 1 > cap &&
			(size_t)r->hh.content_length <= r->opts.max_body)
			cap = (size_t)r->hh.content_length + 1;
		uint8_t *nb = realloc(r->res.body, cap);
		if (!nb)
			return HTTP_ERR_NOMEM;
		r->res.body = nb;
		r->cap = cap;
	}
	memcpy(r->res.body + r->res.body_len, data, n);
	r->res.body_len += n;
	return 0;
}

/* Response fully received. Decides on keep-alive, redirects and retries. */
static void response_complete(struct http_req *r, int framed)
{
	release_conn(r, framed && r->hh.keep_alive);
	int st = r->hh.status;

	if (st >= 300 && st < 400 && st != 304 && r->hh.location[0]) {
		struct url next;
		if (++r->redirects > MAX_REDIRECTS || url_resolve(&r->url, r->hh.location, &next) != 0) {
			remove_active(r);
			finish(r, HTTP_FAILED, r->redirects > MAX_REDIRECTS ? HTTP_ERR_REDIRECTS : HTTP_ERR_URL);
			return;
		}
		r->url = next;
		reset_response(r);
		return;
	}
	if ((st == 429 || st == 503) && r->attempts + 1 < MAX_ATTEMPTS) {
		r->attempts++;
		int wait = r->hh.retry_after > 0 ? r->hh.retry_after : st == 429 ? 30 : 2 * r->attempts;
		if (st == 429 && wait < 30)
			wait = 30;
		if (wait > 120)
			wait = 120;
		log_printf("http: %d from %s, retrying in %d s\n", st, r->url.host, wait);
		g.stats.throttled++;
		reset_response(r);
		r->throttle_wait = 1;
		r->not_before_ms = plat_ms() + (uint64_t)wait * 1000u;
		if (st == 429)
			cooldown_set(r->url.host, r->not_before_ms);
		return;
	}
	/* A storage node failing (archive.org's /download/ redirects to one of many): start over
	 * from the original URL, so the redirect can pick another node. */
	if ((st == 500 || st == 502 || st == 504) && r->attempts + 1 < MAX_ATTEMPTS) {
		r->attempts++;
		log_printf("http: %d from %s, retrying from %s\n", st, r->url.host, r->first.host);
		r->url = r->first;
		r->redirects = 0;
		reset_response(r);
		r->not_before_ms = plat_ms() + 1000u * (unsigned)r->attempts;
		return;
	}
	remove_active(r);
	finish(r, HTTP_DONE, HTTP_ERR_NONE);
}

/* Feeds received body bytes; returns 1 when the response is complete. */
static int feed_body(struct http_req *r, uint8_t *data, size_t n, int *err)
{
	if (r->hh.chunked) {
		size_t used;
		int out = chunked_feed(&r->ch, data, n, &used);
		if (out < 0) {
			*err = HTTP_ERR_PROTOCOL;
			return 0;
		}
		if ((*err = body_append(r, data, (size_t)out)) != 0)
			return 0;
		return r->ch.done;
	}
	if (r->body_remaining >= 0 && (int64_t)n > r->body_remaining)
		n = (size_t)r->body_remaining;
	if ((*err = body_append(r, data, n)) != 0)
		return 0;
	if (r->body_remaining >= 0) {
		r->body_remaining -= (int64_t)n;
		return r->body_remaining == 0;
	}
	return 0;
}

static void head_parsed(struct http_req *r)
{
	struct http_head *h = &r->hh;
	if (h->date[0])
		tls_note_server_date(h->date);
	snprintf(r->res.content_type, sizeof r->res.content_type, "%s", h->content_type);
	r->res.total_size = h->range_total >= 0 ? h->range_total : h->content_length;
	int st = h->status;
	r->discard = (st >= 300 && st < 400 && h->location[0]) ||
		((st == 429 || st == 503 || st == 500 || st == 502 || st == 504) && r->attempts + 1 < MAX_ATTEMPTS);
	if (h->chunked) {
		chunked_init(&r->ch);
	} else if (st == 204 || st == 304 || (st >= 100 && st < 200)) {
		r->body_remaining = 0;
	} else if (h->content_length >= 0) {
		r->body_remaining = h->content_length;
	} else {
		r->body_remaining = -1;
		h->keep_alive = 0;
	}
	r->phase = R_BODY;
}

/* Advances r on its ready connection as far as possible without blocking. */
static void drive_req(struct http_req *r)
{
	struct conn *c = r->conn;
	for (;;) {
		if (r->phase == R_SEND) {
			int n = conn_write(c, r->reqbuf + r->req_sent, r->req_len - r->req_sent);
			if (n == SOCK_WOULD_BLOCK)
				return;
			if (n <= 0) {
				fail_or_retry(r, HTTP_ERR_IO);
				return;
			}
			r->req_sent += (size_t)n;
			if (r->req_sent == r->req_len)
				r->phase = R_HEAD;
		} else if (r->phase == R_HEAD) {
			int n = conn_read(c, r->head + r->head_len, HEAD_MAX - 1 - r->head_len);
			if (n == SOCK_WOULD_BLOCK)
				return;
			if (n <= 0) {
				fail_or_retry(r, HTTP_ERR_IO);
				return;
			}
			r->head_len += (size_t)n;
			r->head[r->head_len] = '\0';
			char *end = strstr(r->head, "\r\n\r\n");
			if (!end) {
				if (r->head_len >= HEAD_MAX - 1) {
					release_conn(r, 0);
					remove_active(r);
					finish(r, HTTP_FAILED, HTTP_ERR_PROTOCOL);
				}
				continue;
			}
			size_t head_bytes = (size_t)(end - r->head) + 4;
			if (http_parse_head(r->head, head_bytes - 2, &r->hh) != 0) {
				release_conn(r, 0);
				remove_active(r);
				finish(r, HTTP_FAILED, HTTP_ERR_PROTOCOL);
				return;
			}
			head_parsed(r);
			size_t extra = r->head_len - head_bytes;
			int err = 0, done = r->body_remaining == 0 && !r->hh.chunked;
			if (extra && !done)
				done = feed_body(r, (uint8_t *)r->head + head_bytes, extra, &err);
			if (err) {
				release_conn(r, 0);
				remove_active(r);
				finish(r, HTTP_FAILED, err);
				return;
			}
			if (done) {
				response_complete(r, 1);
				return;
			}
		} else if (r->phase == R_BODY) {
			int n = conn_read(c, g.rx, sizeof g.rx);
			if (n == SOCK_WOULD_BLOCK)
				return;
			if (n == 0 && r->body_remaining < 0 && !r->hh.chunked) {
				response_complete(r, 0); /* body delimited by close */
				return;
			}
			if (n <= 0) {
				fail_or_retry(r, HTTP_ERR_IO);
				return;
			}
			int err = 0;
			int done = feed_body(r, g.rx, (size_t)n, &err);
			if (err) {
				release_conn(r, 0);
				remove_active(r);
				finish(r, HTTP_FAILED, err);
				return;
			}
			if (done) {
				response_complete(r, 1);
				return;
			}
		} else {
			return;
		}
	}
}

/* Gives a queued request a connection: an idle one to the same host, a new one, or (when
 * full) a new one in place of an idle connection to another host. */
static void try_assign(struct http_req *r)
{
	for (int i = 0; i < MAX_CONNS; i++) {
		struct conn *c = &g.conns[i];
		if (c->state == C_READY && !c->req && c->port == r->url.port && c->https == r->url.https &&
			strcasecmp(c->host, r->url.host) == 0) {
			c->req = r;
			r->conn = c;
			build_request(r);
			r->phase = R_SEND;
			return;
		}
	}
	int total;
	if (conns_for_host(&r->url, &total) >= MAX_PER_HOST)
		return;
	uint32_t addr;
	int dns = sock_resolve_async(r->url.host, &addr);
	if (dns == SOCK_WOULD_BLOCK)
		return; /* looked up on the resolver thread; try again next pass */
	if (dns != 0) {
		remove_active(r);
		finish(r, HTTP_FAILED, HTTP_ERR_DNS);
		return;
	}
	struct conn *slot = NULL;
	for (int i = 0; i < MAX_CONNS && !slot; i++)
		if (g.conns[i].state == C_FREE)
			slot = &g.conns[i];
	if (!slot) {
		for (int i = 0; i < MAX_CONNS && !slot; i++)
			if (g.conns[i].state == C_READY && !g.conns[i].req) {
				conn_close(&g.conns[i]);
				slot = &g.conns[i];
			}
	}
	if (!slot)
		return;

	int fd = sock_nb_open(addr, r->url.port);
	if (fd < 0) {
		fail_or_retry(r, HTTP_ERR_CONNECT);
		return;
	}
	memset(slot, 0, sizeof *slot);
	slot->fd = fd;
	slot->state = C_CONNECTING;
	slot->https = r->url.https;
	slot->port = r->url.port;
	slot->addr = addr;
	snprintf(slot->host, sizeof slot->host, "%s", r->url.host);
	slot->req = r;
	r->conn = slot;
	build_request(r);
	r->phase = R_SEND;
}

/* Advances a connection's setup; when ready, its request. */
static void drive_conn(struct conn *c)
{
	if (c->state == C_CONNECTING) {
		int rc = sock_nb_connect_poll(c->fd, c->addr, c->port);
		if (rc == 0)
			return;
		if (rc < 0) {
			if (c->req)
				fail_or_retry(c->req, HTTP_ERR_CONNECT);
			else
				conn_close(c);
			return;
		}
		if (!c->https) {
			c->state = C_READY;
		} else {
			mbedtls_ssl_init(&c->ssl);
			c->ssl_ready = 1;
			if (mbedtls_ssl_setup(&c->ssl, tls_shared_config()) != 0 ||
				mbedtls_ssl_set_hostname(&c->ssl, c->host) != 0) {
				if (c->req)
					fail_or_retry(c->req, HTTP_ERR_TLS);
				return;
			}
			mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
			tls_session_offer(&c->ssl, c->host);
			c->state = C_HANDSHAKE;
		}
	}
	if (c->state == C_HANDSHAKE) {
		int rc = mbedtls_ssl_handshake(&c->ssl);
		if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
			return;
		if (rc != 0) {
			uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
			log_printf("http: TLS handshake with %s failed: -0x%04x (verify flags 0x%x)\n", c->host,
				(unsigned)-rc, (unsigned)flags);
			if (c->req)
				fail_or_retry(c->req, HTTP_ERR_TLS);
			else
				conn_close(c);
			return;
		}
		c->state = C_READY;
	}
	if (c->state == C_READY && c->req)
		drive_req(c->req);
}

/* ---- the network thread ---------------------------------------------------------------- */

static void *net_thread(void *arg)
{
	(void)arg;
	while (g.running) {
		uint64_t now = plat_ms();

		/* Intake and cancellations. */
		plat_mutex_lock(&g.lock);
		while (g.incoming) {
			struct http_req *r = g.incoming;
			g.incoming = r->next;
			r->next = g.active;
			g.active = r;
		}
		for (struct http_req **p = &g.active; *p;) {
			struct http_req *r = *p;
			if (r->released) {
				*p = r->next;
				release_conn(r, 0);
				free_req(r);
				continue;
			}
			p = &r->next;
		}
		plat_mutex_unlock(&g.lock);

		/* Deadlines, then connections for queued requests (higher priority first). */
		for (struct http_req *r = g.active, *next; r; r = next) {
			next = r->next;
			if (now > r->deadline_ms) {
				release_conn(r, 0);
				remove_active(r);
				finish(r, HTTP_FAILED, HTTP_ERR_TIMEOUT);
			}
		}
		for (int pass = 0; pass < 2; pass++)
			for (struct http_req *r = g.active, *next; r; r = next) {
				next = r->next;
				int high = r->opts.priority > 0;
				if (r->phase == R_QUEUED && !r->conn && now >= r->not_before_ms && (pass == 0) == high &&
					now >= cooldown_until(r->url.host))
					try_assign(r);
			}

		/* Drive every connection, retire stale idle ones, then wait for socket activity. */
		int fds[MAX_CONNS], ev[MAX_CONNS], rev[MAX_CONNS], n = 0, pending = 0, busy = 0;
		for (int i = 0; i < MAX_CONNS; i++) {
			struct conn *c = &g.conns[i];
			if (c->state == C_FREE)
				continue;
			drive_conn(c);
			if (c->state == C_FREE)
				continue;
			if (c->state == C_READY && !c->req) {
				if (plat_ms() - c->idle_since > IDLE_KEEP_MS)
					conn_close(c);
				continue;
			}
			busy++;
			int want = SOCK_POLL_IN;
			if (c->state == C_CONNECTING || (c->req && c->req->phase == R_SEND))
				want = SOCK_POLL_OUT;
			if (c->ssl_ready && mbedtls_ssl_get_bytes_avail(&c->ssl) > 0)
				pending = 1;
			fds[n] = c->fd;
			ev[n] = want | (c->state == C_HANDSHAKE ? SOCK_POLL_OUT : 0);
			n++;
		}
		int queued = 0, waiting = 0;
		for (struct http_req *r = g.active; r; r = r->next)
			if (!r->conn) {
				queued++;
				waiting += (r->throttle_wait && now < r->not_before_ms) || now < cooldown_until(r->url.host);
			}
		g.stats.active = (uint32_t)busy;
		g.stats.queued = (uint32_t)queued;
		g.stats.waiting = (uint32_t)waiting;
		g.stats.connections = 0;
		for (int i = 0; i < MAX_CONNS; i++)
			if (g.conns[i].state != C_FREE)
				g.stats.connections++;

		if (pending)
			continue;
		if (n > 0)
			sock_poll_many(fds, ev, rev, n, queued ? 10 : 20);
		else
			usleep(queued ? 10000 : 30000);
	}
	return NULL;
}

int http_init(void)
{
	memset(&g, 0, sizeof g);
	for (int i = 0; i < MAX_CONNS; i++)
		g.conns[i].fd = -1;
	plat_mutex_init(&g.lock);
	g.running = 1;
	/* NET priority between the UI (64) and audio (100), per WiiLibs' conventions. */
	if (plat_thread_create(&g.thread, net_thread, NULL, 64 * 1024, 80) != 0) {
		g.running = 0;
		return -1;
	}
	return 0;
}

void http_shutdown(void)
{
	if (!g.running)
		return;
	g.running = 0;
	plat_thread_join(g.thread);
	for (int i = 0; i < MAX_CONNS; i++)
		conn_close(&g.conns[i]);
	struct http_req *lists[2] = {g.incoming, g.active};
	for (int l = 0; l < 2; l++)
		for (struct http_req *r = lists[l], *next; r; r = next) {
			next = r->next;
			if (r->released)
				free_req(r);
			else
				r->state = HTTP_FAILED, r->res.error = HTTP_ERR_SHUTDOWN;
		}
	g.incoming = g.active = NULL;
}

http_req *http_get(const char *url, const struct http_opts *opts)
{
	static struct http_req oom = {.state = HTTP_FAILED, .res = {.error = HTTP_ERR_NOMEM}};
	struct http_req *r = calloc(1, sizeof *r);
	if (!r)
		return &oom;
	if (opts)
		r->opts = *opts;
	else
		http_opts_default(&r->opts);
	if (r->opts.max_body == 0)
		r->opts.max_body = 4u << 20;
	if (r->opts.timeout_ms <= 0)
		r->opts.timeout_ms = 60000;
	r->res.total_size = -1;
	r->start_ms = plat_ms();
	r->deadline_ms = r->start_ms + (uint64_t)r->opts.timeout_ms;
	if (url_parse(url, &r->url) != 0) {
		r->state = HTTP_FAILED;
		r->res.error = HTTP_ERR_URL;
		return r;
	}
	r->first = r->url;
	plat_mutex_lock(&g.lock);
	if (!g.running) {
		r->state = HTTP_FAILED;
		r->res.error = HTTP_ERR_SHUTDOWN;
	} else {
		r->next = g.incoming;
		g.incoming = r;
	}
	plat_mutex_unlock(&g.lock);
	return r;
}

enum http_state http_state(const http_req *r)
{
	return (enum http_state)r->state;
}

const struct http_result *http_result(const http_req *r)
{
	return &r->res;
}

void http_release(http_req *r)
{
	if (!r || (r->res.error == HTTP_ERR_NOMEM && r->state == HTTP_FAILED && !r->start_ms))
		return; /* the static out-of-memory request */
	plat_mutex_lock(&g.lock);
	if (r->state == HTTP_PENDING && g.running) {
		r->released = 1; /* the net thread frees it */
		plat_mutex_unlock(&g.lock);
		return;
	}
	plat_mutex_unlock(&g.lock);
	free_req(r);
}

void http_get_stats(struct http_stats *out)
{
	*out = g.stats;
}
