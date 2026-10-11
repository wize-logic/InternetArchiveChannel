/* Manual live test (needs the network): concurrent archive.org requests through net/http.c.
 * Run: build/host/http_live */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "core/log.h"
#include "core/platform.h"
#include "net/http.h"
#include "net/sock.h"
#include "net/tls.h"

int main(void)
{
	log_init(NULL);
	if (sock_init() != 0 || tls_global_init(NULL, NULL, TLS_FLAGS_DEFAULT) != 0 || http_init() != 0) {
		printf("init failed\n");
		return 1;
	}
	static const char *ids[] = {"nasa", "gd1977-05-08.shure57.stevenson.29303.flac16", "AboutBan1935",
		"alicesadventures00carr", "122Eyes1950", "ParkCons1938", "oldtimeradio", "prelinger"};
	enum { N = 12 };
	http_req *reqs[N];
	const char *names[N];
	char urls[N][256];
	int n = 0;
	snprintf(urls[n], 256, "https://archive.org/metadata/nasa/metadata");
	names[n] = "metadata";
	reqs[n] = http_get(urls[n], NULL);
	n++;
	snprintf(urls[n], 256,
		"https://archive.org/advancedsearch.php?q=collection%%3Aprelinger&fl%%5B%%5D=identifier&fl%%5B%%5D=title"
		"&sort%%5B%%5D=downloads+desc&rows=10&page=1&output=json");
	names[n] = "search";
	reqs[n] = http_get(urls[n], NULL);
	n++;
	for (int i = 0; i < 8; i++, n++) {
		snprintf(urls[n], 256, "https://archive.org/services/img/%s", ids[i]);
		names[n] = "thumb";
		reqs[n] = http_get(urls[n], NULL);
	}
	struct http_opts o;
	http_opts_default(&o);
	o.range_start = 1000;
	o.range_end = 1000 + 65535;
	snprintf(urls[n], 256, "https://archive.org/download/122Eyes1950/122Eyes1950_512kb.mp4");
	names[n] = "range";
	reqs[n] = http_get(urls[n], &o);
	n++;
	snprintf(urls[n], 256, "https://no-such-host.invalid/");
	names[n] = "bad-host";
	reqs[n] = http_get(urls[n], NULL);
	n++;

	uint64_t t0 = plat_ms();
	int left = n;
	int done[N] = {0};
	while (left > 0 && plat_ms() - t0 < 120000) {
		for (int i = 0; i < n; i++) {
			if (done[i] || http_state(reqs[i]) == HTTP_PENDING)
				continue;
			done[i] = 1;
			left--;
			const struct http_result *r = http_result(reqs[i]);
			printf("%-9s %-5s status %3d %7zu bytes total %lld in %5u ms  %s%s\n", names[i],
				http_state(reqs[i]) == HTTP_DONE ? "OK" : "FAIL", r->status, r->body_len,
				(long long)r->total_size, r->elapsed_ms,
				http_state(reqs[i]) == HTTP_DONE ? r->content_type : http_strerror(r->error),
				strcmp(r->final_url, urls[i]) && http_state(reqs[i]) == HTTP_DONE ? "  (redirected)" : "");
		}
		usleep(20000);
	}
	struct http_stats st;
	http_get_stats(&st);
	printf("completed %u failed %u bytes %u, connections open %u, %d still pending\n", st.completed,
		st.failed, st.bytes_in, st.connections, left);
	for (int i = 0; i < n; i++)
		http_release(reqs[i]);
	http_shutdown();
	return left ? 1 : 0;
}
