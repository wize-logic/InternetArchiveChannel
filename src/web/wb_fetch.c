/* NetSurf fetcher for http(s): every URL a page asks for (page, images, stylesheets, links) is
 * fetched from the Wayback Machine at the current snapshot time, as raw archived bytes
 * (web.archive.org/web/<ts>id_/<url>), through the app's HTTP client. NetSurf keeps seeing the
 * original URLs, so relative links resolve against the original site and stay in the past. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libwapcaplet/libwapcaplet.h>

#include "content/fetch.h"
#include "content/fetchers.h"
#include "utils/nsurl.h"

#include "net/http.h"
#include "wl/platform/platform.h"
#include "web/webview.h"

struct wbf {
	struct fetch *parent;
	nsurl *url;
	http_req *req;
	char *query; /* a form's POST body, replayed as a GET query (the archive only has GETs) */
	int aborted, locked, started;
	struct wbf *next;
};

static struct wbf *g_active;
/* web.archive.org answers 429 when a client opens many requests at once, so a page's images
 * and stylesheets are fetched a few at a time. */
#define MAX_RUNNING 3
static char g_ts[16] = "1999";

void wb_fetch_set_timestamp(const char *ts)
{
	snprintf(g_ts, sizeof g_ts, "%s", ts && *ts ? ts : "1999");
}

static bool wbf_initialise(lwc_string *scheme)
{
	(void)scheme;
	return true;
}

static void wbf_finalise(lwc_string *scheme)
{
	(void)scheme;
}

static bool wbf_acceptable(const struct nsurl *url)
{
	(void)url;
	return true;
}

static void *wbf_setup(struct fetch *parent, struct nsurl *url, bool only_2xx, bool downgrade_tls,
	const char *post_urlenc, const struct fetch_multipart_data *post_multipart, const char **headers)
{
	(void)only_2xx;
	(void)downgrade_tls;
	(void)post_multipart;
	(void)headers;
	struct wbf *f = calloc(1, sizeof *f);
	if (!f)
		return NULL;
	f->parent = parent;
	f->url = nsurl_ref(url);
	if (post_urlenc && *post_urlenc)
		f->query = strdup(post_urlenc);
	return f;
}

/* The Wayback URL for an original URL (or the URL itself if it already is one). */
void wb_fetch_rewrite(const char *orig, char *out, size_t cap)
{
	if (strncasecmp(orig, "https://web.archive.org/", 24) == 0 || strncasecmp(orig, "http://web.archive.org/", 23) == 0)
		snprintf(out, cap, "%s", orig);
	else
		snprintf(out, cap, "https://web.archive.org/web/%sid_/%s", g_ts, orig);
}

static void begin(struct wbf *f)
{
	char orig[2048], url[2300];
	const char *u = nsurl_access(f->url);
	if (f->query)
		snprintf(orig, sizeof orig, "%s%c%s", u, strchr(u, '?') ? '&' : '?', f->query);
	else
		snprintf(orig, sizeof orig, "%s", u);
	wb_fetch_rewrite(orig, url, sizeof url);
	if (strchr(orig, '?'))
		WL_LOG_I("WEB", "fetch %s", url); /* queries: form submissions, mostly */
	struct http_opts o;
	http_opts_default(&o);
	o.max_body = 6u << 20;
	o.timeout_ms = 60000;
	o.priority = 5;
	f->req = http_get(url, &o);
	f->started = 1;
}

/* Queued here; begin() runs from the poll once a slot is free. NetSurf asks for the page first,
 * so it goes out before its images. */
static bool wbf_start(void *ctx)
{
	struct wbf *f = ctx;
	struct wbf **tail = &g_active;
	while (*tail)
		tail = &(*tail)->next;
	f->next = NULL;
	*tail = f;
	return true;
}

static void wbf_abort(void *ctx)
{
	struct wbf *f = ctx;
	f->aborted = 1;
	if (f->req) {
		http_release(f->req);
		f->req = NULL;
	}
}

static void wbf_free(void *ctx)
{
	struct wbf *f = ctx;
	for (struct wbf **p = &g_active; *p; p = &(*p)->next)
		if (*p == f) {
			*p = f->next;
			break;
		}
	if (f->req)
		http_release(f->req);
	nsurl_unref(f->url);
	free(f->query);
	free(f);
}

static void deliver(struct wbf *f, fetch_msg *msg)
{
	f->locked = 1;
	fetch_send_callback(msg, f->parent);
	f->locked = 0;
}

static void send_header(struct wbf *f, const char *line)
{
	fetch_msg msg = {.type = FETCH_HEADER};
	msg.data.header_or_data.buf = (const uint8_t *)line;
	msg.data.header_or_data.len = strlen(line);
	deliver(f, &msg);
}

/* Delivers finished requests. Callbacks may start or abort other fetches, so the list is
 * re-scanned from the top after each delivery. */
static void wbf_poll(lwc_string *scheme)
{
	(void)scheme;
	int running = 0;
	for (struct wbf *f = g_active; f; f = f->next)
		running += f->started && f->req && !f->aborted;
	for (struct wbf *f = g_active; f && running < MAX_RUNNING; f = f->next)
		if (!f->started && !f->aborted) {
			begin(f);
			running++;
		}
	int again = 1;
	while (again) {
		again = 0;
		for (struct wbf *f = g_active; f; f = f->next) {
			if (f->locked || f->aborted)
				continue;
			if (!f->req || http_state(f->req) == HTTP_PENDING)
				continue;
			const struct http_result *r = http_result(f->req);
			fetch_msg msg;
			if (http_state(f->req) == HTTP_FAILED) {
				WL_LOG_W("WEB", "failed (%s): %s", http_strerror(r->error), nsurl_access(f->url));
				msg.type = FETCH_ERROR;
				msg.data.error = http_strerror(r->error);
				deliver(f, &msg);
			} else {
				char line[160];
				if (r->status >= 400)
					WL_LOG_W("WEB", "HTTP %d: %s", r->status, nsurl_access(f->url));
				fetch_set_http_code(f->parent, r->status);
				if (r->content_type[0] && !f->aborted) {
					snprintf(line, sizeof line, "Content-Type: %s", r->content_type);
					send_header(f, line);
				}
				if (!f->aborted) {
					snprintf(line, sizeof line, "Content-Length: %u", (unsigned)r->body_len);
					send_header(f, line);
				}
				if (!f->aborted && r->body_len) {
					msg.type = FETCH_DATA;
					msg.data.header_or_data.buf = r->body;
					msg.data.header_or_data.len = r->body_len;
					deliver(f, &msg);
				}
				if (!f->aborted) {
					msg.type = FETCH_FINISHED;
					deliver(f, &msg);
				}
				webview_note_bytes(r->body_len);
			}
			if (f->req) {
				http_release(f->req);
				f->req = NULL;
			}
			fetch_remove_from_queues(f->parent);
			fetch_free(f->parent); /* calls wbf_free, which unlinks f */
			again = 1;
			break;
		}
	}
	/* Aborted fetches are finished off here too. */
	for (struct wbf *f = g_active, *next; f; f = next) {
		next = f->next;
		if (f->aborted && !f->locked) {
			fetch_remove_from_queues(f->parent);
			fetch_free(f->parent);
			next = g_active;
		}
	}
}

int wb_fetch_register(void)
{
	static const struct fetcher_operation_table ops = {
		.initialise = wbf_initialise,
		.acceptable = wbf_acceptable,
		.setup = wbf_setup,
		.start = wbf_start,
		.abort = wbf_abort,
		.free = wbf_free,
		.poll = wbf_poll,
		.finalise = wbf_finalise,
	};
	const char *schemes[] = {"http", "https"};
	for (int i = 0; i < 2; i++) {
		lwc_string *s;
		if (lwc_intern_string(schemes[i], strlen(schemes[i]), &s) != lwc_error_ok)
			return -1;
		if (fetcher_add(s, &ops) != NSERROR_OK)
			return -1;
	}
	return 0;
}

int wb_fetch_active(void)
{
	int n = 0;
	for (struct wbf *f = g_active; f; f = f->next)
		n++;
	return n;
}
