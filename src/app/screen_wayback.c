/* Wayback Machine: pick a site and a year (or a famous old site, or a bookmark), then a month and
 * a day from the site's captures that year (CDX API), then browse it (screen_web). If the CDX
 * lookup fails, the closest snapshot to July 1st is opened instead. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/app.h"
#include "ia/ia.h"
#include "net/http.h"
#include "ui/osk.h"
#include "web/webview.h"

enum { ID_URL = 1, ID_GO, ID_BOOKMARKS, ID_YEAR = 100, ID_SUGGEST = 200, ID_MONTH = 300, ID_DAY = 400,
	ID_MARK = 500 };

#define FIRST_YEAR 1996
#define YEARS 25
#define MAX_CAPTURES 400

static const struct {
	const char *label, *url, *year;
} k_suggest[] = {
	{"Yahoo! 1999", "www.yahoo.com", "1999"},
	{"Space Jam 1996", "www.spacejam.com", "1996"},
	{"Apple 1998", "www.apple.com", "1998"},
	{"Google 1999", "www.google.com", "1999"},
	{"CNN 2001", "www.cnn.com", "2001"},
	{"Nintendo 2006", "www.nintendo.com", "2006"},
};

static const char *const k_months[12] = {"January", "February", "March", "April", "May", "June", "July",
	"August", "September", "October", "November", "December"};

enum mode { PICK, MONTHS, DAYS, BOOKMARKS };

static struct {
	enum mode mode;
	char url[256];
	int year, month; /* month 0..11 in DAYS */
	int editing;
	http_req *req;
	char message[160];
	char (*caps)[IA_TS_LEN];
	int ncaps;
} g;

static void enter(void *arg)
{
	(void)arg;
	if (!g.url[0]) {
		snprintf(g.url, sizeof g.url, "www.yahoo.com");
		g.year = 1999;
	}
	if (!g.caps)
		g.caps = malloc(sizeof *g.caps * MAX_CAPTURES);
	g.message[0] = '\0';
}

static void open_page(const char *ts)
{
	static struct web_args args;
	snprintf(args.url, sizeof args.url, "%s%s", strstr(g.url, "://") ? "" : "http://", g.url);
	snprintf(args.timestamp, sizeof args.timestamp, "%s", ts);
	WL_LOG_I("WAYBACK", "%s -> snapshot %s", g.url, ts);
	app_push(&screen_web, &args);
}

static void lookup(void)
{
	if (strncmp(g.url, "file:", 5) == 0) { /* local test pages (sd:/...) aren't in the archive */
		char ts[16];
		snprintf(ts, sizeof ts, "%d0701", g.year);
		open_page(ts);
		return;
	}
	if (g.req)
		http_release(g.req);
	char url[1024];
	ia_wayback_cdx_url(url, sizeof url, g.url, g.year);
	struct http_opts o;
	http_opts_default(&o);
	o.priority = 10;
	o.timeout_ms = 30000;
	g.req = http_get(url, &o);
	snprintf(g.message, sizeof g.message, "Finding captures of %s in %d...", g.url, g.year);
	WL_LOG_I("WAYBACK", "lookup %s", url);
}

static void poll_lookup(void)
{
	if (!g.req || http_state(g.req) == HTTP_PENDING)
		return;
	const struct http_result *r = http_result(g.req);
	int n = -1;
	if (http_state(g.req) == HTTP_DONE && r->status == 200 && g.caps)
		n = ia_parse_cdx((const char *)r->body, g.caps, MAX_CAPTURES);
	WL_LOG_I("WAYBACK", "CDX: state %d, status %d, %d captures", (int)http_state(g.req), r->status, n);
	http_release(g.req);
	g.req = NULL;
	g.message[0] = '\0';
	if (n > 0) {
		g.ncaps = n;
		g.mode = MONTHS;
		ui_focus(0);
	} else if (n == 0) {
		snprintf(g.message, sizeof g.message, "No captures of %s in %d. Try another year.", g.url, g.year);
	} else {
		/* CDX unavailable (rate limits, timeouts): the Wayback Machine redirects /web/<date>id_/ to
		 * the nearest capture, so go by date. */
		char ts[16];
		snprintf(ts, sizeof ts, "%d0701", g.year);
		open_page(ts);
	}
}

static int captures_in(int month, int day)
{
	int n = 0;
	for (int i = 0; i < g.ncaps; i++) {
		int m = (g.caps[i][4] - '0') * 10 + (g.caps[i][5] - '0');
		int d = (g.caps[i][6] - '0') * 10 + (g.caps[i][7] - '0');
		if (m == month + 1 && (day == 0 || d == day))
			n++;
	}
	return n;
}

static const char *capture_on(int month, int day)
{
	for (int i = 0; i < g.ncaps; i++) {
		int m = (g.caps[i][4] - '0') * 10 + (g.caps[i][5] - '0');
		int d = (g.caps[i][6] - '0') * 10 + (g.caps[i][7] - '0');
		if (m == month + 1 && d == day)
			return g.caps[i];
	}
	return NULL;
}

static int weekday(int y, int m, int d) /* 0 = Sunday; m is 1..12 */
{
	static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
	if (m < 3)
		y--;
	return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static int days_in(int y, int m)
{
	static const int dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	return m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : dm[m - 1];
}

static void frame_pick(struct rect c)
{
	font_draw(theme.small, c.x, c.y, theme.text_dim, "Website", -1);
	if (ui_text_field(ID_URL, (struct rect){c.x, c.y + 22, c.w - 330, 48}, g.url, "www.example.com",
			art_icon(ICON_WAYBACK))) {
		osk_open("Website address", g.url, sizeof g.url - 1);
		g.editing = 1;
	}
	if (ui_button(ID_GO, (struct rect){c.x + c.w - 316, c.y + 22, 120, 48}, "Go") && g.url[0] && !g.req)
		lookup();
	char bl[32];
	snprintf(bl, sizeof bl, "Bookmarks (%d)", bookmarks_count());
	if (ui_button(ID_BOOKMARKS, (struct rect){c.x + c.w - 186, c.y + 22, 186, 48}, bl)) {
		g.mode = BOOKMARKS;
		ui_focus(0);
	}

	float y = c.y + 82;
	font_draw(theme.small, c.x, y, theme.text_dim, "Year", -1);
	y += 22;
	int per_row = draw_width() > 700 ? 13 : 9;
	float gap = 6, bw = (c.w - gap * (float)(per_row - 1)) / (float)per_row, bh = 34;
	for (int i = 0; i < YEARS; i++) {
		char label[8];
		snprintf(label, sizeof label, "%d", FIRST_YEAR + i);
		if ((float)font_measure(theme.body, label, -1) > bw - 10) /* narrow buttons: '96 */
			snprintf(label, sizeof label, "'%02d", (FIRST_YEAR + i) % 100);
		struct rect r = {c.x + (float)(i % per_row) * (bw + gap), y + (float)(i / per_row) * (bh + gap), bw, bh};
		if (FIRST_YEAR + i == g.year)
			draw_rect((struct rect){r.x - 2, r.y - 2, r.w + 4, r.h + 4}, theme.accent);
		if (ui_button(ID_YEAR + i, r, label))
			g.year = FIRST_YEAR + i;
	}
	y += (float)((YEARS + per_row - 1) / per_row) * (bh + gap) + 10;

	font_draw(theme.small, c.x, y, theme.text_dim, "Famous old sites", -1);
	y += 22;
	int n = (int)(sizeof k_suggest / sizeof k_suggest[0]);
	int cols = draw_width() > 700 ? 3 : 2;
	float sw = (c.w - gap * (float)(cols - 1)) / (float)cols;
	for (int i = 0; i < n; i++) {
		struct rect r = {c.x + (float)(i % cols) * (sw + gap), y + (float)(i / cols) * (40 + gap), sw, 40};
		if (r.y + r.h > c.y + c.h)
			break;
		if (ui_button(ID_SUGGEST + i, r, k_suggest[i].label) && !g.req) {
			snprintf(g.url, sizeof g.url, "%s", k_suggest[i].url);
			g.year = atoi(k_suggest[i].year);
			lookup();
		}
	}
}

static void frame_months(struct rect c)
{
	char head[320];
	snprintf(head, sizeof head, "%s in %d: %d day%s with captures. Pick a month.", g.url, g.year, g.ncaps,
		g.ncaps == 1 ? "" : "s");
	font_draw_clipped(theme.body, c.x, c.y, theme.text, head, (int)c.w);
	int cols = draw_width() > 700 ? 4 : 3;
	float gap = 8, bw = (c.w - gap * (float)(cols - 1)) / (float)cols, bh = 52;
	for (int m = 0; m < 12; m++) {
		struct rect r = {c.x + (float)(m % cols) * (bw + gap), c.y + 40 + (float)(m / cols) * (bh + gap), bw, bh};
		int n = captures_in(m, 0);
		char label[48];
		snprintf(label, sizeof label, n ? "%s (%d)" : "%s", k_months[m], n);
		if (!n) {
			draw_rect(r, RGBA(0x2a, 0x2b, 0x30, 0xFF));
			int tw = font_measure(theme.body, label, -1);
			font_draw(theme.body, r.x + (r.w - (float)tw) / 2, r.y + 12, theme.text_dim, label, -1);
			continue;
		}
		if (ui_button(ID_MONTH + m, r, label)) {
			g.month = m;
			g.mode = DAYS;
			ui_focus(0);
		}
	}
}

static void frame_days(struct rect c)
{
	char head[96];
	snprintf(head, sizeof head, "%s %d: pick a day", k_months[g.month], g.year);
	font_draw(theme.body, c.x, c.y, theme.text, head, -1);
	static const char *const wd[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	float gap = 6, cw = (c.w - gap * 6) / 7, ch = 40;
	float top = c.y + 38;
	for (int i = 0; i < 7; i++) {
		int tw = font_measure(theme.small, wd[i], -1);
		font_draw(theme.small, c.x + (float)i * (cw + gap) + (cw - (float)tw) / 2, top, theme.text_dim, wd[i], -1);
	}
	top += 24;
	int first = weekday(g.year, g.month + 1, 1), nd = days_in(g.year, g.month + 1);
	for (int d = 1; d <= nd; d++) {
		int cell = first + d - 1;
		struct rect r = {c.x + (float)(cell % 7) * (cw + gap), top + (float)(cell / 7) * (ch + gap), cw, ch};
		char label[4];
		snprintf(label, sizeof label, "%d", d);
		const char *ts = capture_on(g.month, d);
		if (!ts) {
			int tw = font_measure(theme.body, label, -1);
			font_draw(theme.body, r.x + (r.w - (float)tw) / 2, r.y + 8, RGBA(0x60, 0x62, 0x68, 0xFF), label, -1);
			continue;
		}
		if (ui_button(ID_DAY + d, r, label))
			open_page(ts);
	}
}

static void frame_bookmarks(struct rect c)
{
	int n = bookmarks_count();
	font_draw(theme.body, c.x, c.y, theme.text, n ? "Bookmarks" : "No bookmarks yet: use Save on a page.", -1);
	float y = c.y + 36, bh = 44, gap = 6;
	for (int i = 0; i < n && y + bh <= c.y + c.h; i++, y += bh + gap) {
		const struct bookmark *b = bookmarks_get(i);
		char label[200];
		snprintf(label, sizeof label, "%.4s-%.2s-%.2s   %s", b->timestamp, b->timestamp + 4, b->timestamp + 6,
			b->title);
		if (ui_button(ID_MARK + i, (struct rect){c.x, y, c.w, bh}, label)) {
			const char *u = b->url;
			if (strncmp(u, "http://", 7) == 0)
				u += 7;
			snprintf(g.url, sizeof g.url, "%s", u);
			char yy[5] = {0};
			memcpy(yy, b->timestamp, 4);
			g.year = atoi(yy);
			open_page(b->timestamp);
		}
	}
}

static void frame(void)
{
	struct rect c = app_chrome("Wayback Machine", "Browse websites as they were");
	poll_lookup();
	if (!g.editing && !g.req && ui_pressed(BTN_B)) {
		if (g.mode == DAYS)
			g.mode = MONTHS;
		else if (g.mode != PICK)
			g.mode = PICK;
		else {
			app_pop();
			return;
		}
		ui_focus(0);
		return;
	}

	switch (g.mode) {
	case PICK: frame_pick(c); break;
	case MONTHS: frame_months(c); break;
	case DAYS: frame_days(c); break;
	case BOOKMARKS: frame_bookmarks(c); break;
	}

	if (g.message[0] || g.req) {
		struct rect all = {0, 0, (float)draw_width(), UI_H};
		if (g.req) {
			ui_modal_begin();
			app_spinner(all, g.message);
			ui_modal_end();
		} else {
			font_draw(theme.small, c.x, c.y + c.h - 4, theme.danger, g.message, -1);
		}
	}
	ui_hints(g.mode == PICK ? "(A) Select    (B) Back" : "(A) Select    (B) Previous step");

	if (g.editing) {
		int r = osk_update();
		if (r != OSK_EDITING) {
			g.editing = 0;
			if (r == OSK_DONE) {
				const char *t = osk_text();
				if (strncmp(t, "http://", 7) == 0)
					t += 7;
				snprintf(g.url, sizeof g.url, "%s", t);
			}
		}
	}
}

const struct screen screen_wayback = {"wayback", enter, frame};
