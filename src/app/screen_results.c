/* Results grid: a search or a category, one numbered page of advancedsearch at a time, with
 * thumbnails. A category's list has a search button that searches inside it; the results open
 * over the list (a new search from them replaces them), and Back returns to the list as it was.
 * Holding B and moving the pointer drags the grid; Prev and Next (or -/+) change the page. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/app.h"
#include "ia/ia.h"
#include "net/http.h"
#include "ui/drag.h"
#include "ui/osk.h"

#define PAGE_ROWS 30
/* advancedsearch refuses to page past its first 10,000 results. */
#define MAX_PAGE (10000 / PAGE_ROWS)
#define ID_CARD 100
#define ID_RETRY 1
#define ID_SEARCH 3
#define ID_PREV 4
#define ID_NEXT 5

struct state {
	int depth; /* app_depth() of the screen this belongs to, 0 for none */
	struct results_args args;
	struct ia_doc *docs; /* the page shown */
	int count, total;
	int page;      /* the page shown (1-based), 0 before the first arrives */
	int want_page; /* the page requested */
	http_req *req;
	int error;
	char error_msg[96];
	char note[96]; /* e.g. the search fell back to matching anywhere */
	float scroll, scroll_target;
	int focus_index;
	struct drag drag;
	int typing;
	char last_search[128]; /* what the search button last searched for from this list */
	struct results_args next; /* applied after the frame */
	int pending;
};

/* The results screen on top, and those further down the stack (a collection opened from a list,
 * a search over a category's list) by depth, kept as they were for Back. */
static struct state g, saved[APP_STACK_MAX];

static int page_count(void)
{
	int n = (g.total + PAGE_ROWS - 1) / PAGE_ROWS;
	return n > MAX_PAGE ? MAX_PAGE : n;
}

static void request_page(int page)
{
	char url[4096];
	if (ia_search_url(url, sizeof url, g.args.query, (enum ia_sort)g.args.sort, PAGE_ROWS, page) != 0) {
		g.error = 1;
		snprintf(g.error_msg, sizeof g.error_msg, "That search is too long.");
		return;
	}
	if (g.req)
		http_release(g.req);
	struct http_opts o;
	http_opts_default(&o);
	o.priority = 10; /* ahead of thumbnails */
	g.req = http_get(url, &o);
	g.want_page = page;
	if (page == 1)
		WL_LOG_I("RESULTS", "%s: %s", g.args.title, g.args.query);
}

static void discard(struct state *s)
{
	free(s->docs);
	if (s->req)
		http_release(s->req);
	memset(s, 0, sizeof *s);
}

/* Shows `args` from its first page, on this screen. */
static void load(const struct results_args *args)
{
	int depth = g.depth;
	discard(&g);
	g.depth = depth;
	if (args)
		g.args = *args;
	g.focus_index = -1;
}

static void enter(void *arg)
{
	int depth = app_depth();
	/* The results screen below this one keeps its list for Back; any at or above this depth
	 * were closed. */
	for (int i = depth; i < APP_STACK_MAX; i++)
		discard(&saved[i]);
	if (g.depth > 0 && g.depth < depth && g.depth < APP_STACK_MAX) {
		/* A page still loading is dropped (a list with none yet asks again on the way back). */
		if (g.req) {
			http_release(g.req);
			g.req = NULL;
			g.want_page = g.page;
		}
		memset(&g.drag, 0, sizeof g.drag);
		saved[g.depth] = g;
		memset(&g, 0, sizeof g);
	}
	load(arg);
	g.depth = depth;
}

/* Back to a results screen further down the stack: its list as it was left. */
static void restore(void)
{
	int depth = app_depth();
	if (g.depth == depth)
		return;
	discard(&g);
	if (depth < APP_STACK_MAX && saved[depth].depth == depth) {
		g = saved[depth];
		memset(&saved[depth], 0, sizeof saved[depth]);
	} else {
		g.depth = depth;
		g.focus_index = -1;
	}
}

/* The search button and its keyboard (a category's list, or a search inside one), opened on the
 * words last searched for. */
static void search_button(struct rect c)
{
	if (!g.args.mediatype[0])
		return;
	char label[48];
	snprintf(label, sizeof label, "Search %s", g.args.category);
	float bw = (float)font_measure(theme.body, label, -1) + 40;
	if (ui_button(ID_SEARCH, (struct rect){c.x + c.w - bw, c.y - 74, bw, 44}, label)) {
		osk_open(label, g.args.search[0] ? g.args.search : g.last_search, 127);
		g.typing = 1;
	}
}

static void search_update(void)
{
	if (!g.typing)
		return;
	int r = osk_update();
	if (r == OSK_EDITING)
		return;
	g.typing = 0;
	if (r != OSK_DONE || !osk_text()[0])
		return;
	memset(&g.next, 0, sizeof g.next);
	snprintf(g.next.title, sizeof g.next.title, "\"%s\" in %s", osk_text(), g.args.category);
	ia_user_query(osk_text(), g.args.mediatype, 1, g.next.query, sizeof g.next.query);
	ia_user_query(osk_text(), g.args.mediatype, 0, g.next.fallback, sizeof g.next.fallback);
	if (!g.next.query[0])
		return;
	snprintf(g.next.mediatype, sizeof g.next.mediatype, "%s", g.args.mediatype);
	snprintf(g.next.category, sizeof g.next.category, "%s", g.args.category);
	snprintf(g.next.search, sizeof g.next.search, "%s", osk_text());
	snprintf(g.last_search, sizeof g.last_search, "%s", osk_text());
	g.next.sort = IA_SORT_POPULAR;
	g.pending = 1;
}

/* Collects a finished page request: it replaces the page shown. */
static void poll_request(void)
{
	if (!g.req && !g.error && g.page == 0 && net_state() == NET_READY)
		request_page(1);
	if (!g.req || http_state(g.req) == HTTP_PENDING)
		return;
	const struct http_result *r = http_result(g.req);
	struct ia_search s;
	if (http_state(g.req) == HTTP_DONE && r->status == 200 && ia_parse_search((const char *)r->body, &s) == 0) {
		WL_LOG_I("RESULTS", "page %d: %d items (%d total) in %u ms", g.want_page, s.count, s.total, r->elapsed_ms);
		http_release(g.req);
		g.req = NULL;
		/* Nothing has the words in its title or creator: match them anywhere instead. */
		if (s.total == 0 && g.want_page == 1 && g.args.fallback[0]) {
			ia_search_free(&s);
			snprintf(g.args.query, sizeof g.args.query, "%s", g.args.fallback);
			g.args.fallback[0] = '\0';
			snprintf(g.note, sizeof g.note, "No titles match; showing items that mention it.");
			request_page(1);
			return;
		}
		/* A new list starts on its first card (not the Search button above it), so A opens it;
		 * a new page starts at its top, on its first card if a card had the focus. */
		int focus = ui_get_focus();
		if (s.count && (g.page == 0 || (focus >= ID_CARD && focus < ID_CARD + g.count)))
			ui_focus(ID_CARD);
		g.focus_index = -1;
		free(g.docs);
		g.docs = s.docs; /* taken over: not freed with s */
		g.count = s.count;
		g.total = s.total;
		g.page = g.want_page;
		g.scroll = g.scroll_target = 0;
		s.docs = NULL;
		s.count = 0;
		ia_search_free(&s);
		return;
	}
	if (g.page == 0) {
		g.error = 1;
		if (http_state(g.req) == HTTP_FAILED)
			snprintf(g.error_msg, sizeof g.error_msg, "Couldn't reach archive.org (%s).", http_strerror(r->error));
		else
			snprintf(g.error_msg, sizeof g.error_msg, "archive.org answered with an error (%d).", r->status);
		WL_LOG_W("RESULTS", "%s", g.error_msg);
	} else {
		snprintf(g.note, sizeof g.note, "Page %d didn't load. Try again.", g.want_page);
		WL_LOG_W("RESULTS", "page %d failed (status %d)", g.want_page, r->status);
	}
	http_release(g.req);
	g.req = NULL;
}

static void draw_card(int i, struct rect r)
{
	const struct ia_doc *d = &g.docs[i];
	int hot = 0;
	/* The card is a tile without text inside; text goes underneath. */
	struct rect thumb = {r.x, r.y, r.w, r.w * 0.75f};
	if (ui_tile(ID_CARD + i, thumb, "", NULL, theme.panel_hi, NULL)) {
		app_push(&screen_item, (void *)d->id);
	}
	hot = ui_get_focus() == ID_CARD + i;
	if (hot)
		g.focus_index = i;
	const struct image *img = thumb_get(d->id);
	struct rect inner = {thumb.x + 10, thumb.y + 4, thumb.w - 14, thumb.h - 8};
	if (img)
		ui_draw_image_fit(inner, img, 0xFFFFFFFFu);
	else
		font_draw(theme.small, inner.x + inner.w / 2 - 12, inner.y + inner.h / 2 - 10, theme.text_dim, "...", -1);
	float ty = thumb.y + thumb.h + 4;
	font_draw_clipped(theme.small, r.x, ty, hot ? theme.text : RGBA(0xdd, 0xdd, 0xdd, 0xff), d->title, (int)r.w);
	char sub[128];
	snprintf(sub, sizeof sub, "%s%s%s", d->year, d->year[0] && d->creator[0] ? " - " : "", d->creator);
	font_draw_clipped(theme.small, r.x, ty + (float)font_line_height(theme.small), theme.text_dim, sub, (int)r.w);
}

static void frame_body(void);

static void frame(void)
{
	restore();
	frame_body();
	search_update();
	if (g.pending) {
		/* A search from a category's list opens over it, so Back comes back to it; a new
		 * search from those results replaces them. */
		static struct results_args next; /* outlives this frame: the push happens after it */
		next = g.next;
		g.pending = 0;
		if (g.args.search[0])
			load(&next);
		else
			app_push(&screen_results, &next);
	}
}

/* "(-) Prev   Page N of M   Next (+)", right-aligned in the hint strip so the grid keeps its height. */
static void page_bar(void)
{
	struct rect s = ui_safe_area();
	float h = (float)font_line_height(theme.small) + 12;
	struct rect strip = {s.x, s.y + s.h - h, s.w, h};
	int pages = page_count();
	int shown = g.req ? g.want_page : g.page;
	char label[64];
	if (g.req && g.page)
		snprintf(label, sizeof label, "Loading page %d...", g.want_page);
	else
		snprintf(label, sizeof label, "Page %d of %d%s", shown, pages, g.total > MAX_PAGE * PAGE_ROWS ? "+" : "");
	float bw = 120, gap = 12, lw = (float)font_measure(theme.small, label, -1);
	float x = strip.x + strip.w - bw, by = strip.y + 2, bh = strip.h - 4;
	if (shown < pages && (ui_button(ID_NEXT, (struct rect){x, by, bw, bh}, "Next (+)") || ui_pressed(BTN_PLUS)))
		request_page(shown + 1);
	x -= gap + lw;
	font_draw(theme.small, x, strip.y + 6, theme.text, label, -1);
	x -= gap + bw;
	if (shown > 1 && (ui_button(ID_PREV, (struct rect){x, by, bw, bh}, "(-) Prev") || ui_pressed(BTN_MINUS)))
		request_page(shown - 1);
}

static void frame_body(void)
{
	struct rect c = app_chrome(g.args.title, g.total ? NULL : "Internet Archive");
	poll_request();
	float drag_dx = 0, drag_dy = 0;
	if (!g.typing)
		drag_update(&g.drag, ui_input(), &drag_dx, &drag_dy);
	if (!g.typing && app_back()) {
		app_pop();
		return;
	}
	if (g.drag.active)
		ui_set_cursor(UI_CURSOR_GRAB);
	search_button(c);
	if (g.total) {
		char count[48];
		snprintf(count, sizeof count, "%d result%s", g.total, g.total == 1 ? "" : "s");
		int w = font_measure(theme.small, count, -1);
		font_draw(theme.small, c.x + c.w - (float)w, c.y - 28, theme.text_dim, count, -1);
	}

	if (g.count == 0) {
		if (g.error) {
			font_draw_wrapped(theme.body, c.x, c.y + 20, (int)c.w, theme.text, g.error_msg);
			if (ui_button(ID_RETRY, (struct rect){c.x, c.y + 90, 180, 46}, "Try again")) {
				g.error = 0;
				request_page(1);
			}
		} else if (net_state() == NET_FAILED) {
			font_draw_wrapped(theme.body, c.x, c.y + 20, (int)c.w, theme.text,
				"No network connection. Check the Wii's Internet settings, then come back.");
		} else if (g.page > 0 && !g.args.search[0] && !strncmp(g.args.query, "collection:", 11)) {
			font_draw_wrapped(theme.body, c.x, c.y + 20, (int)c.w, theme.text,
				"Nothing in this collection opens on the Wii: items that can only be borrowed or "
				"streamed on archive.org are left out.");
		} else if (g.page > 0) {
			font_draw(theme.body, c.x, c.y + 20, theme.text, "Nothing found.", -1);
		} else {
			app_spinner(c, net_state() == NET_READY ? "Searching the archive..." : "Connecting to the network...");
		}
		ui_hints("");
		return;
	}

	/* A note (fallback search, failed page) above the grid. */
	struct rect grid = c;
	if (g.note[0]) {
		font_draw_clipped(theme.small, c.x, c.y, theme.accent, g.note, (int)c.w);
		float nh = (float)font_line_height(theme.small) + 6;
		grid.y += nh;
		grid.h -= nh;
	}

	/* Grid: 5 columns on 16:9, 4 on 4:3; scrolls to keep the focused card in view. */
	int cols = draw_width() > 700 ? 5 : 4;
	float gap = 14;
	float cw = (grid.w - gap * (float)(cols - 1)) / (float)cols;
	float ch = cw * 0.75f + 4 + 2 * (float)font_line_height(theme.small) + 12;
	if (g.focus_index >= 0) {
		float fy = (float)(g.focus_index / cols) * ch;
		if (fy - g.scroll_target < 0)
			g.scroll_target = fy;
		if (fy + ch - g.scroll_target > grid.h)
			g.scroll_target = fy + ch - grid.h;
	}
	/* Dragging with B, or the pointer at the top/bottom edge, scrolls too. */
	const struct input_state *in = ui_input();
	float rows_h = (float)((g.count + cols - 1) / cols) * ch;
	if (g.drag.active) {
		g.scroll_target -= drag_dy;
		if (g.scroll_target > rows_h - grid.h)
			g.scroll_target = rows_h - grid.h;
		g.scroll = g.scroll_target;
	} else if (in->pointer_valid && in->px >= grid.x && in->px < grid.x + grid.w) {
		if (in->py > grid.y + grid.h - 24 && in->py < grid.y + grid.h && g.scroll_target < rows_h - grid.h)
			g.scroll_target += 6;
		if (in->py < grid.y + 24 && in->py >= grid.y && g.scroll_target > 0)
			g.scroll_target -= 6;
	}
	if (g.scroll_target < 0)
		g.scroll_target = 0;
	g.scroll += (g.scroll_target - g.scroll) * 0.3f;

	draw_clip(grid);
	g.focus_index = -1;
	int first_row = (int)(g.scroll / ch), last_row = (int)((g.scroll + grid.h) / ch);
	for (int i = 0; i < g.count; i++) {
		int row = i / cols, col = i % cols;
		/* Off-screen cards are still declared (for D-pad focus) but kept cheap. */
		struct rect r = {grid.x + (float)col * (cw + gap), grid.y + (float)row * ch - g.scroll, cw, ch - 12};
		if (row < first_row - 1 || row > last_row + 1) {
			if (ui_get_focus() == ID_CARD + i)
				g.focus_index = i;
			if (ui_region(ID_CARD + i, (struct rect){r.x, r.y, r.w, r.w * 0.75f}))
				app_push(&screen_item, (void *)g.docs[i].id);
			continue;
		}
		draw_card(i, r);
	}
	draw_clip_reset();

	ui_hints("(A) Open    Hold (B) to scroll");
	page_bar();
}

const struct screen screen_results = {"results", enter, frame};
