/* Browsing an archived site: the NetSurf page view (src/web/webview.c) under an address strip
 * with back/forward, zoom and bookmark buttons. Clicking a text field opens the keyboard; its
 * text is typed into the field and the form submitted (forms come back as GET queries). */
#include <stdio.h>
#include <string.h>

#include "app/app.h"
#include "ui/osk.h"
#include "web/webview.h"

enum { ID_BACK = 1, ID_FORWARD, ID_ZOOM_OUT, ID_ZOOM_IN, ID_BOOKMARK };

static struct web_args g_args;
static int g_failed;
static int g_typing, g_had_caret;
static char g_note[64];
static uint64_t g_note_until;

static void note(const char *text)
{
	snprintf(g_note, sizeof g_note, "%s", text);
	g_note_until = wl_platform_time_us() + 2500000;
}

static void enter(void *arg)
{
	if (arg)
		g_args = *(const struct web_args *)arg;
	g_failed = webview_open(g_args.url, g_args.timestamp) != 0;
	g_typing = g_had_caret = 0;
	g_note[0] = '\0';
}

static void toolbar(struct rect s, float strip, float *right_edge)
{
	float bh = strip - 8, bw = 44, gap = 6;
	float x = s.x + s.w - (bw * 4 + gap * 4 + 96);
	*right_edge = x - 10;
	float y = s.y + 2;
	if (ui_button(ID_BACK, (struct rect){x, y, bw, bh}, "<"))
		webview_back();
	x += bw + gap;
	if (ui_button(ID_FORWARD, (struct rect){x, y, bw, bh}, ">"))
		webview_forward();
	x += bw + gap;
	if (ui_button(ID_ZOOM_OUT, (struct rect){x, y, bw, bh}, "-"))
		webview_zoom(-1);
	x += bw + gap;
	if (ui_button(ID_ZOOM_IN, (struct rect){x, y, bw, bh}, "+"))
		webview_zoom(1);
	x += bw + gap;
	const char *url = webview_url();
	int marked = bookmarks_find(url, g_args.timestamp) >= 0;
	if (ui_button(ID_BOOKMARK, (struct rect){x, y, 96, bh}, marked ? "Saved" : "Save")) {
		int now = bookmarks_toggle(url, g_args.timestamp, webview_title());
		note(now ? "Bookmarked" : "Bookmark removed");
	}
}

static void frame(void)
{
	draw_rect((struct rect){0, 0, (float)draw_width(), UI_H}, RGBA(0x1a, 0x1b, 0x1e, 0xFF));
	struct rect s = ui_safe_area();
	if (g_failed) {
		app_spinner(s, "The page viewer couldn't start (NetSurf resources missing from the SD card?)");
		if (ui_pressed(BTN_B))
			app_pop();
		return;
	}

	/* Address strip: icon, title or URL, date, then the buttons. */
	char date[32] = "";
	if (strlen(g_args.timestamp) >= 8)
		snprintf(date, sizeof date, "%.4s-%.2s-%.2s", g_args.timestamp, g_args.timestamp + 4, g_args.timestamp + 6);
	float strip = (float)font_line_height(theme.body) + 18;
	draw_rect((struct rect){0, 0, (float)draw_width(), s.y + strip}, RGBA(0x10, 0x10, 0x12, 0xFF));
	if (art_icon(ICON_WAYBACK))
		ui_draw_image_fit((struct rect){s.x, s.y + 4, strip - 10, strip - 10}, art_icon(ICON_WAYBACK), 0xFFFFFFFFu);
	float right;
	toolbar(s, strip, &right);
	int dw = font_measure(theme.body, date, -1);
	font_draw(theme.body, right - (float)dw, s.y + 7, theme.accent, date, -1);
	const char *title = webview_title()[0] ? webview_title() : webview_url();
	font_draw_clipped(theme.body, s.x + strip, s.y + 7, theme.text, title,
		(int)(right - (float)dw - 16 - s.x - strip));

	float status_h = (float)font_line_height(theme.small) + 6;
	struct rect page = {s.x, s.y + strip, s.w, s.h - strip - status_h};
	int want_close = 0;
	/* While the keyboard is up the page gets no input. */
	static const struct input_state idle;
	webview_frame(page.x, page.y, page.w, page.h, g_typing ? &idle : ui_input(), &want_close);
	float draw_h, bar_pos, bar_size;
	const WlGxTexObj *tex = webview_texture(page.w, &draw_h, &bar_pos, &bar_size);
	if (tex) {
		draw_rect(page, RGBA(0xff, 0xff, 0xff, 0xff));
		draw_clip(page);
		draw_image((struct rect){page.x, page.y, page.w, draw_h}, tex, 0xFFFFFFFFu);
		draw_clip_reset();
		if (bar_size < 1)
			draw_rect((struct rect){page.x + page.w - 6, page.y + page.h * bar_pos, 6, page.h * bar_size},
				RGBA(0x40, 0x40, 0x40, 0xB0));
		float hpos, hsize;
		webview_hbar(&hpos, &hsize);
		if (hsize < 1)
			draw_rect((struct rect){page.x + page.w * hpos, page.y + page.h - 6, page.w * hsize, 6},
				RGBA(0x40, 0x40, 0x40, 0xB0));
	} else {
		app_spinner(page, "Loading from the Wayback Machine...");
	}

	/* Pointer: the grabbing hand while B drags, the clock while the page loads, the hand over
	 * links, the open hand elsewhere on the page. The toolbar gets the usual hands. */
	const struct input_state *in = ui_input();
	if (!g_typing && in->pointer_valid && rect_contains(page, in->px, in->py)) {
		if (webview_dragging())
			ui_set_cursor(UI_CURSOR_GRAB);
		else if (webview_loading())
			ui_set_cursor(UI_CURSOR_WAIT);
		else
			ui_set_cursor(webview_over_link() ? UI_CURSOR_HAND : UI_CURSOR_NORMAL);
	}

	char status[512];
	int zoom = webview_zoom_percent();
	if (g_note[0] && wl_platform_time_us() < g_note_until)
		snprintf(status, sizeof status, "%s", g_note);
	else if (zoom != 100 && !webview_loading())
		snprintf(status, sizeof status, "Zoom %d%%   %s", zoom, webview_url());
	else
		webview_status(status, sizeof status);
	font_draw_clipped(theme.small, s.x, page.y + page.h + 3, theme.text_dim, status, (int)s.w);

	/* A text field took the caret: ask for its text. */
	int caret = webview_text_focus();
	if (caret && !g_had_caret && !g_typing) {
		osk_open("Type into the page (Done submits)", "", 255);
		g_typing = 1;
	}
	g_had_caret = caret;
	if (g_typing) {
		int r = osk_update();
		if (r != OSK_EDITING) {
			g_typing = 0;
			if (r == OSK_DONE && osk_text()[0]) {
				WL_LOG_I("WEB", "form text entered");
				webview_type(osk_text(), 1);
			}
		}
		return;
	}

	if (want_close) {
		webview_close();
		app_pop();
	}
}

const struct screen screen_web = {"web", enter, frame};
