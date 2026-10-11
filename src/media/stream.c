#include "media/stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/platform.h"
#include "net/http.h"

#define MAX_CHUNKS 8
#define RETRIES 3

struct chunk {
	int64_t start;
	http_req *req; /* NULL: free slot */
	int tries;
};

struct media_stream {
	char url[1024];       /* after the first response: the storage node's URL */
	int have_final;
	size_t chunk_size;
	int ahead;
	int64_t pos, size;
	struct chunk chunks[MAX_CHUNKS];
	volatile int interrupted;
	char error[96];
};

media_stream *stream_open(const char *url, size_t chunk_size, int ahead)
{
	media_stream *s = calloc(1, sizeof *s);
	if (!s)
		return NULL;
	snprintf(s->url, sizeof s->url, "%s", url);
	s->chunk_size = chunk_size ? chunk_size : 256 * 1024;
	s->ahead = ahead < 0 ? 0 : ahead > MAX_CHUNKS - 1 ? MAX_CHUNKS - 1 : ahead;
	s->size = -1;
	return s;
}

static void drop(struct chunk *c)
{
	if (c->req)
		http_release(c->req);
	memset(c, 0, sizeof *c);
}

void stream_close(media_stream *s)
{
	if (!s)
		return;
	for (int i = 0; i < MAX_CHUNKS; i++)
		drop(&s->chunks[i]);
	free(s);
}

static struct chunk *find(media_stream *s, int64_t start)
{
	for (int i = 0; i < MAX_CHUNKS; i++)
		if (s->chunks[i].req && s->chunks[i].start == start)
			return &s->chunks[i];
	return NULL;
}

static void request(media_stream *s, struct chunk *c, int64_t start)
{
	struct http_opts o;
	http_opts_default(&o);
	o.range_start = start;
	o.range_end = start + (int64_t)s->chunk_size - 1;
	o.max_body = s->chunk_size;
	o.timeout_ms = 45000;
	o.priority = 5;
	c->start = start;
	c->req = http_get(s->url, &o);
}

/* Keeps the chunk at the read position and `ahead` more in flight; drops the rest. */
static void schedule(media_stream *s)
{
	int64_t base = s->pos - s->pos % (int64_t)s->chunk_size;
	for (int i = 0; i < MAX_CHUNKS; i++) {
		struct chunk *c = &s->chunks[i];
		if (c->req && (c->start < base || c->start > base + (int64_t)s->chunk_size * s->ahead))
			drop(c);
	}
	/* Until the first response tells us the size and the final URL, ask for one chunk only. */
	int want = s->have_final ? s->ahead : 0;
	for (int k = 0; k <= want; k++) {
		int64_t start = base + (int64_t)k * (int64_t)s->chunk_size;
		if (s->size >= 0 && start >= s->size)
			break;
		if (find(s, start))
			continue;
		for (int i = 0; i < MAX_CHUNKS; i++)
			if (!s->chunks[i].req) {
				request(s, &s->chunks[i], start);
				break;
			}
	}
}

int stream_read(media_stream *s, uint8_t *buf, size_t n)
{
	for (;;) {
		if (s->interrupted)
			return -1;
		if (s->size >= 0 && s->pos >= s->size)
			return 0;
		schedule(s);
		int64_t base = s->pos - s->pos % (int64_t)s->chunk_size;
		struct chunk *c = find(s, base);
		if (!c) {
			usleep(5000);
			continue;
		}
		enum http_state st = http_state(c->req);
		if (st == HTTP_PENDING) {
			usleep(5000);
			continue;
		}
		const struct http_result *r = http_result(c->req);
		if (st == HTTP_FAILED || (r->status != 206 && r->status != 200)) {
			if (++c->tries < RETRIES) {
				int tries = c->tries;
				http_release(c->req);
				request(s, c, base);
				c->tries = tries;
				continue;
			}
			snprintf(s->error, sizeof s->error, "%s (HTTP %d)",
				st == HTTP_FAILED ? http_strerror(r->error) : "download failed", r->status);
			return -1;
		}
		if (!s->have_final) {
			/* Later chunks go straight to the storage node archive.org redirected us to. */
			if (r->final_url[0])
				snprintf(s->url, sizeof s->url, "%s", r->final_url);
			s->have_final = 1;
		}
		if (r->total_size >= 0)
			s->size = r->total_size;
		else if (r->status == 200)
			s->size = (int64_t)r->body_len; /* server ignored Range: whole file in one go */
		size_t off = (size_t)(s->pos - c->start);
		if (off >= r->body_len) {
			if (r->body_len < s->chunk_size) /* short chunk: end of file */
				return 0;
			usleep(1000);
			continue;
		}
		size_t take = r->body_len - off < n ? r->body_len - off : n;
		memcpy(buf, r->body + off, take);
		s->pos += (int64_t)take;
		return (int)take;
	}
}

int stream_seek(media_stream *s, int64_t pos)
{
	if (pos < 0)
		pos = 0;
	if (s->size >= 0 && pos > s->size)
		pos = s->size;
	s->pos = pos;
	return 0;
}

int64_t stream_tell(const media_stream *s)
{
	return s->pos;
}

int64_t stream_size(const media_stream *s)
{
	return s->size;
}

void stream_interrupt(media_stream *s)
{
	s->interrupted = 1;
}

void stream_resume(media_stream *s)
{
	s->interrupted = 0;
}

int64_t stream_buffered(const media_stream *s)
{
	int64_t got = 0;
	int64_t at = s->pos - s->pos % (int64_t)s->chunk_size;
	for (;;) {
		const struct chunk *c = NULL;
		for (int i = 0; i < MAX_CHUNKS && !c; i++)
			if (s->chunks[i].req && s->chunks[i].start == at)
				c = &s->chunks[i];
		if (!c || http_state(c->req) != HTTP_DONE)
			break;
		got += (int64_t)http_result(c->req)->body_len;
		at += (int64_t)s->chunk_size;
	}
	got -= s->pos % (int64_t)s->chunk_size;
	return got < 0 ? 0 : got;
}

const char *stream_error(const media_stream *s)
{
	return s->error;
}
