/* Pure parsing helpers for the HTTP client (no I/O; unit-tested on the host). */
#ifndef IAC_HTTP_PARSE_H
#define IAC_HTTP_PARSE_H

#include <stddef.h>
#include <stdint.h>

struct http_head {
	int status;
	int64_t content_length; /* -1 if absent */
	int chunked;
	int keep_alive;   /* HTTP/1.1 default unless "Connection: close" */
	int64_t range_total; /* from Content-Range ".../total", -1 if absent */
	int retry_after;     /* seconds, -1 if absent */
	char location[1024];
	char content_type[64];
	char date[64];
};

/* Parses a header block (status line + headers, without the final blank line). 0 = ok. */
int http_parse_head(const char *block, size_t len, struct http_head *out);

/* Chunked transfer decoding, incremental. */
struct chunked {
	int state;
	uint64_t remaining;
	int done;
};
void chunked_init(struct chunked *c);
/* Consumes bytes from in, writing decoded payload in place to the front of in.
 * Returns the number of payload bytes produced, or -1 on a malformed stream.
 * *consumed gets how many input bytes were used (may be < len once done). */
int chunked_feed(struct chunked *c, uint8_t *in, size_t len, size_t *consumed);

#endif
