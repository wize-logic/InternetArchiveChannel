#include "net/http_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void copy_value(const char *v, size_t len, char *out, size_t cap)
{
	while (len && (*v == ' ' || *v == '\t')) {
		v++;
		len--;
	}
	while (len && (v[len - 1] == ' ' || v[len - 1] == '\t'))
		len--;
	if (len >= cap)
		len = cap - 1;
	memcpy(out, v, len);
	out[len] = '\0';
}

int http_parse_head(const char *block, size_t len, struct http_head *h)
{
	memset(h, 0, sizeof *h);
	h->content_length = -1;
	h->range_total = -1;
	h->retry_after = -1;
	int minor = 1;
	if (len < 12 || sscanf(block, "HTTP/1.%d %d", &minor, &h->status) != 2)
		return -1;
	h->keep_alive = minor >= 1;

	const char *p = block, *end = block + len;
	const char *nl = memchr(p, '\n', (size_t)(end - p));
	p = nl ? nl + 1 : end;
	while (p < end) {
		nl = memchr(p, '\n', (size_t)(end - p));
		const char *le = nl ? nl : end;
		size_t ll = (size_t)(le - p);
		if (ll && p[ll - 1] == '\r')
			ll--;
		const char *colon = memchr(p, ':', ll);
		if (colon) {
			size_t kl = (size_t)(colon - p);
			const char *v = colon + 1;
			size_t vl = ll - kl - 1;
			char val[1024];
			copy_value(v, vl, val, sizeof val);
#define KEY(k) (kl == sizeof(k) - 1 && strncasecmp(p, k, kl) == 0)
			if (KEY("Content-Length"))
				h->content_length = strtoll(val, NULL, 10);
			else if (KEY("Transfer-Encoding"))
				h->chunked = strstr(val, "chunked") != NULL || strstr(val, "Chunked") != NULL;
			else if (KEY("Connection"))
				h->keep_alive = strcasecmp(val, "close") != 0 && (h->keep_alive || strcasecmp(val, "keep-alive") == 0);
			else if (KEY("Location"))
				copy_value(v, vl, h->location, sizeof h->location);
			else if (KEY("Content-Type"))
				copy_value(v, vl, h->content_type, sizeof h->content_type);
			else if (KEY("Date"))
				copy_value(v, vl, h->date, sizeof h->date);
			else if (KEY("Retry-After"))
				h->retry_after = atoi(val);
			else if (KEY("Content-Range")) {
				const char *slash = strrchr(val, '/');
				if (slash && slash[1] != '*')
					h->range_total = strtoll(slash + 1, NULL, 10);
			}
#undef KEY
		}
		p = nl ? nl + 1 : end;
	}
	return 0;
}

enum { CH_SIZE, CH_EXT, CH_SIZE_LF, CH_DATA, CH_DATA_CR, CH_DATA_LF, CH_TRAILER, CH_TRAILER_LF };

void chunked_init(struct chunked *c)
{
	memset(c, 0, sizeof *c);
}

static int hexval(uint8_t ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

int chunked_feed(struct chunked *c, uint8_t *in, size_t len, size_t *consumed)
{
	size_t i = 0, out = 0;
	int line_empty = 1; /* trailer: an empty line ends the message */
	while (i < len && !c->done) {
		uint8_t ch = in[i];
		switch (c->state) {
		case CH_SIZE: {
			int v = hexval(ch);
			if (v >= 0) {
				if (c->remaining >> 60)
					return -1;
				c->remaining = c->remaining * 16 + (uint64_t)v;
				i++;
			} else if (ch == ';' || ch == ' ') {
				c->state = CH_EXT;
				i++;
			} else if (ch == '\r') {
				c->state = CH_SIZE_LF;
				i++;
			} else {
				return -1;
			}
			break;
		}
		case CH_EXT:
			if (ch == '\r')
				c->state = CH_SIZE_LF;
			i++;
			break;
		case CH_SIZE_LF:
			if (ch != '\n')
				return -1;
			i++;
			c->state = c->remaining ? CH_DATA : CH_TRAILER;
			line_empty = 1;
			break;
		case CH_DATA: {
			size_t n = len - i;
			if (n > c->remaining)
				n = (size_t)c->remaining;
			memmove(in + out, in + i, n);
			out += n;
			i += n;
			c->remaining -= n;
			if (c->remaining == 0)
				c->state = CH_DATA_CR;
			break;
		}
		case CH_DATA_CR:
			if (ch != '\r')
				return -1;
			c->state = CH_DATA_LF;
			i++;
			break;
		case CH_DATA_LF:
			if (ch != '\n')
				return -1;
			c->state = CH_SIZE;
			i++;
			break;
		case CH_TRAILER:
			if (ch == '\r') {
				c->state = CH_TRAILER_LF;
			} else {
				line_empty = 0;
			}
			i++;
			break;
		case CH_TRAILER_LF:
			if (ch != '\n')
				return -1;
			i++;
			if (line_empty)
				c->done = 1;
			else
				c->state = CH_TRAILER;
			line_empty = 1;
			break;
		}
	}
	*consumed = i;
	return (int)out;
}
