/* Host unit test for third_party/compat/src/iconv.c (built with renamed symbols). */
#include <stdio.h>
#include <string.h>

typedef void *iconv_t;
iconv_t t_iconv_open(const char *to, const char *from);
size_t t_iconv(iconv_t cd, char **in, size_t *inl, char **out, size_t *outl);
int t_iconv_close(iconv_t cd);

static int failures;

static void check(const char *to, const char *from, const char *in, size_t inlen,
	const char *want, size_t wantlen)
{
	char out[64], *ip = (char *)in, *op = out;
	size_t il = inlen, ol = sizeof out;
	iconv_t cd = t_iconv_open(to, from);
	size_t r = cd == (iconv_t)-1 ? (size_t)-1 : t_iconv(cd, &ip, &il, &op, &ol);
	size_t got = sizeof out - ol;
	if (r == (size_t)-1 || got != wantlen || memcmp(out, want, wantlen) != 0) {
		printf("FAIL %s -> %s (r=%ld, %zu bytes)\n", from, to, (long)r, got);
		failures++;
	}
	if (cd != (iconv_t)-1)
		t_iconv_close(cd);
}

int main(void)
{
	check("ISO-8859-1", "UTF-8", "caf\xc3\xa9", 5, "caf\xe9", 4);
	check("UTF-8", "WINDOWS-1252", "\x80\x93", 2, "\xe2\x82\xac\xe2\x80\x9c", 6);
	check("UTF-16BE", "UTF-8", "\xf0\x9f\x98\x80", 4, "\xd8\x3d\xde\x00", 4);
	check("UTF-8", "UTF-16LE", "\x3d\xd8\x00\xde", 4, "\xf0\x9f\x98\x80", 4);
	check("UCS-4", "UTF-8", "A", 1, "\0\0\0A", 4);
	check("ISO-8859-1//TRANSLIT", "UTF-8", "a\xe2\x82\xac", 4, "a?", 2);
	check("US-ASCII//IGNORE", "UTF-8", "a\xc3\xa9z", 4, "az", 2);
	if (t_iconv_open("UTF-8", "KOI8-R") != (iconv_t)-1) {
		printf("FAIL unsupported encoding accepted\n");
		failures++;
	}
	printf("%s\n", failures ? "FAILED" : "ok");
	return failures != 0;
}
