/* Phase 0 / Spike 1b: where does Wii download throughput go?
 *
 * Spike 1 on hardware: TLS CPU is cheap with ChaCha20 (~21 MB/s), but bulk downloads ran at
 * only 100-180 KB/s, and one stalled for 60 s. archive.org's per-connection speed also varies
 * a lot (280 KB/s to 5 MB/s from a PC), so this spike separates the causes:
 *   A. one connection, 8 MB                  -> baseline
 *   B. four parallel Range connections, 8 MB -> does the aggregate scale? (per-connection limit)
 *   C. one connection, SO_RCVBUF = 64 KB     -> is the IOS TCP window the limit? (libogc sets 32 KB)
 * Run in the order A B A B C to average out server variance. Every transfer resumes with an
 * HTTP Range request after a 15 s stall, which is also the behaviour the real http.c will need.
 * Results: screen + sd:/apps/iachannel/spike_net.log. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

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

#define STALL_TIMEOUT_MS 15000
#define MAX_RESUMES 6
/* Hard cap per test: in Dolphin, SO_RCVBUF 64 KB made recv trickle 256 bytes per call. */
#define TEST_DEADLINE_MS 90000
#define TOTAL_BYTES (8u * 1024u * 1024u)
#define PARALLEL 4
#define USER_AGENT "IAChannel-spike/0.2 (Nintendo Wii)"
#define START_URL "https://archive.org/download/122Eyes1950/122Eyes1950_512kb.mp4"

struct target {
	char host[128];
	char path[1024];
};

struct range_job {
	const struct target *t;
	size_t start, len;
	size_t got;
	int resumes;
	int failed;
	uint64_t wait_us;
	uint64_t deadline_ms;
};

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

static int split_url(const char *url, struct target *t)
{
	if (strncmp(url, "https://", 8) != 0)
		return -1;
	const char *h = url + 8, *slash = strchr(h, '/');
	size_t len = slash ? (size_t)(slash - h) : strlen(h);
	if (len == 0 || len >= sizeof t->host)
		return -1;
	memcpy(t->host, h, len);
	t->host[len] = '\0';
	snprintf(t->path, sizeof t->path, "%s", slash ? slash : "/");
	return 0;
}

/* Sends a GET with a Range and reads the header block. On success, returns the connection
 * with any body bytes that arrived along with the headers in buf[0..*body_have). */
static tls_conn *open_range(const struct target *t, size_t from, size_t to, int *status,
	char *location, size_t loc_n, char *buf, size_t buf_n, size_t *body_have)
{
	int err;
	tls_conn *c = tls_connect(t->host, 443, STALL_TIMEOUT_MS, &err);
	if (!c) {
		char msg[128];
		tls_strerror(err, msg, sizeof msg);
		log_printf("  connect %s: %s\n", t->host, msg);
		return NULL;
	}
	char req[1400];
	int n = snprintf(req, sizeof req,
		"GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\nRange: bytes=%u-%u\r\n"
		"Connection: close\r\n\r\n", t->path, t->host, (unsigned)from, (unsigned)to);
	if (tls_write_all(c, req, (size_t)n) < 0)
		goto fail;
	size_t have = 0;
	char *end = NULL;
	while (!end && have < buf_n - 1) {
		int got = tls_read(c, buf + have, buf_n - 1 - have);
		if (got <= 0)
			goto fail;
		have += (size_t)got;
		buf[have] = '\0';
		end = strstr(buf, "\r\n\r\n");
	}
	if (!end)
		goto fail;
	*end = '\0';
	*status = 0;
	sscanf(buf, "HTTP/%*s %d", status);
	char date[64];
	header_value(buf, "Date", date, sizeof date);
	if (date[0])
		tls_note_server_date(date);
	if (location)
		header_value(buf, "Location", location, loc_n);
	size_t hdr_len = (size_t)(end + 4 - buf);
	*body_have = have - hdr_len;
	memmove(buf, end + 4, *body_have);
	return c;
fail:
	tls_close(c);
	return NULL;
}

/* Follows the archive.org redirect once so every test hits the same storage node. */
static int resolve_target(struct target *t)
{
	static char buf[16384];
	char loc[1024];
	struct target cur;
	if (split_url(START_URL, &cur) != 0)
		return -1;
	for (int hop = 0; hop < 8; hop++) {
		int status;
		size_t body;
		tls_conn *c = open_range(&cur, 0, 0, &status, loc, sizeof loc, buf, sizeof buf, &body);
		if (!c)
			return -1;
		tls_close(c);
		if (status == 206 || status == 200) {
			*t = cur;
			log_printf("target: https://%s%s\n", t->host, t->path);
			return 0;
		}
		if (status < 300 || status >= 400 || !loc[0] || split_url(loc, &cur) != 0)
			return -1;
	}
	return -1;
}

static void *range_worker(void *arg)
{
	struct range_job *j = arg;
	char *buf = malloc(16384);
	if (!buf) {
		j->failed = 1;
		return NULL;
	}
	while (j->got < j->len) {
		int status;
		size_t body;
		size_t from = j->start + j->got, to = j->start + j->len - 1;
		tls_conn *c = open_range(j->t, from, to, &status, NULL, 0, buf, 16384, &body);
		if (c && status == 206) {
			j->got += body;
			struct tls_stats before, after;
			tls_get_stats(c, &before);
			while (j->got < j->len) {
				if (plat_ms() > j->deadline_ms) {
					log_printf("  [%u+%u] deadline hit at %u bytes\n", (unsigned)j->start,
						(unsigned)j->len, (unsigned)j->got);
					break;
				}
				int n = tls_read(c, buf, 16384);
				if (n <= 0) {
					if (n < 0) {
						char msg[128];
						tls_strerror(n, msg, sizeof msg);
						log_printf("  [%u+%u] stalled/failed at %u: %s\n", (unsigned)j->start,
							(unsigned)j->len, (unsigned)j->got, msg);
					}
					break;
				}
				size_t before_mb = j->got >> 20;
				j->got += (size_t)n;
				if ((j->got >> 20) != before_mb)
					log_printf("  [%u+%u] %u MB\n", (unsigned)j->start, (unsigned)j->len,
						(unsigned)(j->got >> 20));
			}
			tls_get_stats(c, &after);
			j->wait_us += after.io_wait_us - before.io_wait_us;
		} else if (c) {
			log_printf("  unexpected status %d for range\n", status);
		}
		if (c)
			tls_close(c);
		if (j->got >= j->len)
			break;
		if (plat_ms() > j->deadline_ms) {
			j->failed = 1;
			break;
		}
		if (++j->resumes > MAX_RESUMES) {
			j->failed = 1;
			break;
		}
		log_printf("  resume #%d from byte %u\n", j->resumes, (unsigned)(j->start + j->got));
	}
	free(buf);
	return NULL;
}

/* Downloads TOTAL_BYTES split across `conns` parallel connections. Returns KB/s. */
static double run_test(const char *label, const struct target *t, int conns, int rcvbuf)
{
	struct range_job jobs[PARALLEL];
	iac_thread th[PARALLEL];
	size_t share = TOTAL_BYTES / (size_t)conns;

	sock_set_rcvbuf(rcvbuf);
	log_printf("\n[%s] %d connection(s)%s\n", label, conns, rcvbuf ? ", SO_RCVBUF 64 KB" : "");
	memset(jobs, 0, sizeof jobs);
	uint64_t t0 = plat_ms();
	for (int i = 0; i < conns; i++) {
		jobs[i].t = t;
		jobs[i].deadline_ms = t0 + TEST_DEADLINE_MS;
		jobs[i].start = share * (size_t)i;
		jobs[i].len = i == conns - 1 ? TOTAL_BYTES - jobs[i].start : share;
		if (conns == 1) {
			range_worker(&jobs[i]);
		} else if (plat_thread_create(&th[i], range_worker, &jobs[i], 64 * 1024, 50) != 0) {
			log_printf("  thread create failed\n");
			jobs[i].failed = 1;
			conns = i;
			break;
		}
	}
	if (conns > 1)
		for (int i = 0; i < conns; i++)
			plat_thread_join(th[i]);
	uint64_t ms = plat_ms() - t0;
	sock_set_rcvbuf(0);

	size_t got = 0;
	int resumes = 0, failed = 0;
	for (int i = 0; i < conns; i++) {
		got += jobs[i].got;
		resumes += jobs[i].resumes;
		failed |= jobs[i].failed;
		if (conns > 1)
			log_printf("  conn %d: %u bytes, wait %llu ms\n", i, (unsigned)jobs[i].got,
				(unsigned long long)(jobs[i].wait_us / 1000));
	}
	double kbps = ms ? (double)got / 1024.0 / ((double)ms / 1000.0) : 0;
	log_printf("  %u bytes in %llu ms = %.0f KB/s, %d resume(s)%s\n", (unsigned)got,
		(unsigned long long)ms, kbps, resumes, failed ? ", INCOMPLETE" : "");
	return kbps;
}

static void run_spike(void)
{
	if (sock_init() != 0) {
		log_printf("FAIL: network init\n");
		return;
	}
	int rc = tls_global_init(CA_OVERRIDE_PATH, SEED_PATH, TLS_FLAGS_DEFAULT);
	if (rc != 0) {
		char msg[128];
		tls_strerror(rc, msg, sizeof msg);
		log_printf("FAIL: tls init: %s\n", msg);
		return;
	}
	/* The first request corrects the clock from its Date: header. If the console clock is far
	 * off, that first verify can fail; retry once after the correction. */
	struct target t;
	if (resolve_target(&t) != 0 && resolve_target(&t) != 0) {
		log_printf("FAIL: could not resolve download target\n");
		return;
	}

	double a1 = run_test("A1 single", &t, 1, 0);
	double b1 = run_test("B1 parallel", &t, PARALLEL, 0);
	double a2 = run_test("A2 single", &t, 1, 0);
	double b2 = run_test("B2 parallel", &t, PARALLEL, 0);
	/* Last: libogc already defaults to 32 KB, and 64 KB misbehaved in Dolphin. */
	double c1 = run_test("C1 single+rcvbuf", &t, 1, 64 * 1024);

	double a = (a1 + a2) / 2, b = (b1 + b2) / 2;
	log_printf("\n=== Spike 1b results ===\n");
	log_printf("single     %.0f KB/s (runs %.0f, %.0f)\n", a, a1, a2);
	log_printf("parallel x%d %.0f KB/s (runs %.0f, %.0f) -> %.1fx single\n", PARALLEL, b, b1, b2,
		a > 0 ? b / a : 0);
	log_printf("rcvbuf 64K %.0f KB/s -> %.1fx single\n", c1, a > 0 ? c1 / a : 0);
	if (a > 0 && b / a > 1.7)
		log_printf("verdict: per-connection limited -> stream media over parallel Range requests\n");
	else if (a > 0 && b / a > 1.25)
		log_printf("verdict: partly per-connection limited -> 2-4 parallel ranges help buffer fill\n");
	else
		log_printf("verdict: link limited (Wii radio/IOS or ISP) -> parallelism won't help\n");
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
	log_init(have_sd ? APP_DIR "/spike_net.log" : NULL);
	log_printf("IAChannel spike 1b: download throughput\n\n");

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
	log_printf("IAChannel spike 1b: download throughput (host build)\n\n");
	run_spike();
	return 0;
}
#endif
