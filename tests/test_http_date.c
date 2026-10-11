/* Host unit test: HTTP Date parsing used to correct the Wii clock before cert checks. */
#include <stdio.h>
#include <time.h>

#include "net/tls.h"

static int failures;

static void expect(const char *s, int ok, long long want)
{
	time_t t = 0;
	int rc = tls_parse_http_date(s, &t);
	if ((rc == 0) != ok || (ok && (long long)t != want)) {
		printf("FAIL '%s': rc=%d t=%lld want ok=%d t=%lld\n", s, rc, (long long)t, ok, want);
		failures++;
	}
}

int main(void)
{
	expect("Sun, 06 Nov 1994 08:49:37 GMT", 1, 784111777LL);
	expect("Fri, 09 Oct 2026 17:15:59 GMT", 1, 1791566159LL);
	expect("Thu, 01 Jan 1970 00:00:00 GMT", 1, 0);
	expect("Tue, 29 Feb 2028 12:00:00 GMT", 1, 1835438400LL);
	expect("garbage", 0, 0);
	expect("Fri, 09 Foo 2026 17:15:59 GMT", 0, 0);
	expect("Fri, 09 ctN 2026 17:15:59 GMT", 0, 0);
	printf("%s\n", failures ? "FAILED" : "ok");
	return failures != 0;
}
