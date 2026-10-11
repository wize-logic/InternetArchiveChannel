/* Book and image viewer: one page/image at a time, fitted to the screen, with the neighbours
 * prefetched; zoom and pan; for books, a text mode (OCR or a plain-text edition, reflowed into
 * paragraphs). Books without page images open straight in text mode.
 * Holding B drags: it pans a zoomed page, turns the page when swiped sideways at normal size, and
 * scrolls the text. A tap of B leaves text mode, then the viewer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/app.h"
#include "ia/ia.h"
#include "net/http.h"
#include "ui/drag.h"

#define CACHE 5          /* current page +-2 */
#define BOOK_WIDTH 700   /* page JPEG width: ~1000 px tall, under the 1024 texture limit */

enum { ID_PREV = 1, ID_NEXT, ID_ZOOM_OUT, ID_ZOOM_IN, ID_TEXT, ID_CLOSE };

struct page {
	int index; /* -1: empty */
	http_req *req;
	struct image img;
	int failed;
	int own_size; /* IIIF refused to fit it in the box (it is smaller): asked for its own size */
};

static struct {
	struct viewer_args a;
	int page;
	struct page cache[CACHE];
	float zoom, pan_x, pan_y;
	/* text mode */
	int text_mode;
	http_req *text_req;
	char *text;
	int *lines; /* byte offsets of wrapped lines */
	int line_count, line_cap, text_failed;
	float text_scroll;
	int osd_frames;
	/* books: the page count is read from the page list first */
	http_req *count_req;
	int counting;
	struct drag drag;
	float swipe; /* sideways drag at normal size: turns the page when released */
} g;

static void cache_drop(struct page *p)
{
	if (p->req)
		http_release(p->req);
	image_free(&p->img);
	memset(p, 0, sizeof *p);
	p->index = -1;
}

static void free_text(void)
{
	if (g.count_req)
		http_release(g.count_req);
	g.count_req = NULL;
	if (g.text_req)
		http_release(g.text_req);
	free(g.text);
	free(g.lines);
	g.text_req = NULL;
	g.text = NULL;
	g.lines = NULL;
	g.line_count = g.line_cap = 0;
}

static void enter(void *arg)
{
	for (int i = 0; i < CACHE; i++)
		cache_drop(&g.cache[i]);
	free_text();
	memset(&g, 0, sizeof g);
	for (int i = 0; i < CACHE; i++)
		g.cache[i].index = -1;
	if (arg)
		g.a = *(const struct viewer_args *)arg;
	g.zoom = 1;
	g.osd_frames = 240;
	if (g.a.is_book && g.a.count_url[0]) {
		struct http_opts o;
		http_opts_default(&o);
		o.priority = 10;
		o.max_body = 4u << 20;
		g.count_req = http_get(g.a.count_url, &o);
		g.counting = 1;
	} else if (g.a.is_book && g.a.count == 0) {
		g.text_mode = 1; /* text only */
	}
}

/* The page list answered (or failed): its count wins over the metadata's imagecount, which
 * also counts leaves the reader leaves out. No pages at all: text only. */
static void count_poll(void)
{
	if (!g.count_req || http_state(g.count_req) == HTTP_PENDING)
		return;
	const struct http_result *r = http_result(g.count_req);
	int n = http_state(g.count_req) == HTTP_DONE && r->status == 200 && r->body
		? ia_parse_page_count((const char *)r->body)
		: -1;
	if (n > 0)
		g.a.count = n;
	else
		WL_LOG_W("VIEWER", "page list: status %d, using %d pages", r->status, g.a.count);
	http_release(g.count_req);
	g.count_req = NULL;
	g.counting = 0;
	if (g.a.count == 0)
		g.text_mode = 1;
}

static void page_url(int index, int own_size, char *url, size_t cap)
{
	if (g.a.is_book)
		ia_book_page_url(url, cap, g.a.id, index, BOOK_WIDTH);
	else
		ia_iiif_url(url, cap, g.a.id, g.a.files[index], own_size ? 0 : 1024, 1024);
}

static http_req *page_request(int index, int own_size, int priority)
{
	char url[1024];
	page_url(index, own_size, url, sizeof url);
	struct http_opts o;
	http_opts_default(&o);
	o.priority = priority;
	o.max_body = 4u << 20;
	return http_get(url, &o);
}

/* Keeps the current page and its neighbours loading; drops pages that moved out of range. */
static void schedule(void)
{
	for (int i = 0; i < CACHE; i++) {
		struct page *p = &g.cache[i];
		if (p->index >= 0 && (p->index < g.page - 2 || p->index > g.page + 2))
			cache_drop(p);
	}
	static const int order[CACHE] = {0, 1, -1, 2, -2};
	for (int k = 0; k < CACHE; k++) {
		int idx = g.page + order[k];
		if (idx < 0 || idx >= g.a.count)
			continue;
		int have = 0;
		for (int i = 0; i < CACHE && !have; i++)
			have = g.cache[i].index == idx;
		if (have)
			continue;
		for (int i = 0; i < CACHE; i++) {
			struct page *p = &g.cache[i];
			if (p->index >= 0)
				continue;
			p->index = idx;
			p->own_size = 0;
			p->req = page_request(idx, 0, k == 0 ? 10 : 1);
			break;
		}
	}
	/* Decode at most one finished page per frame (they're big). */
	for (int i = 0; i < CACHE; i++) {
		struct page *p = &g.cache[i];
		if (!p->req || http_state(p->req) == HTTP_PENDING)
			continue;
		const struct http_result *r = http_result(p->req);
		if (!g.a.is_book && !p->own_size && http_state(p->req) == HTTP_DONE && r->status == 400) {
			http_release(p->req);
			p->own_size = 1;
			p->req = page_request(p->index, 1, p->index == g.page ? 10 : 1);
			continue;
		}
		p->failed = !(http_state(p->req) == HTTP_DONE && r->status == 200 &&
			image_load_any(&p->img, r->body, r->body_len, 1024, 1024) == 0);
		if (p->failed)
			WL_LOG_W("VIEWER", "page %d: status %d, %u bytes", p->index, r->status, (unsigned)r->body_len);
		http_release(p->req);
		p->req = NULL;
		break;
	}
}

static struct page *current(void)
{
	for (int i = 0; i < CACHE; i++)
		if (g.cache[i].index == g.page)
			return &g.cache[i];
	return NULL;
}

static void go(int delta)
{
	int to = g.page + delta;
	if (to < 0 || to >= g.a.count)
		return;
	g.page = to;
	g.zoom = 1;
	g.pan_x = g.pan_y = 0;
	g.swipe = 0;
}

/* Wraps the whole OCR text once into a line table, so drawing only touches visible lines. */
static void layout_text(int width)
{
	free(g.lines);
	g.line_cap = 4096;
	g.lines = malloc(sizeof *g.lines * (size_t)g.line_cap);
	g.line_count = 0;
	if (!g.lines)
		return;
	const char *t = g.text;
	while (*t) {
		if (g.line_count == g.line_cap) {
			int *more = realloc(g.lines, sizeof *g.lines * (size_t)g.line_cap * 2);
			if (!more)
				break;
			g.lines = more;
			g.line_cap *= 2;
		}
		g.lines[g.line_count++] = (int)(t - g.text);
		const char *nl = strchr(t, '\n');
		size_t para = nl ? (size_t)(nl - t) : strlen(t);
		char save = t[para];
		((char *)t)[para] = '\0'; /* measure within the paragraph */
		int fit = font_fit(theme.body, t, width);
		((char *)t)[para] = save;
		if ((size_t)fit >= para) {
			t += para + (nl ? 1 : 0);
			continue;
		}
		/* Break at the last space that fits; a word too long for a line is cut, but never inside
		 * a UTF-8 character. */
		int cut = fit;
		while (cut > 0 && t[cut] != ' ')
			cut--;
		if (cut > 0) {
			t += cut + 1;
		} else {
			int n = fit > 0 ? fit : 1;
			while (n < (int)para && ((unsigned char)t[n] & 0xC0) == 0x80)
				n++;
			t += n;
		}
	}
}

static float drag_dx, drag_dy; /* this frame's B-drag */

static void text_frame(struct rect c)
{
	if (!g.text && !g.text_req && !g.text_failed) {
		struct http_opts o;
		http_opts_default(&o);
		o.priority = 10;
		o.max_body = 8u << 20;
		g.text_req = http_get(g.a.text_url, &o);
	}
	if (g.text_req && http_state(g.text_req) != HTTP_PENDING) {
		const struct http_result *r = http_result(g.text_req);
		if (http_state(g.text_req) == HTTP_DONE && r->status == 200 && r->body_len) {
			g.text = ia_reflow_text((const char *)r->body, r->body_len);
			if (g.text)
				layout_text((int)c.w - 20);
			else
				g.text_failed = 1;
		} else {
			WL_LOG_W("VIEWER", "text: status %d, %u bytes", r->status, (unsigned)r->body_len);
			g.text_failed = 1;
		}
		http_release(g.text_req);
		g.text_req = NULL;
	}
	if (!g.text) {
		if (!g.text_failed)
			ui_set_cursor(UI_CURSOR_WAIT);
		app_spinner(c, g.text_failed ? "The text couldn't be loaded." : "Loading text...");
		return;
	}
	float lh = (float)font_line_height(theme.body);
	const struct input_state *in = ui_input();
	g.text_scroll -= drag_dy;
	if (in->held & BTN_DOWN)
		g.text_scroll += 6;
	if (in->held & BTN_UP)
		g.text_scroll -= 6;
	if (ui_pressed(BTN_RIGHT))
		g.text_scroll += c.h - lh;
	if (ui_pressed(BTN_LEFT))
		g.text_scroll -= c.h - lh;
	float max = (float)g.line_count * lh - c.h;
	if (g.text_scroll > max)
		g.text_scroll = max;
	if (g.text_scroll < 0)
		g.text_scroll = 0;
	draw_rect(c, RGBA(0xf4, 0xef, 0xe2, 0xFF)); /* paper */
	draw_clip(c);
	int first = (int)(g.text_scroll / lh);
	for (int i = first; i < g.line_count && (float)(i - first) * lh < c.h + lh; i++) {
		int start = g.lines[i], end = i + 1 < g.line_count ? g.lines[i + 1] : (int)strlen(g.text);
		while (end > start && (g.text[end - 1] == '\n' || g.text[end - 1] == ' '))
			end--;
		font_draw(theme.body, c.x + 10, c.y + (float)i * lh - g.text_scroll, RGBA(0x22, 0x20, 0x1c, 0xFF),
			g.text + start, end - start);
	}
	draw_clip_reset();
}

static void close_viewer(void)
{
	for (int i = 0; i < CACHE; i++)
		cache_drop(&g.cache[i]);
	free_text();
	app_pop();
}

static void set_zoom(float z)
{
	if (z < 1)
		z = 1;
	if (z > 3)
		z = 3;
	if (z != g.zoom)
		WL_LOG_I("VIEWER", "zoom %.1fx", (double)z);
	g.zoom = z;
}

static void frame(void)
{
	/* The page gets the whole screen; title and controls are an overlay that fades out. */
	draw_rect((struct rect){0, 0, (float)draw_width(), UI_H}, RGBA(0x12, 0x12, 0x14, 0xFF));
	struct rect s = ui_safe_area();
	const struct input_state *in = ui_input();
	/* B: a tap leaves text mode (when there are pages to go back to), then the viewer; held,
	 * it drags. A sideways swipe at normal size turns the page when B is let go. */
	int was_dragging = g.drag.active;
	int b = drag_update(&g.drag, ui_modal_active() ? &(struct input_state){0} : in, wl_platform_time_us(),
		&drag_dx, &drag_dy);
	if (b == DRAG_TAP) {
		if (g.text_mode && g.a.count > 0) {
			g.text_mode = 0;
		} else {
			close_viewer();
			return;
		}
	}
	if (g.drag.active)
		ui_set_cursor(UI_CURSOR_GRAB);
	if (in->pressed || in->pointer_moved)
		g.osd_frames = 240;
	struct rect view = s;
	count_poll();

	if (g.counting) {
		ui_set_cursor(UI_CURSOR_WAIT);
		app_spinner(view, "Opening the book...");
	} else if (g.text_mode) {
		text_frame((struct rect){s.x, s.y + 40, s.w, s.h - 100});
	} else {
		schedule();
		struct page *p = current();
		if (!p || (!p->img.w && !p->failed)) {
			if (!g.drag.active)
				ui_set_cursor(UI_CURSOR_WAIT);
			app_spinner(view, "Loading page...");
		} else if (p->failed) {
			app_spinner(view, "This page couldn't be loaded.");
		} else {
			/* Fit, then zoom around the centre; pan by dragging with B, with the D-pad, or with the
			 * pointer near the edges. */
			float sx = view.w / (float)p->img.w, sy = view.h / (float)p->img.h, fit = sx < sy ? sx : sy;
			float w = (float)p->img.w * fit * g.zoom, h = (float)p->img.h * fit * g.zoom;
			float swipe_x = 0;
			if (g.zoom > 1) {
				float step = 8;
				int edges = in->pointer_valid && !g.drag.active;
				g.pan_x -= drag_dx;
				g.pan_y -= drag_dy;
				if (in->held & BTN_RIGHT || (edges && in->px > view.x + view.w - 30))
					g.pan_x += step;
				if (in->held & BTN_LEFT || (edges && in->px < view.x + 30))
					g.pan_x -= step;
				if (in->held & BTN_DOWN || (edges && in->py > view.y + view.h - 30))
					g.pan_y += step;
				if (in->held & BTN_UP || (edges && in->py < view.y + 30))
					g.pan_y -= step;
				float mx = (w - view.w) / 2 > 0 ? (w - view.w) / 2 : 0, my = (h - view.h) / 2 > 0 ? (h - view.h) / 2 : 0;
				g.pan_x = g.pan_x < -mx ? -mx : g.pan_x > mx ? mx : g.pan_x;
				g.pan_y = g.pan_y < -my ? -my : g.pan_y > my ? my : g.pan_y;
			} else {
				/* Normal size: the page follows a sideways drag; let go past a quarter of the
				 * screen to turn it. */
				g.swipe += drag_dx;
				if (was_dragging && !g.drag.active) {
					if (g.swipe < -view.w / 4)
						go(1);
					else if (g.swipe > view.w / 4)
						go(-1);
					g.swipe = 0;
				}
				swipe_x = g.swipe;
				if (g.osd_frames == 0 || !in->pointer_valid) {
					if (ui_pressed(BTN_RIGHT))
						go(1);
					if (ui_pressed(BTN_LEFT))
						go(-1);
				}
			}
			draw_clip(view);
			draw_image((struct rect){view.x + (view.w - w) / 2 - g.pan_x + swipe_x, view.y + (view.h - h) / 2 - g.pan_y,
					   w, h},
				&p->img.tex, 0xFFFFFFFFu);
			draw_clip_reset();
		}
	}
	if (ui_pressed(BTN_PLUS))
		set_zoom(g.zoom + 0.5f);
	if (ui_pressed(BTN_MINUS))
		set_zoom(g.zoom - 0.5f);
	if (ui_pressed(BTN_1))
		go(-1);
	if (ui_pressed(BTN_2))
		go(1);

	if (g.osd_frames == 0)
		return;
	g.osd_frames--;
	/* Overlay: title strip on top, controls at the bottom. */
	float top_h = (float)font_line_height(theme.body) + 14;
	draw_rect((struct rect){0, 0, (float)draw_width(), s.y + top_h}, RGBA(0, 0, 0, 0xB0));
	font_draw_clipped(theme.body, s.x, s.y + 6, theme.text, g.a.title, (int)s.w);
	float bw = 96, bh = 42, gap = 8, bar_h = bh + 16;
	float by = s.y + s.h - bar_h + 8;
	draw_rect((struct rect){0, by - 8, (float)draw_width(), UI_H - by + 8}, RGBA(0, 0, 0, 0xB0));
	char counter[64];
	snprintf(counter, sizeof counter, "%s %d / %d", g.a.is_book ? "Page" : "Image", g.page + 1, g.a.count);
	font_draw(theme.body, s.x, by + 8, theme.text, g.text_mode ? "Text" : counter, -1);
	/* Text/Pages toggle only when the book has both. */
	int has_text = g.a.is_book && g.a.text_url[0] && g.a.count > 0;
	float bx = s.x + s.w - (float)(5 + has_text) * (bw + gap) + gap;
	if (!g.text_mode) {
		if (ui_button(ID_PREV, (struct rect){bx, by, bw, bh}, "Prev"))
			go(-1);
		if (ui_button(ID_NEXT, (struct rect){bx + (bw + gap), by, bw, bh}, "Next"))
			go(1);
		if (ui_button(ID_ZOOM_OUT, (struct rect){bx + 2 * (bw + gap), by, bw, bh}, "Zoom -"))
			set_zoom(g.zoom - 0.5f);
		if (ui_button(ID_ZOOM_IN, (struct rect){bx + 3 * (bw + gap), by, bw, bh}, "Zoom +"))
			set_zoom(g.zoom + 0.5f);
	}
	bx += 4 * (bw + gap);
	if (has_text) {
		if (ui_button(ID_TEXT, (struct rect){bx, by, bw, bh}, g.text_mode ? "Pages" : "Text"))
			g.text_mode = !g.text_mode;
		bx += bw + gap;
	}
	if (ui_button(ID_CLOSE, (struct rect){bx, by, bw, bh}, "Close"))
		close_viewer();
}

const struct screen screen_viewer = {"viewer", enter, frame};
