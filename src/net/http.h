/* Asynchronous HTTP(S) client.
 *
 * One network thread runs an event loop over non-blocking sockets and non-blocking mbedTLS
 * contexts, polling every connection with a single poll call. (A thread per connection, each
 * blocking in its own poll+recv, drew IOS -22 errors on hardware.)
 *
 * The UI thread starts requests and polls their state each frame; nothing calls back into the
 * UI. Bodies are buffered in memory up to opts.max_body. Keep-alive connections are pooled per
 * host; redirects, chunked bodies, Range requests, and retries after 429/503 and 500/502/504 are handled here. */
#ifndef IAC_HTTP_H
#define IAC_HTTP_H

#include <stddef.h>
#include <stdint.h>

enum http_state { HTTP_PENDING = 0, HTTP_DONE, HTTP_FAILED };

enum {
	HTTP_ERR_NONE = 0,
	HTTP_ERR_URL,
	HTTP_ERR_DNS,
	HTTP_ERR_CONNECT,
	HTTP_ERR_TLS,
	HTTP_ERR_IO,
	HTTP_ERR_PROTOCOL,
	HTTP_ERR_TOO_LARGE,
	HTTP_ERR_TIMEOUT,
	HTTP_ERR_REDIRECTS,
	HTTP_ERR_NOMEM,
	HTTP_ERR_SHUTDOWN,
};

struct http_opts {
	int64_t range_start; /* -1: no Range header */
	int64_t range_end;   /* inclusive; -1: open-ended */
	size_t max_body;     /* default 4 MiB */
	int timeout_ms;      /* whole request including redirects/retries; default 60 s */
	int priority;        /* higher first when connections are scarce (thumbnails 0, pages 10) */
};

typedef struct http_req http_req;

/* Results, valid once http_state() != HTTP_PENDING. */
struct http_result {
	int status;           /* final HTTP status, 0 if none */
	int error;            /* HTTP_ERR_* when FAILED */
	uint8_t *body;        /* NUL-terminated for convenience (not counted in body_len) */
	size_t body_len;
	int64_t total_size;   /* entity size from Content-Range or Content-Length, -1 unknown */
	char content_type[64];
	char final_url[512];
	uint32_t elapsed_ms;
};

/* Needs sock_init() and tls_global_init() first. Starts the network thread. */
int http_init(void);
void http_shutdown(void);

void http_opts_default(struct http_opts *o);
/* Never NULL (out-of-memory requests come back FAILED). opts may be NULL. */
http_req *http_get(const char *url, const struct http_opts *opts);
enum http_state http_state(const http_req *r);
const struct http_result *http_result(const http_req *r);
/* Frees the request; cancels it if still running. The pointer is invalid afterwards. */
void http_release(http_req *r);

const char *http_strerror(int err);

/* Counters for the debug overlay. */
struct http_stats {
	uint32_t active, queued, connections, completed, failed, bytes_in;
	uint32_t throttled; /* 429/503 answers retried so far (the server is rate-limiting us) */
	uint32_t waiting;   /* requests held back right now by a 429/503 (retry wait or host cooldown) */
};
void http_get_stats(struct http_stats *out);

#endif
