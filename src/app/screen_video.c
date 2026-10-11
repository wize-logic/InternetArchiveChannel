/* Full-screen video. Frames from the player are uploaded as three I8 planes and converted by the
 * GPU (src/gfx/yuv.c, validated in Spike 2); the controls appear on any input and fade out. */
#include <stdio.h>
#include <string.h>

#include "app/app.h"
#include "gfx/yuv.h"
#include "media/player.h"

enum { ID_PLAY = 1, ID_BACK30, ID_FWD30, ID_BACK, ID_SEEKBAR };

static struct {
	struct yuv_tex tex;
	int tex_w, tex_h, have_tex;
	uint32_t serial;
	int osd_frames;
} g;

static void enter(void *arg)
{
	(void)arg;
	g.serial = 0;
	g.osd_frames = 180;
	ui_focus(ID_PLAY);
}

static void leave(void)
{
	nowplaying_stop();
	if (g.have_tex)
		yuv_tex_free(&g.tex);
	g.have_tex = 0;
	app_pop();
}

static void draw_video(void)
{
	const struct player_frame *f = player_video_frame();
	if (!f)
		return;
	if (!g.have_tex || f->width != g.tex_w || f->height != g.tex_h) {
		if (g.have_tex)
			yuv_tex_free(&g.tex);
		g.have_tex = yuv_tex_init(&g.tex, f->width, f->height, 0) == 0;
		g.tex_w = f->width;
		g.tex_h = f->height;
		if (!g.have_tex)
			return;
	}
	if (f->serial != g.serial) {
		yuv_tex_upload(&g.tex, f->planes, f->linesize);
		g.serial = f->serial;
	}
	/* Fit the canvas, keeping the aspect ratio (square pixels assumed). */
	float cw = (float)draw_width(), ch = UI_H;
	float sc = cw / (float)f->width < ch / (float)f->height ? cw / (float)f->width : ch / (float)f->height;
	float w = (float)f->width * sc, h = (float)f->height * sc;
	font_flush();
	yuv_begin(&g.tex);
	yuv_draw(&g.tex, (cw - w) / 2, (ch - h) / 2, w, h);
	wl_gx_invalidate_shadow(); /* raw GX state above: WiiLibs re-sends its own */
}

static void frame(void)
{
	struct nowplaying *np = nowplaying();
	draw_rect((struct rect){0, 0, (float)draw_width(), UI_H}, RGBA(0, 0, 0, 0xFF));
	draw_video();

	const struct input_state *in = ui_input();
	if (in->pressed || in->pointer_moved || in->text_len)
		g.osd_frames = 180;
	if (ui_pressed(BTN_B)) {
		leave();
		return;
	}
	enum player_state st = player_get_state();
	if (st == PL_ENDED) {
		leave();
		return;
	}
	if (st == PL_LOADING || st == PL_ERROR) {
		struct rect all = {0, 0, (float)draw_width(), UI_H};
		app_spinner(all, st == PL_ERROR ? player_error() : "Loading video...");
		if (st == PL_ERROR && ui_button(ID_BACK, (struct rect){all.w / 2 - 80, UI_H - 120, 160, 46}, "Back"))
			leave();
		return;
	}
	/* Quick controls without the OSD: A pauses, left/right seek. */
	if (g.osd_frames == 0) {
		if (ui_pressed(BTN_A))
			player_pause(st != PL_PAUSED);
		if (ui_pressed(BTN_RIGHT))
			player_seek(player_position() + 30);
		if (ui_pressed(BTN_LEFT))
			player_seek(player_position() - 30);
		return;
	}
	g.osd_frames--;

	struct rect s = ui_safe_area();
	float h = 130;
	struct rect osd = {0, s.y + s.h - h, (float)draw_width(), h + (UI_H - s.y - s.h)};
	draw_rect_vgradient(osd, RGBA(0, 0, 0, 0x40), RGBA(0, 0, 0, 0xD0));
	font_draw_clipped(theme.body, s.x, osd.y + 6, theme.text, np->item_title, (int)s.w);
	double pos = player_position(), dur = player_duration();
	struct rect bar = {s.x, osd.y + 40, s.w, 14};
	if (ui_region(ID_SEEKBAR, bar) && dur > 0 && in->pointer_valid)
		player_seek(dur * (double)((in->px - bar.x) / bar.w));
	draw_rect(bar, RGBA(0x30, 0x30, 0x30, 0xE0));
	if (dur > 0)
		draw_rect((struct rect){bar.x, bar.y, bar.w * (float)(pos / dur), bar.h}, theme.accent);
	if (ui_get_focus() == ID_SEEKBAR)
		draw_rect_outline(bar, 2, theme.text);
	char t1[24], t2[24], times[64];
	nowplaying_format_time(pos, t1, sizeof t1);
	nowplaying_format_time(dur, t2, sizeof t2);
	snprintf(times, sizeof times, "%s / %s%s", t1, t2,
		st == PL_PLAYING && player_buffer_level() < 0.02f && pos < dur - 2 ? "   Buffering..." : "");
	font_draw(theme.small, s.x, bar.y + 18, theme.text_dim, times, -1);
	float bw = 100, bh = 40, gap = 10, bx = s.x + s.w - 4 * bw - 3 * gap, by = bar.y + 18;
	if (ui_button(ID_BACK30, (struct rect){bx, by, bw, bh}, "-30s"))
		player_seek(pos - 30);
	if (ui_button(ID_PLAY, (struct rect){bx + bw + gap, by, bw, bh}, st == PL_PAUSED ? "Play" : "Pause"))
		player_pause(st != PL_PAUSED);
	if (ui_button(ID_FWD30, (struct rect){bx + 2 * (bw + gap), by, bw, bh}, "+30s"))
		player_seek(pos + 30);
	if (ui_button(ID_BACK, (struct rect){bx + 3 * (bw + gap), by, bw, bh}, "Close"))
		leave();
}

const struct screen screen_video = {"video", enter, frame};
