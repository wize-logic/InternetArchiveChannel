/* Host unit test: response head parsing and chunked decoding (including split input). */
#include <stdio.h>
#include <string.h>

#include "net/http_parse.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static int decode_all(const char *in, size_t step, char *out)
{
	struct chunked c;
	chunked_init(&c);
	size_t total = strlen(in), pos = 0, n = 0;
	uint8_t buf[256];
	while (pos < total && !c.done) {
		size_t len = total - pos < step ? total - pos : step, used;
		memcpy(buf, in + pos, len);
		int got = chunked_feed(&c, buf, len, &used);
		if (got < 0)
			return -1;
		memcpy(out + n, buf, (size_t)got);
		n += (size_t)got;
		pos += used;
	}
	out[n] = '\0';
	return c.done ? (int)n : -2;
}

int main(void)
{
	struct http_head h;
	const char *head = "HTTP/1.1 206 Partial Content\r\nContent-Type: video/mp4\r\n"
		"Content-Range: bytes 0-99/38874589\r\nContent-Length: 100\r\nConnection: keep-alive\r\n"
		"Date: Fri, 09 Oct 2026 17:15:59 GMT\r\n";
	CHECK(http_parse_head(head, strlen(head), &h) == 0);
	CHECK(h.status == 206 && h.content_length == 100 && h.range_total == 38874589);
	CHECK(h.keep_alive && !h.chunked && strcmp(h.content_type, "video/mp4") == 0);
	CHECK(strcmp(h.date, "Fri, 09 Oct 2026 17:15:59 GMT") == 0);

	const char *redir = "HTTP/1.1 302 Found\r\nLocation:  https://ia800.us.archive.org/x  \r\nConnection: close\r\n";
	CHECK(http_parse_head(redir, strlen(redir), &h) == 0);
	CHECK(h.status == 302 && !h.keep_alive && strcmp(h.location, "https://ia800.us.archive.org/x") == 0);

	const char *ch = "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\nRetry-After: 5\r\n";
	CHECK(http_parse_head(ch, strlen(ch), &h) == 0 && h.chunked && h.retry_after == 5);
	CHECK(http_parse_head("garbage", 7, &h) != 0);

	char out[256];
	const char *body = "4\r\nWiki\r\n6;ext=1\r\npedia \r\nE\r\nin \r\n\r\nchunks.\r\n0\r\nX-T: y\r\n\r\n";
	CHECK(decode_all(body, 1000, out) == 24 && strcmp(out, "Wikipedia in \r\n\r\nchunks.") == 0);
	for (size_t step = 1; step < 9; step++)
		CHECK(decode_all(body, step, out) == 24 && strcmp(out, "Wikipedia in \r\n\r\nchunks.") == 0);
	CHECK(decode_all("0\r\n\r\n", 3, out) == 0);
	CHECK(decode_all("zz\r\n", 4, out) == -1);

	printf("%s\n", failures ? "FAILED" : "ok");
	return failures != 0;
}
