/* Host unit test: URL parsing, redirect resolution, query encoding. */
#include <stdio.h>
#include <string.h>

#include "net/url.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
	struct url u, r;
	CHECK(url_parse("https://archive.org/metadata/nasa?x=1#frag", &u) == 0);
	CHECK(u.https && u.port == 443 && strcmp(u.host, "archive.org") == 0);
	CHECK(strcmp(u.path, "/metadata/nasa?x=1") == 0);
	CHECK(url_parse("http://web.archive.org:8080", &u) == 0 && u.port == 8080 && strcmp(u.path, "/") == 0);
	CHECK(url_parse("ftp://x/", &u) != 0);
	CHECK(url_parse("https://user@x/", &u) != 0);
	CHECK(url_parse("https://:80/", &u) != 0);

	url_parse("https://archive.org/download/id/file.mp4", &u);
	CHECK(url_resolve(&u, "https://ia800.us.archive.org/0/items/id/file.mp4", &r) == 0 &&
		strcmp(r.host, "ia800.us.archive.org") == 0);
	CHECK(url_resolve(&u, "//dn1.us.archive.org/x", &r) == 0 && r.https && strcmp(r.path, "/x") == 0);
	CHECK(url_resolve(&u, "/services/img/id", &r) == 0 && strcmp(r.host, "archive.org") == 0 &&
		strcmp(r.path, "/services/img/id") == 0);
	CHECK(url_resolve(&u, "other.mp4", &r) == 0 && strcmp(r.path, "/download/id/other.mp4") == 0);

	char buf[64];
	CHECK(url_encode("collection:etree AND \"grateful dead\"", buf, sizeof buf) > 0);
	CHECK(strcmp(buf, "collection%3Aetree%20AND%20%22grateful%20dead%22") == 0);
	CHECK(url_encode("caf\xc3\xa9", buf, sizeof buf) > 0 && strcmp(buf, "caf%C3%A9") == 0);
	CHECK(url_encode("abcdef", buf, 4) == -1);

	printf("%s\n", failures ? "FAILED" : "ok");
	return failures != 0;
}
