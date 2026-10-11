/* Phase 0 / Spike 1: can the Wii talk modern TLS to archive.org fast enough?
 *
 * Pass bars (from the plan): verified chain, full handshake < 1 s, >= 1 MB/s bulk.
 * Runs on the Wii (HBC / Dolphin) and on the host for comparison. Results go to the
 * screen/stdout and, on the Wii, to sd:/apps/iachannel/spike_tls.log. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <mbedtls/chachapoly.h>
#include <mbedtls/gcm.h>

#include "core/log.h"
#include "core/platform.h"
#include "net/sock.h"
#include "net/tls.h"

#ifdef HW_RVL
#include <fat.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#define APP_DIR "sd:/apps/iachannel"
#define CA_OVERRIDE_PATH APP_DIR "/cacert.pem"
#define SEED_PATH APP_DIR "/seed.bin"
#else
#define CA_OVERRIDE_PATH NULL
#define SEED_PATH NULL
#endif

#define TIMEOUT_MS 60000
#define BULK_LIMIT (8u * 1024u * 1024u)
#define USER_AGENT "IAChannel-spike/0.1 (Nintendo Wii)"

struct fetch_result {
	int status;
	char location[512];
	char date[64];
	uint64_t handshake_ms;
	uint64_t ttfb_ms; /* request sent -> first header bytes */
	uint64_t body_ms;
	uint64_t body_wait_ms; /* part of body_ms spent blocked on the socket */
	size_t body_bytes;
	struct tls_stats stats;
	char desc[96];
};

static int parse_https_url(const char *url, char *host, size_t hn, char *path, size_t pn)
{
	if (strncmp(url, "https://", 8) != 0)
		return -1;
	const char *h = url + 8;
	const char *slash = strchr(h, '/');
	size_t len = slash ? (size_t)(slash - h) : strlen(h);
	if (len == 0 || len >= hn)
		return -1;
	memcpy(host, h, len);
	host[len] = '\0';
	snprintf(path, pn, "%s", slash ? slash : "/");
	return 0;
}

/* Copies the value of header `name` from a raw header block. */
static void header_value(const char *headers, const char *name, char *out, size_t n)
{
	size_t nl = strlen(name);
	out[0] = '\0';
	for (const char *p = headers; p && *p; p = strstr(p, "\r\n")) {
		if (p[0] == '\r')
			p += 2;
		if (strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
			const char *v = p + nl + 1;
			while (*v == ' ')
				v++;
			const char *e = strstr(v, "\r\n");
			size_t len = e ? (size_t)(e - v) : strlen(v);
			if (len >= n)
				len = n - 1;
			memcpy(out, v, len);
			out[len] = '\0';
			return;
		}
	}
}

static int fetch_once(const char *url, size_t body_limit, struct fetch_result *r)
{
	char host[128], path[1024], req[1400];
	static char buf[16384 + 1];
	int err = 0;

	memset(r, 0, sizeof *r);
	if (parse_https_url(url, host, sizeof host, path, sizeof path) != 0)
		return -1;

	uint64_t t0 = plat_ms();
	tls_conn *c = tls_connect(host, 443, TIMEOUT_MS, &err);
	if (!c) {
		char msg[160];
		tls_strerror(err, msg, sizeof msg);
		log_printf("  connect %s failed: %s\n", host, msg);
		return err ? err : -1;
	}
	r->handshake_ms = plat_ms() - t0;
	tls_describe(c, r->desc, sizeof r->desc);

	int n = snprintf(req, sizeof req,
		"GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\n"
		"Accept: */*\r\nConnection: close\r\n\r\n", path, host);
	uint64_t t1 = plat_ms();
	if (tls_write_all(c, req, (size_t)n) < 0) {
		tls_close(c);
		return -1;
	}

	/* Read until the end of the header block. */
	size_t have = 0;
	char *hdr_end = NULL;
	while (!hdr_end && have < sizeof buf - 1) {
		int got = tls_read(c, buf + have, sizeof buf - 1 - have);
		if (got <= 0)
			break;
		if (have == 0)
			r->ttfb_ms = plat_ms() - t1;
		have += (size_t)got;
		buf[have] = '\0';
		hdr_end = strstr(buf, "\r\n\r\n");
	}
	if (!hdr_end) {
		tls_close(c);
		return -1;
	}
	*hdr_end = '\0';
	sscanf(buf, "HTTP/%*s %d", &r->status);
	header_value(buf, "Location", r->location, sizeof r->location);
	header_value(buf, "Date", r->date, sizeof r->date);

	size_t body_have = have - (size_t)(hdr_end + 4 - buf);
	r->body_bytes = body_have;
	struct tls_stats before;
	tls_get_stats(c, &before);
	uint64_t t2 = plat_ms();
	while (r->body_bytes < body_limit) {
		int got = tls_read(c, buf, sizeof buf - 1);
		if (got < 0) {
			char msg[160];
			tls_strerror(got, msg, sizeof msg);
			log_printf("  read error after %u bytes: %s\n", (unsigned)r->body_bytes, msg);
			break;
		}
		if (got == 0)
			break;
		r->body_bytes += (size_t)got;
	}
	r->body_ms = plat_ms() - t2;
	tls_get_stats(c, &r->stats);
	r->body_wait_ms = (r->stats.io_wait_us - before.io_wait_us) / 1000;
	tls_close(c);
	return 0;
}

/* Follows up to 8 redirects. */
static int fetch(const char *url, size_t body_limit, struct fetch_result *r)
{
	char cur[1024];
	snprintf(cur, sizeof cur, "%s", url);
	for (int hop = 0; hop < 8; hop++) {
		int rc = fetch_once(cur, body_limit, r);
		if (rc != 0)
			return rc;
		log_printf("  %s -> %d\n    %s connect=%ums handshake=%ums%s ttfb=%llums\n", cur,
			r->status, r->desc, (unsigned)r->stats.connect_ms, (unsigned)r->stats.handshake_ms,
			r->stats.offered_session ? " (session offered)" : "", (unsigned long long)r->ttfb_ms);
		if (r->date[0])
			tls_note_server_date(r->date);
		if (r->status < 300 || r->status >= 400 || !r->location[0])
			return 0;
		snprintf(cur, sizeof cur, "%s", r->location);
	}
	return -1;
}

/* Plain-HTTP HEAD to learn the real time before any certificate is checked. */
static void sync_clock(void)
{
	int fd = sock_connect("archive.org", 80, TIMEOUT_MS);
	if (fd < 0) {
		log_printf("clock: connect failed: %s\n", sock_strerror(fd));
		return;
	}
	static const char req[] = "HEAD / HTTP/1.1\r\nHost: archive.org\r\nUser-Agent: " USER_AGENT
		"\r\nConnection: close\r\n\r\n";
	char buf[2048];
	size_t have = 0;
	if (sock_send_all(fd, req, sizeof req - 1, TIMEOUT_MS) > 0) {
		int n;
		while (have < sizeof buf - 1 &&
			(n = sock_recv(fd, buf + have, sizeof buf - 1 - have, TIMEOUT_MS)) > 0)
			have += (size_t)n;
	}
	sock_close(fd);
	buf[have] = '\0';
	char date[64];
	header_value(buf, "Date", date, sizeof date);
	if (date[0] && tls_note_server_date(date) == 0)
		log_printf("clock: server date '%s', offset %lld s\n", date, (long long)tls_clock_offset());
	else
		log_printf("clock: no usable Date header\n");
}

/* Offline AEAD throughput: the ceiling on TLS bulk speed regardless of the network. */
static double bench_aead(const char *name, int kind)
{
	enum { CHUNK = 16384, TOTAL = 4 << 20 };
	static unsigned char in[CHUNK], out[CHUNK];
	unsigned char key[32] = {1}, iv[12] = {2}, tag[16];
	mbedtls_gcm_context gcm;
	mbedtls_chachapoly_context cp;

	if (kind == 2) {
		mbedtls_chachapoly_init(&cp);
		mbedtls_chachapoly_setkey(&cp, key);
	} else {
		mbedtls_gcm_init(&gcm);
		mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, kind == 0 ? 128 : 256);
	}
	uint64_t t = plat_us();
	for (int done = 0; done < TOTAL; done += CHUNK) {
		iv[0]++;
		if (kind == 2)
			mbedtls_chachapoly_encrypt_and_tag(&cp, CHUNK, iv, NULL, 0, in, out, tag);
		else
			mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, CHUNK, iv, sizeof iv, NULL, 0,
				in, out, sizeof tag, tag);
	}
	uint64_t us = plat_us() - t;
	if (kind == 2)
		mbedtls_chachapoly_free(&cp);
	else
		mbedtls_gcm_free(&gcm);
	double kbs = (double)TOTAL / 1024.0 / ((double)us / 1e6);
	log_printf("  %-22s %8.0f KB/s\n", name, kbs);
	return kbs;
}

static double bulk_run(const char *label)
{
	static const char *url = "https://archive.org/download/122Eyes1950/122Eyes1950_512kb.mp4";
	struct fetch_result r;
	log_printf("\n[bulk %s] %u MB cap, redirect to storage node\n", label, BULK_LIMIT >> 20);
	if (fetch(url, BULK_LIMIT, &r) != 0 || r.status != 200 || r.body_ms == 0) {
		log_printf("  bulk download failed\n");
		return 0;
	}
	double kbps = (double)r.body_bytes / 1024.0 / ((double)r.body_ms / 1000.0);
	uint64_t cpu_ms = r.body_ms > r.body_wait_ms ? r.body_ms - r.body_wait_ms : 0;
	log_printf("  %u bytes in %llu ms = %.0f KB/s; CPU (TLS+copy) %llu ms, socket wait %llu ms\n",
		(unsigned)r.body_bytes, (unsigned long long)r.body_ms, kbps,
		(unsigned long long)cpu_ms, (unsigned long long)r.body_wait_ms);
	if (cpu_ms > 0)
		log_printf("  CPU-bound ceiling ~%.0f KB/s\n",
			(double)r.body_bytes / 1024.0 / ((double)cpu_ms / 1000.0));
	return kbps;
}

static void run_spike(void)
{
	struct fetch_result r;
	int pass = 1;

	if (sock_init() != 0) {
		log_printf("FAIL: network init\n");
		return;
	}
	sync_clock();

	uint64_t t = plat_ms();
	int rc = tls_global_init(CA_OVERRIDE_PATH, SEED_PATH, TLS_FLAGS_DEFAULT);
	if (rc != 0) {
		char msg[160];
		tls_strerror(rc, msg, sizeof msg);
		log_printf("FAIL: tls_global_init: %s\n", msg);
		return;
	}
	log_printf("tls init (DRBG seed + CA parse): %llu ms\n", (unsigned long long)(plat_ms() - t));

	log_printf("\n[1] full handshake: metadata API\n");
	if (fetch("https://archive.org/metadata/nasa/metadata", 1u << 20, &r) != 0 || r.status != 200) {
		log_printf("FAIL: metadata request\n");
		return;
	}
	uint64_t full_hs = r.handshake_ms;
	log_printf("  body %u bytes\n", (unsigned)r.body_bytes);

	log_printf("\n[2] second connection (session resumption if offered)\n");
	if (fetch("https://archive.org/metadata/nasa/metadata", 1u << 20, &r) == 0)
		log_printf("  handshake %llu ms vs %llu ms full\n", (unsigned long long)r.handshake_ms,
			(unsigned long long)full_hs);

	log_printf("\n[3] AEAD benchmark (4 MB each)\n");
	double aes128 = bench_aead("AES-128-GCM", 0);
	double aes256 = bench_aead("AES-256-GCM", 1);
	double chacha = bench_aead("ChaCha20-Poly1305", 2);
	double slowest = aes256 < aes128 ? aes256 : aes128;
	if (chacha < slowest)
		slowest = chacha;

	tls_set_flags(0);
	double kbps = bulk_run("server's choice");
	tls_set_flags(TLS_FLAGS_DEFAULT);
	double kbps2 = bulk_run("no AES-256, ChaCha first");
	if (kbps2 > kbps)
		kbps = kbps2;

	log_printf("\n=== Spike 1 results ===\n");
	log_printf("full handshake %llu ms (bar < 1000): %s\n", (unsigned long long)full_hs,
		full_hs < 1000 ? "PASS" : "FAIL");
	if (full_hs >= 1000)
		pass = 0;
	log_printf("bulk %.0f KB/s (bar >= 1024): %s\n", kbps, kbps >= 1024 ? "PASS" : "FAIL");
	log_printf("slowest AEAD %.0f KB/s (crypto must not be the limit, bar >= 2048): %s\n",
		slowest, slowest >= 2048 ? "PASS" : "FAIL");
	if (kbps < 1024 || slowest < 2048)
		pass = 0;
	log_printf("overall: %s\n", pass ? "PASS" : "FAIL");
	tls_global_shutdown();
}

#ifdef HW_RVL
static void *g_xfb;

static void video_console_init(void)
{
	VIDEO_Init();
	GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);
	g_xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
	console_init(g_xfb, 20, 20, mode->fbWidth, mode->xfbHeight, mode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(mode);
	VIDEO_SetNextFramebuffer(g_xfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (mode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
}

int main(void)
{
	video_console_init();
	WPAD_Init();
	int have_sd = fatInitDefault();
	if (have_sd) {
		mkdir("sd:/apps", 0777);
		mkdir(APP_DIR, 0777);
	}
	log_init(have_sd ? APP_DIR "/spike_tls.log" : NULL);
	log_printf("IAChannel spike 1: TLS to archive.org\n\n");

	run_spike();

	log_printf("\nPress HOME to exit.\n");
	log_close();
	for (;;) {
		WPAD_ScanPads();
		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
			break;
		VIDEO_WaitVSync();
	}
	return 0;
}
#else
int main(void)
{
	log_init(NULL);
	log_printf("IAChannel spike 1: TLS to archive.org (host build)\n\n");
	run_spike();
	return 0;
}
#endif
