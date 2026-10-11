#include "web/webview.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <stdbool.h>

#include <libnsfb.h>
#include <libnsfb_plot.h>

#include "netsurf/browser_window.h"
#include "netsurf/keypress.h"
#include "netsurf/misc.h"
#include "netsurf/mouse.h"
#include "netsurf/netsurf.h"
#include "netsurf/plotters.h"
#include "netsurf/window.h"
#include "netsurf/bitmap.h" /* after the headers that define nserror */
#include "desktop/browser_history.h"
#include "utils/file.h"
#include "utils/filepath.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"
struct fbtk_bitmap; /* framebuffer.h uses it in a prototype without declaring it */
#include "framebuffer/fetch.h"
#include "framebuffer/findfile.h"
#include "framebuffer/font.h"
#include "framebuffer/framebuffer.h"
#include "framebuffer/schedule.h"

#include "net/http.h"
#include "ui/drag.h"
#include "ui/input.h"
#include "wl/platform/platform.h"
#include "wl/base/base.h"

#define PAGE_W 800
#define SURF_H 600 /* tallest viewport (in page pixels) we render at once */
#define RES_DIR "sd:/apps/iachannel/netsurf"

extern struct gui_bitmap_table *framebuffer_bitmap_table;
extern struct gui_layout_table *framebuffer_layout_table;
extern struct gui_utf8_table *framebuffer_utf8_table;

struct gui_window {
	struct browser_window *bw;
	int scroll_y;
	int loading;
};

static struct {
	int ready;
	nsfb_t *fb;
	struct gui_window *win;
	struct browser_window *bw;
	uint16_t *tex_buf;
	WlGxTexObj tex;
	int dirty;
	int view_h; /* page pixels visible */
	float scroll, scroll_target;
	int doc_h;
	char url[1024], title[256], hover[512];
	int pressed;
	int caret; /* a text field has the caret: the screen offers the keyboard */
	float scale;
	size_t bytes;
	enum gui_pointer_shape shape; /* what NetSurf wants under the pointer (link, text field...) */
	struct drag drag; /* B: a tap goes back, holding it drags the page */
	float scroll_x, scroll_x_target; /* pages wider than the view (fixed-width layouts, zoom) */
	int doc_w;
} g;

static const float k_scales[] = {0.5f, 0.67f, 0.8f, 1.0f, 1.25f, 1.5f, 2.0f};
#define SCALE_DEFAULT 3

/* ---- NetSurf frontend tables ------------------------------------------------------------ */

static struct gui_window *win_create(struct browser_window *bw, struct gui_window *existing,
	gui_window_create_flags flags)
{
	struct gui_window *w = calloc(1, sizeof *w);
	if (w)
		w->bw = bw;
	g.win = w;
	return w;
}

static void win_destroy(struct gui_window *w)
{
	if (g.win == w)
		g.win = NULL;
	free(w);
}

static nserror win_invalidate(struct gui_window *w, const struct rect *r)
{
	g.dirty = 1;
	return NSERROR_OK;
}

static bool win_get_scroll(struct gui_window *w, int *sx, int *sy)
{
	*sx = (int)g.scroll_x;
	*sy = (int)g.scroll;
	return true;
}

static nserror win_set_scroll(struct gui_window *w, const struct rect *r)
{
	g.scroll = g.scroll_target = (float)r->y0;
	g.scroll_x = g.scroll_x_target = (float)r->x0;
	g.dirty = 1;
	return NSERROR_OK;
}

static nserror win_get_dimensions(struct gui_window *w, int *width, int *height)
{
	*width = PAGE_W;
	*height = g.view_h ? g.view_h : 450;
	return NSERROR_OK;
}

static nserror win_event(struct gui_window *w, enum gui_window_event ev)
{
	if (ev == GW_EVENT_START_THROBBER)
		w->loading = 1;
	else if (ev == GW_EVENT_STOP_THROBBER)
		w->loading = 0;
	else if (ev == GW_EVENT_UPDATE_EXTENT || ev == GW_EVENT_NEW_CONTENT)
		g.dirty = 1;
	if (ev == GW_EVENT_NEW_CONTENT) {
		g.scroll = g.scroll_target = 0;
		g.scroll_x = g.scroll_x_target = 0;
		g.caret = 0;
		g.shape = GUI_POINTER_DEFAULT;
	}
	if (ev == GW_EVENT_REMOVE_CARET)
		g.caret = 0;
	return NSERROR_OK;
}

static void win_set_title(struct gui_window *w, const char *title)
{
	snprintf(g.title, sizeof g.title, "%s", title ? title : "");
}

static nserror win_set_url(struct gui_window *w, struct nsurl *url)
{
	snprintf(g.url, sizeof g.url, "%s", nsurl_access(url));
	return NSERROR_OK;
}

static void win_set_status(struct gui_window *w, const char *text)
{
	snprintf(g.hover, sizeof g.hover, "%s", text ? text : "");
}

static void win_place_caret(struct gui_window *w, int x, int y, int height, const struct rect *clip)
{
	g.caret = 1;
	g.dirty = 1;
}

static void win_set_pointer(struct gui_window *w, enum gui_pointer_shape shape)
{
	g.shape = shape;
}

static struct gui_window_table window_table = {
	.set_pointer = win_set_pointer,
	.place_caret = win_place_caret,
	.create = win_create,
	.destroy = win_destroy,
	.invalidate = win_invalidate,
	.get_scroll = win_get_scroll,
	.set_scroll = win_set_scroll,
	.get_dimensions = win_get_dimensions,
	.event = win_event,
	.set_title = win_set_title,
	.set_url = win_set_url,
	.set_status = win_set_status,
};

static void misc_quit(void)
{
}

static struct gui_misc_table misc_table = {
	.schedule = framebuffer_schedule,
	.quit = misc_quit,
};

/* libfat paths are "sd:/x/y"; file URLs carry them as file:///sd/x/y. */
static nserror wii_nsurl_to_path(struct nsurl *url, char **path_out)
{
	lwc_string *upath = nsurl_get_component(url, NSURL_PATH);
	if (!upath)
		return NSERROR_BAD_PARAMETER;
	const char *p = lwc_string_data(upath);
	size_t len = lwc_string_length(upath);
	const char *slash = len > 1 ? memchr(p + 1, '/', len - 1) : NULL;
	size_t dev = slash ? (size_t)(slash - p - 1) : len - 1;
	char *out = malloc(len + 3);
	if (out)
		snprintf(out, len + 3, "%.*s:%.*s", (int)dev, p + 1, (int)(len - 1 - dev), p + 1 + dev);
	lwc_string_unref(upath);
	if (!out)
		return NSERROR_NOMEM;
	*path_out = out;
	return NSERROR_OK;
}

static nserror wii_path_to_nsurl(const char *path, struct nsurl **url_out)
{
	char buf[1024];
	if (!path)
		return NSERROR_NOT_FOUND; /* fetch.c passes NULL for a missing resource file */
	const char *colon = strchr(path, ':');
	if (colon && colon - path < 8)
		snprintf(buf, sizeof buf, "file:///%.*s%s", (int)(colon - path), path, colon + 1);
	else
		snprintf(buf, sizeof buf, "file://%s", path);
	return nsurl_create(buf, url_out);
}

static struct gui_file_table file_table;

static nserror set_defaults(struct nsoption_s *defaults)
{
	nsoption_setnull_charp(cookie_file, strdup(RES_DIR "/Cookies"));
	nsoption_setnull_charp(cookie_jar, strdup(RES_DIR "/Cookies"));
	/* Keep NetSurf's caches inside its share of memory. */
	nsoption_set_int(memory_cache_size, 4 * 1024 * 1024);
	nsoption_set_bool(enable_javascript, false);
	return NSERROR_OK;
}

int webview_init(void)
{
	if (g.ready)
		return 0;
	char *argv[] = {"iachannel", NULL};
	int argc = 1;
	file_table = *default_file_table;
	file_table.nsurl_to_path = wii_nsurl_to_path;
	file_table.path_to_nsurl = wii_path_to_nsurl;
	/* netsurf_register keeps the pointer. */
	static struct netsurf_table table;
	table.misc = &misc_table;
	table.window = &window_table;
	table.file = &file_table;
	table.fetch = framebuffer_fetch_table;
	table.utf8 = framebuffer_utf8_table;
	table.bitmap = framebuffer_bitmap_table;
	table.layout = framebuffer_layout_table;
	if (netsurf_register(&table) != NSERROR_OK)
		return -1;
	respaths = fb_init_resource_path(RES_DIR);
	nslog_init(NULL, &argc, argv);
	if (nsoption_init(set_defaults, &nsoptions, &nsoptions_default) != NSERROR_OK)
		return -1;
	char *messages = filepath_find(respaths, "Messages");
	if (!messages || messages_add_from_file(messages) != NSERROR_OK)
		WL_LOG_W("WEB", "NetSurf Messages missing from " RES_DIR);
	free(messages);
	/* libnsfb reads bitmap pixels as native 0xAABBGGRR words. The core's default is byte-wise RGBA,
	 * which is the same thing only on little-endian hosts; on the Wii it put alpha in the red
	 * channel (images came out bright red). */
	bitmap_set_format(&(bitmap_fmt_t){.layout = BITMAP_LAYOUT_ABGR8888, .pma = false});
	if (netsurf_init(NULL) != NSERROR_OK)
		return -1;
	if (wb_fetch_register() != 0)
		return -1;
	nsoption_set_bool(core_select_menu, true);
	g.fb = framebuffer_initialise("ram", PAGE_W, SURF_H, 16);
	if (!g.fb || !fb_font_init())
		return -1;
	g.tex_buf = memalign(32, PAGE_W * SURF_H * 2);
	if (!g.tex_buf)
		return -1;
	g.ready = 1;
	WL_LOG_I("WEB", "NetSurf ready");
	return 0;
}

int webview_open(const char *url, const char *timestamp)
{
	if (webview_init() != 0)
		return -1;
	webview_close();
	wb_fetch_set_timestamp(timestamp);
	nsurl *u;
	if (nsurl_create(url, &u) != NSERROR_OK)
		return -1;
	nserror err = browser_window_create(BW_CREATE_HISTORY, u, NULL, NULL, &g.bw);
	nsurl_unref(u);
	snprintf(g.url, sizeof g.url, "%s", url);
	g.title[0] = '\0';
	g.scroll = g.scroll_target = 0;
	g.dirty = 1;
	g.bytes = 0;
	g.caret = 0;
	g.scroll_x = g.scroll_x_target = 0;
	drag_reset(&g.drag);
	g.shape = GUI_POINTER_DEFAULT;
	if (g.scale <= 0)
		g.scale = k_scales[SCALE_DEFAULT];
	if (err == NSERROR_OK && g.scale != 1.0f)
		browser_window_set_scale(g.bw, g.scale, true);
	WL_LOG_I("WEB", "open %s @ %s", url, timestamp);
	return err == NSERROR_OK ? 0 : -1;
}

void webview_close(void)
{
	if (g.bw) {
		browser_window_destroy(g.bw);
		g.bw = NULL;
		for (int i = 0; i < 20; i++)
			schedule_run();
	}
}

int webview_loading(void)
{
	return g.win && g.win->loading;
}

const char *webview_url(void)
{
	return g.url;
}

const char *webview_title(void)
{
	return g.title;
}

void webview_note_bytes(size_t n)
{
	g.bytes += n;
}

void webview_status(char *out, size_t cap)
{
	int n = wb_fetch_active();
	/* Requests held back by a 429/503 right now: say so, or the page just looks stuck. */
	struct http_stats hs;
	http_get_stats(&hs);
	if (n > 0 && hs.waiting > 0)
		snprintf(out, cap, "web.archive.org is limiting requests (too many at once). Waiting and retrying...");
	else if (n > 0)
		snprintf(out, cap, "Loading %d file%s from the Wayback Machine... (%u KB)", n, n == 1 ? "" : "s",
			(unsigned)(g.bytes / 1024));
	else if (g.hover[0])
		snprintf(out, cap, "%s", g.hover);
	else
		snprintf(out, cap, "%s", g.url);
}

/* nsfb RGB565 (native big-endian) -> GX RGB565 4x4 tiles. */
static void upload(int h)
{
	uint8_t *ptr;
	int linelen;
	nsfb_get_buffer(g.fb, &ptr, &linelen);
	uint16_t *d = g.tex_buf;
	for (int ty = 0; ty < h; ty += 4)
		for (int tx = 0; tx < PAGE_W; tx += 4)
			for (int r = 0; r < 4; r++) {
				const uint16_t *row = (const uint16_t *)(ptr + (size_t)(ty + r) * linelen) + tx;
				*d++ = row[0];
				*d++ = row[1];
				*d++ = row[2];
				*d++ = row[3];
			}
	wl_gpu_buffer_commit(g.tex_buf, (size_t)PAGE_W * (size_t)h * 2);
	wl_gx_tex_init(&g.tex, g.tex_buf, PAGE_W, (uint32_t)h, WL_GX_TEX_RGB565, false, true);
	wl_gx_tex_invalidate();
}

static void render(void)
{
	struct redraw_context ctx = {.interactive = true, .background_images = true, .plot = &fb_plotters};
	struct rect clip = {0, 0, PAGE_W, g.view_h};
	nsfb_bbox_t box = {0, 0, PAGE_W, g.view_h};
	nsfb_claim(g.fb, &box);
	nsfb_plot_rectangle_fill(g.fb, &box, 0xFFFFFFFF); /* white behind pages without a background */
	browser_window_redraw(g.bw, -(int)g.scroll_x, -(int)g.scroll, &clip, &ctx);
	nsfb_update(g.fb, &box);
	upload(g.view_h);
}

void webview_frame(float ax, float ay, float aw, float ah, const struct input_state *in, int *want_close)
{
	struct { float x, y, w, h; } area = {ax, ay, aw, ah};
	*want_close = 0;
	schedule_run(); /* NetSurf's scheduler: fetch polling, layout, image decoding */
	if (!g.bw)
		return;

	/* Page pixels shown: the area's aspect at PAGE_W wide (multiple of 4 for GX tiles). */
	int vh = (int)((float)PAGE_W * area.h / area.w) & ~3;
	if (vh > SURF_H)
		vh = SURF_H;
	if (vh != g.view_h) {
		g.view_h = vh;
		g.dirty = 1;
	}
	browser_window_get_extents(g.bw, true, &g.doc_w, &g.doc_h);
	float max_scroll = (float)(g.doc_h - g.view_h) > 0 ? (float)(g.doc_h - g.view_h) : 0;
	float max_x = (float)(g.doc_w - PAGE_W) > 0 ? (float)(g.doc_w - PAGE_W) : 0;
	float page_per_ui = (float)PAGE_W / area.w;

	/* B: hold it and move the pointer to drag the page (it follows the hand, both ways). Tapped,
	 * it goes back instead (below). */
	float dx, dy;
	int b = drag_update(&g.drag, in, wl_platform_time_us(), &dx, &dy);
	g.scroll_x_target -= dx * page_per_ui;
	g.scroll_target -= dy * page_per_ui;

	/* Scrolling: D-pad, pointer at the top/bottom edges. */
	float step = 18;
	if (in->held & BTN_DOWN)
		g.scroll_target += step;
	if (in->held & BTN_UP)
		g.scroll_target -= step;
	if (in->held & BTN_RIGHT)
		g.scroll_x_target += step;
	if (in->held & BTN_LEFT)
		g.scroll_x_target -= step;
	if (!g.drag.active && in->pointer_valid && in->px >= area.x && in->px < area.x + area.w) {
		if (in->py > area.y + area.h - 26 && in->py < area.y + area.h)
			g.scroll_target += 10;
		if (in->py < area.y + 26 && in->py >= area.y)
			g.scroll_target -= 10;
	}
	g.scroll_target = g.scroll_target < 0 ? 0 : g.scroll_target > max_scroll ? max_scroll : g.scroll_target;
	g.scroll_x_target = g.scroll_x_target < 0 ? 0 : g.scroll_x_target > max_x ? max_x : g.scroll_x_target;
	float before = g.scroll, before_x = g.scroll_x;
	g.scroll += (g.scroll_target - g.scroll) * 0.35f;
	g.scroll_x += (g.scroll_x_target - g.scroll_x) * 0.35f;
	if (g.drag.active || (g.scroll - g.scroll_target < 0.5f && g.scroll_target - g.scroll < 0.5f))
		g.scroll = g.scroll_target;
	if (g.drag.active || (g.scroll_x - g.scroll_x_target < 0.5f && g.scroll_x_target - g.scroll_x < 0.5f))
		g.scroll_x = g.scroll_x_target;
	int moved = (int)g.scroll != (int)before || (int)g.scroll_x != (int)before_x;
	if (moved)
		g.dirty = 1;

	/* Pointer: hover for the status line, A clicks links. */
	if (in->pointer_valid && in->px >= area.x && in->py >= area.y && in->px < area.x + area.w &&
		in->py < area.y + area.h) {
		int px = (int)((in->px - area.x) * page_per_ui + g.scroll_x);
		int py = (int)((in->py - area.y) * page_per_ui + g.scroll);
		if (in->pressed & BTN_A) {
			browser_window_mouse_click(g.bw, BROWSER_MOUSE_PRESS_1, px, py);
			g.pressed = 1;
		} else if ((in->released & BTN_A) && g.pressed) {
			browser_window_mouse_click(g.bw, BROWSER_MOUSE_CLICK_1, px, py);
			g.pressed = 0;
		} else if (in->pointer_moved || moved) {
			/* Also when the page scrolls under a still pointer: link and pointer shape. */
			browser_window_mouse_track(g.bw, BROWSER_MOUSE_HOVER, px, py);
		}
	} else if (!in->pointer_valid) {
		g.hover[0] = '\0';
	}

	/* B tapped: back through history, or leave. */
	if (b == DRAG_TAP) {
		if (browser_window_history_back_available(g.bw))
			browser_window_history_back(g.bw, false);
		else
			*want_close = 1;
	}
	/* +/-: zoom. 2: zoom in, cycling back to the smallest size. */
	if (in->pressed & BTN_PLUS)
		webview_zoom(1);
	if (in->pressed & BTN_MINUS)
		webview_zoom(-1);
	if (in->pressed & BTN_2)
		webview_zoom(g.scale >= k_scales[sizeof k_scales / sizeof k_scales[0] - 1] ? -99 : 1);

	if (g.dirty && g.view_h > 0) {
		render();
		g.dirty = 0;
	}
}

const void *webview_texture(float aw, float *draw_h, float *bar_pos, float *bar_size)
{
	if (!g.bw || g.view_h <= 0 || !g.tex_buf)
		return NULL;
	*draw_h = (float)g.view_h * aw / (float)PAGE_W;
	*bar_size = g.doc_h > 0 ? (float)g.view_h / (float)g.doc_h : 1;
	*bar_pos = g.doc_h > 0 ? g.scroll / (float)g.doc_h : 0;
	return &g.tex;
}

void webview_hbar(float *pos, float *size)
{
	*size = g.doc_w > PAGE_W ? (float)PAGE_W / (float)g.doc_w : 1;
	*pos = g.doc_w > 0 ? g.scroll_x / (float)g.doc_w : 0;
}

int webview_can_back(void)
{
	return g.bw && browser_window_history_back_available(g.bw);
}

int webview_can_forward(void)
{
	return g.bw && browser_window_history_forward_available(g.bw);
}

void webview_back(void)
{
	if (webview_can_back())
		browser_window_history_back(g.bw, false);
}

void webview_forward(void)
{
	if (webview_can_forward())
		browser_window_history_forward(g.bw, false);
}

int webview_zoom(int steps)
{
	int n = (int)(sizeof k_scales / sizeof k_scales[0]), i = SCALE_DEFAULT;
	for (int k = 0; k < n; k++)
		if (k_scales[k] == g.scale)
			i = k;
	i += steps;
	i = i < 0 ? 0 : i >= n ? n - 1 : i;
	g.scale = k_scales[i];
	if (g.bw) {
		/* Keep the same part of the page in view. */
		float ratio = g.scale / browser_window_get_scale(g.bw);
		browser_window_set_scale(g.bw, g.scale, true);
		g.scroll *= ratio;
		g.scroll_target *= ratio;
		g.scroll_x *= ratio;
		g.scroll_x_target *= ratio;
		g.dirty = 1;
	}
	return (int)(g.scale * 100.0f + 0.5f);
}

int webview_zoom_percent(void)
{
	return (int)((g.scale > 0 ? g.scale : k_scales[SCALE_DEFAULT]) * 100.0f + 0.5f);
}

int webview_dragging(void)
{
	return g.drag.active;
}

int webview_over_link(void)
{
	return g.shape == GUI_POINTER_POINT || g.shape == GUI_POINTER_CARET || g.shape == GUI_POINTER_MENU;
}

int webview_text_focus(void)
{
	return g.bw && g.caret;
}

/* Replaces the focused field's text: select all, delete, type, then Enter (submits a form). */
void webview_type(const char *utf8, int submit)
{
	if (!g.bw)
		return;
	browser_window_key_press(g.bw, NS_KEY_SELECT_ALL);
	browser_window_key_press(g.bw, NS_KEY_DELETE_LEFT);
	const unsigned char *p = (const unsigned char *)utf8;
	while (*p) {
		uint32_t c = *p++;
		if (c >= 0xC0) {
			int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
			c &= 0x3F >> more;
			while (more-- && (*p & 0xC0) == 0x80)
				c = c << 6 | (*p++ & 0x3F);
		}
		browser_window_key_press(g.bw, c);
	}
	if (submit)
		browser_window_key_press(g.bw, NS_KEY_NL);
	g.dirty = 1;
}
