/* Results grid: a search or a category, paged from advancedsearch, with thumbnails. A category's
 * list has a search button that searches inside it; the results replace the list, and B goes
 * back to the list. Holding B and moving the pointer drags the grid. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/app.h"
#include "ia/ia.h"
#include "net/http.h"
#include "ui/drag.h"
#include "ui/osk.h"

#define PAGE_ROWS 30
#define ID_CARD 100
#define ID_RETRY 1
#define ID_BACK 2
#define ID_SEARCH 3

static struct {
	struct results_args args;
	struct ia_doc *docs;
	int count, total, pages;
	http_req *req;
	int error;
	char error_msg[96];
	float scroll, scroll_target;
	int focus_index;
	struct drag drag;
	int typing;
	/* A search inside a category: the category's list, for B. */
	struct results_args parent;
	int has_parent;
	struct results_args next; /* applied after the frame */
	int pending;
} g;

static void request_page(void)
{
	char url[2048];
	if (ia_search_url(url, sizeof url, g.args.query, (enum ia_sort)g.args.sort, PAGE_ROWS, g.pages + 1) != 0) {
		g.error = 1;
		snprintf(g.error_msg, sizeof g.error_msg, "That search is too long.");
		return;
	}
	struct http_opts o;
	http_opts_default(&o);
	o.priority = 10; /* ahead of thumbnails */
	g.req = http_get(url, &o);
}

static void load(const struct results_args *args, const struct results_args *parent)
{
	free(g.docs);
	if (g.req)
		http_release(g.req);
	memset(&g, 0, sizeof g);
	if (args)
		g.args = *args;
	if (parent) {
		g.parent = *parent;
		g.has_parent = 1;
	}
	g.focus_index = -1;
}

static void enter(void *arg)
{
	load(arg, NULL);
}

/* The search button and its keyboard (a category's list, or a search inside one). */
static void search_button(struct rect c)
{
	const struct results_args *cat = g.has_parent ? &g.parent : &g.args;
	if (!cat->mediatype[0])
		return;
	char label[48];
	snprintf(label, sizeof label, "Search %s", cat->category);
	float bw = (float)font_measure(theme.body, label, -1) + 40;
	if (ui_button(ID_SEARCH, (struct rect){c.x + c.w - bw, c.y - 74, bw, 44}, label)) {
		osk_open(label, "", 127);
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
	const struct results_args *cat = g.has_parent ? &g.parent : &g.args;
	g.parent = *cat;
	memset(&g.next, 0, sizeof g.next);
	snprintf(g.next.title, sizeof g.next.title, "\"%s\" in %s", osk_text(), cat->category);
	ia_user_query(osk_text(), cat->mediatype, g.next.query, sizeof g.next.query);
	g.next.sort = IA_SORT_POPULAR;
	g.pending = 1;
}

/* Collects a finished page request. */
static void poll_request(void)
{
	if (!g.req && !g.error && g.pages == 0 && net_state() == NET_READY)
		request_page();
	if (!g.req || http_state(g.req) == HTTP_PENDING)
		return;
	const struct http_result *r = http_result(g.req);
	struct ia_search s;
	if (http_state(g.req) == HTTP_DONE && r->status == 200 && ia_parse_search((const char *)r->body, &s) == 0) {
		struct ia_doc *grown = realloc(g.docs, sizeof *g.docs * (size_t)(g.count + s.count));
		if (grown) {
			g.docs = grown;
			memcpy(g.docs + g.count, s.docs, sizeof *s.docs * (size_t)s.count);
			g.count += s.count;
			g.total = s.total;
			g.pages++;
		}
		ia_search_free(&s);
		WL_LOG_I("RESULTS", "page %d: %d items (%d total) in %u ms", g.pages, g.count, g.total, r->elapsed_ms);
	} else if (g.pages == 0) {
		g.error = 1;
		if (http_state(g.req) == HTTP_FAILED)
			snprintf(g.error_msg, sizeof g.error_msg, "Couldn't reach archive.org (%s).", http_strerror(r->error));
		else
			snprintf(g.error_msg, sizeof g.error_msg, "archive.org answered with an error (%d).", r->status);
		WL_LOG_W("RESULTS", "%s", g.error_msg);
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
	frame_body();
	search_update();
	if (g.pending) {
		/* A search inside the category replaces the list; B comes back to it. */
		struct results_args next = g.next, parent = g.parent;
		load(&next, &parent);
	}
}

/* B: back to the category list after a search inside it, else to the previous screen. */
static void back(void)
{
	if (g.has_parent) {
		struct results_args parent = g.parent;
		load(&parent, NULL);
	} else {
		app_pop();
	}
}

static void frame_body(void)
{
	struct rect c = app_chrome(g.args.title, g.total ? NULL : "Internet Archive");
	poll_request();
	float drag_dx, drag_dy;
	int b = g.typing ? DRAG_NONE : drag_update(&g.drag, ui_input(), wl_platform_time_us(), &drag_dx, &drag_dy);
	if (b == DRAG_TAP) {
		back();
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
				request_page();
			}
			if (ui_button(ID_BACK, (struct rect){c.x + 194, c.y + 90, 160, 46}, "Back"))
				back();
		} else if (net_state() == NET_FAILED) {
			font_draw_wrapped(theme.body, c.x, c.y + 20, (int)c.w, theme.text,
				"No network connection. Check the Wii's Internet settings, then come back.");
			if (ui_button(ID_BACK, (struct rect){c.x, c.y + 90, 160, 46}, "Back"))
				back();
		} else if (g.pages > 0) {
			font_draw(theme.body, c.x, c.y + 20, theme.text, "Nothing found.", -1);
			if (ui_button(ID_BACK, (struct rect){c.x, c.y + 70, 160, 46}, "Back"))
				back();
		} else {
			app_spinner(c, net_state() == NET_READY ? "Searching the archive..." : "Connecting to the network...");
		}
		ui_hints("(B) Back");
		return;
	}

	/* Grid: 5 columns on 16:9, 4 on 4:3; scrolls to keep the focused card in view. */
	int cols = draw_width() > 700 ? 5 : 4;
	float gap = 14;
	float cw = (c.w - gap * (float)(cols - 1)) / (float)cols;
	float ch = cw * 0.75f + 4 + 2 * (float)font_line_height(theme.small) + 12;
	if (g.focus_index >= 0) {
		float fy = (float)(g.focus_index / cols) * ch;
		if (fy - g.scroll_target < 0)
			g.scroll_target = fy;
		if (fy + ch - g.scroll_target > c.h)
			g.scroll_target = fy + ch - c.h;
	}
	/* Dragging with B, or the pointer at the top/bottom edge, scrolls too. */
	const struct input_state *in = ui_input();
	float rows_h = (float)((g.count + cols - 1) / cols) * ch;
	if (g.drag.active) {
		g.scroll_target -= drag_dy;
		if (g.scroll_target > rows_h - c.h)
			g.scroll_target = rows_h - c.h;
		g.scroll = g.scroll_target;
	} else if (in->pointer_valid && in->px >= c.x && in->px < c.x + c.w) {
		if (in->py > c.y + c.h - 24 && g.scroll_target < rows_h - c.h)
			g.scroll_target += 6;
		if (in->py < c.y + 24 && g.scroll_target > 0)
			g.scroll_target -= 6;
	}
	if (g.scroll_target < 0)
		g.scroll_target = 0;
	g.scroll += (g.scroll_target - g.scroll) * 0.3f;

	draw_clip(c);
	g.focus_index = -1;
	int first_row = (int)(g.scroll / ch), last_row = (int)((g.scroll + c.h) / ch);
	for (int i = 0; i < g.count; i++) {
		int row = i / cols, col = i % cols;
		/* Off-screen cards are still declared (for D-pad focus) but kept cheap. */
		struct rect r = {c.x + (float)col * (cw + gap), c.y + (float)row * ch - g.scroll, cw, ch - 12};
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

	/* Near the end: fetch the next page. */
	if (g.focus_index >= g.count - cols * 2 && g.count < g.total && !g.req && g.pages < 20)
		request_page();
	if (g.req && g.pages > 0)
		font_draw(theme.small, c.x, c.y + c.h + 2, theme.text_dim, "Loading more...", -1);
	ui_hints("(A) Open    (B) Back");
}

const struct screen screen_results = {"results", enter, frame};
