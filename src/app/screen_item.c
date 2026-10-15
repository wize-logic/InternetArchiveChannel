/* Item page: metadata, thumbnail, description and what the channel can do with it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/app.h"
#include "ia/ia.h"
#include "net/http.h"

enum { ID_PRIMARY = 1, ID_BACK, ID_RETRY, ID_DESC_UP, ID_DESC_DOWN };

static struct {
	char id[IA_ID_MAX];
	http_req *req;
	struct ia_item item;
	int loaded, error;
	char error_msg[96];
	int video, video_fast, tracks[256], track_count;
	float desc_scroll;
	char notice[160];
	int notice_frames;
} g;

static void enter(void *arg)
{
	if (g.req)
		http_release(g.req);
	ia_item_free(&g.item);
	memset(&g, 0, sizeof g);
	snprintf(g.id, sizeof g.id, "%s", arg ? (const char *)arg : "");
	g.video = -1;
}

static void poll_request(void)
{
	if (!g.req && !g.loaded && !g.error && net_state() == NET_READY) {
		char url[512];
		ia_metadata_url(url, sizeof url, g.id);
		struct http_opts o;
		http_opts_default(&o);
		o.priority = 10;
		o.max_body = 8u << 20; /* some items list thousands of files */
		g.req = http_get(url, &o);
	}
	if (!g.req || http_state(g.req) == HTTP_PENDING)
		return;
	const struct http_result *r = http_result(g.req);
	if (http_state(g.req) == HTTP_DONE && r->status == 200 &&
		ia_parse_item((const char *)r->body, &g.item) == 0) {
		g.loaded = 1;
		if (g.item.media == IA_MEDIA_COLLECTION) {
			/* A collection: show its contents instead of an empty item page. */
			static struct results_args args;
			snprintf(args.title, sizeof args.title, "%s", g.item.title);
			snprintf(args.query, sizeof args.query, "collection:%s", g.item.id);
			args.sort = 0;
			http_release(g.req);
			g.req = NULL;
			app_pop();
			app_push(&screen_results, &args);
			return;
		}
		g.video = ia_pick_video(&g.item, &g.video_fast);
		g.track_count = ia_list_audio(&g.item, g.tracks, 256);
		WL_LOG_I("ITEM", "%s: %d files, video %d, %d tracks (%u ms, %u KB)", g.id, g.item.file_count, g.video,
			g.track_count, r->elapsed_ms, (unsigned)(r->body_len / 1024));
	} else {
		g.error = 1;
		snprintf(g.error_msg, sizeof g.error_msg, "%s",
			http_state(g.req) == HTTP_FAILED ? http_strerror(r->error) : "The item couldn't be read.");
	}
	http_release(g.req);
	g.req = NULL;
}

static void format_length(float secs, char *out, size_t cap)
{
	int s = (int)secs;
	if (s >= 3600)
		snprintf(out, cap, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(out, cap, "%d:%02d", s / 60, s % 60);
}

/* What the main button does, and a line describing the chosen file(s). */
static const char *primary_action(char *info, size_t cap)
{
	info[0] = '\0';
	if (g.item.restricted) {
		snprintf(info, cap, "This item can only be borrowed on archive.org.");
		return NULL;
	}
	if (g.video >= 0) {
		const struct ia_file *f = &g.item.files[g.video];
		char len[16] = "";
		if (f->length > 0)
			format_length(f->length, len, sizeof len);
		if (f->width)
			snprintf(info, cap, "Video: %s, %dx%d%s%s", f->format, f->width, f->height, len[0] ? ", " : "", len);
		else
			snprintf(info, cap, "Video: %s%s%s", f->format, len[0] ? ", " : "", len);
		if (ia_file_kbps(f) > IA_VIDEO_WARN_KBPS) {
			size_t used = strlen(info);
			snprintf(info + used, cap - used, " - high bitrate, may buffer");
		}
		return "Watch";
	}
	if (g.track_count > 0) {
		snprintf(info, cap, "%d track%s (%s)", g.track_count, g.track_count == 1 ? "" : "s",
			g.item.files[g.tracks[0]].format);
		return "Listen";
	}
	if (g.item.media == IA_MEDIA_TEXTS && ia_has_book_pages(&g.item)) {
		if (g.item.imagecount > 0)
			snprintf(info, cap, "%d pages", g.item.imagecount);
		else
			snprintf(info, cap, "Book (page images%s)", ia_find_text(&g.item) >= 0 ? " and text" : "");
		return "Read";
	}
	if (g.item.media == IA_MEDIA_TEXTS && ia_find_text(&g.item) >= 0) {
		snprintf(info, cap, "Text only (no page images)");
		return "Read";
	}
	if (g.item.media == IA_MEDIA_IMAGE) {
		int idx[VIEWER_MAX_FILES];
		int n = ia_list_images(&g.item, idx, VIEWER_MAX_FILES);
		if (n > 0) {
			snprintf(info, cap, "%d image%s", n, n == 1 ? "" : "s");
			return "View";
		}
	}
	snprintf(info, cap, "Nothing in this item plays on the Wii yet.");
	return NULL;
}

static void frame(void)
{
	poll_request();
	struct rect c = app_chrome(g.loaded ? "Item" : "Loading...", NULL);
	if (app_back()) {
		app_pop();
		return;
	}
	if (!g.loaded) {
		if (g.error) {
			font_draw_wrapped(theme.body, c.x, c.y + 20, (int)c.w, theme.text, g.error_msg);
			if (ui_button(ID_RETRY, (struct rect){c.x, c.y + 80, 180, 46}, "Try again"))
				g.error = 0;
		} else {
			app_spinner(c, net_state() == NET_FAILED ? "No network connection." : "Loading item...");
		}
		ui_hints("");
		return;
	}

	/* Left: thumbnail. Right: title, creator/date, file info, buttons, description. */
	float left_w = c.w * 0.32f;
	struct rect thumb = {c.x, c.y, left_w, left_w * 0.75f};
	draw_rect(thumb, theme.panel);
	const struct image *img = thumb_get(g.item.id);
	if (img)
		ui_draw_image_fit((struct rect){thumb.x + 4, thumb.y + 4, thumb.w - 8, thumb.h - 8}, img, 0xFFFFFFFFu);

	float x = c.x + left_w + 20, w = c.w - left_w - 20, y = c.y - 4;
	/* Title: up to two lines. */
	draw_clip((struct rect){x, y, w, (float)font_line_height(theme.title) * 2});
	font_draw_wrapped(theme.title, x, y, (int)w, theme.text, g.item.title);
	draw_clip_reset();
	int title_h = font_measure(theme.title, g.item.title, -1) > (int)w ? 2 : 1;
	y += (float)(title_h * font_line_height(theme.title)) + 2;
	char meta[256];
	snprintf(meta, sizeof meta, "%s%s%.10s", g.item.creator, g.item.creator[0] && g.item.date[0] ? " - " : "",
		g.item.date);
	font_draw_clipped(theme.small, x, y, theme.text_dim, meta, (int)w);
	y += (float)font_line_height(theme.small) + 8;

	char info[160];
	const char *action = primary_action(info, sizeof info);
	if (info[0]) {
		font_draw_clipped(theme.small, x, y, theme.text, info, (int)w);
		y += (float)font_line_height(theme.small) + 8;
	}
	float bx = x;
	if (action) {
		if (ui_button(ID_PRIMARY, (struct rect){bx, y, 170, 46}, action)) {
			struct nowplaying *np = nowplaying();
			if (g.video >= 0 || g.track_count > 0) {
				nowplaying_stop();
				memset(np, 0, sizeof *np);
				snprintf(np->item_id, sizeof np->item_id, "%s", g.item.id);
				snprintf(np->item_title, sizeof np->item_title, "%s", g.item.title);
				snprintf(np->creator, sizeof np->creator, "%s", g.item.creator);
				if (g.video >= 0) {
					const struct ia_file *f = &g.item.files[g.video];
					np->is_video = 1;
					np->fast_video = app_settings.fast_h264 && g.video_fast;
					np->count = 1;
					snprintf(np->tracks[0].title, sizeof np->tracks[0].title, "%s", g.item.title);
					ia_file_url(np->tracks[0].url, sizeof np->tracks[0].url, g.item.id, f->name);
					np->tracks[0].length = f->length;
					nowplaying_play(0);
					app_push(&screen_video, NULL);
				} else {
					for (int i = 0; i < g.track_count && i < NP_MAX_TRACKS; i++) {
						const struct ia_file *f = &g.item.files[g.tracks[i]];
						struct np_track *t = &np->tracks[np->count++];
						snprintf(t->title, sizeof t->title, "%s", f->title[0] ? f->title : f->name);
						ia_file_url(t->url, sizeof t->url, g.item.id, f->name);
						t->length = f->length;
					}
					nowplaying_play(0);
					app_push(&screen_player, NULL);
				}
			} else {
				static char files[VIEWER_MAX_FILES][192];
				static struct viewer_args va;
				memset(&va, 0, sizeof va);
				snprintf(va.title, sizeof va.title, "%s", g.item.title);
				snprintf(va.id, sizeof va.id, "%s", g.item.id);
				va.files = files;
				int pages = ia_has_book_pages(&g.item), t = ia_find_text(&g.item);
				if (g.item.media == IA_MEDIA_TEXTS && (pages || t >= 0)) {
					/* A book: page images (count from the page list when there is one), its
					 * text, or both. */
					va.is_book = 1;
					va.count = pages ? g.item.imagecount : 0;
					int pc = pages ? ia_find_page_count_file(&g.item) : -1;
					if (pc >= 0)
						ia_file_url(va.count_url, sizeof va.count_url, g.item.id, g.item.files[pc].name);
					if (t >= 0)
						ia_file_url(va.text_url, sizeof va.text_url, g.item.id, g.item.files[t].name);
				} else {
					int idx[VIEWER_MAX_FILES];
					va.count = ia_list_images(&g.item, idx, VIEWER_MAX_FILES);
					for (int i = 0; i < va.count; i++)
						snprintf(files[i], sizeof files[i], "%s", g.item.files[idx[i]].name);
				}
				if (va.count > 0 || va.count_url[0] || va.text_url[0]) {
					app_push(&screen_viewer, &va);
				} else {
					snprintf(g.notice, sizeof g.notice, "No images found in this item.");
					g.notice_frames = 180;
				}
			}
		}
		bx += 184;
	}
	y += 58;

	/* Description fills the rest; UP/DOWN scroll it when it's longer than the space. */
	struct rect desc = {c.x, thumb.y + thumb.h + 12 > y ? thumb.y + thumb.h + 12 : y, c.w, 0};
	desc.h = c.y + c.h - desc.y;
	if (g.item.description && desc.h > 30) {
		draw_clip(desc);
		int h = font_draw_wrapped(theme.small, desc.x, desc.y - g.desc_scroll, (int)desc.w, theme.text_dim,
			g.item.description);
		draw_clip_reset();
		float max_scroll = (float)h - desc.h;
		if (max_scroll > 0) {
			const struct input_state *in = ui_input();
			if ((in->held & BTN_2) || (in->pointer_valid && in->py > desc.y + desc.h - 24 && in->py < desc.y + desc.h))
				g.desc_scroll += 3;
			if ((in->held & BTN_1) || (in->pointer_valid && in->py >= desc.y && in->py < desc.y + 24))
				g.desc_scroll -= 3;
			if (g.desc_scroll < 0)
				g.desc_scroll = 0;
			if (g.desc_scroll > max_scroll)
				g.desc_scroll = max_scroll;
		}
	}

	if (g.notice_frames > 0) {
		g.notice_frames--;
		float nw = (float)font_measure(theme.body, g.notice, -1) + 40;
		struct rect n = {((float)draw_width() - nw) / 2, UI_H - 120, nw, 46};
		draw_rect(n, RGBA(0x10, 0x10, 0x12, 0xF0));
		draw_rect_outline(n, 2, theme.accent);
		font_draw(theme.body, n.x + 20, n.y + 10, theme.text, g.notice, -1);
	}
	ui_hints("(A) Select    (1)/(2) Scroll description");
}

const struct screen screen_item = {"item", enter, frame};
