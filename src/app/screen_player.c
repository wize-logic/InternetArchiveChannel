/* Audio player: track list, transport controls, seek bar. B leaves it playing in the background. */
#include <stdio.h>
#include <string.h>

#include "app/app.h"
#include "media/player.h"

enum { ID_PREV = 1, ID_PLAY, ID_NEXT, ID_BACK30, ID_FWD30, ID_STOP, ID_SEEKBAR, ID_TRACK = 100 };

static float g_list_scroll;

static void enter(void *arg)
{
	(void)arg;
	g_list_scroll = 0;
	ui_focus(ID_PLAY);
}

static void frame(void)
{
	struct nowplaying *np = nowplaying();
	struct rect c = app_chrome(np->item_title, np->creator[0] ? np->creator : NULL);
	if (ui_pressed(BTN_B)) {
		app_pop(); /* keeps playing; the mini bar shows it */
		return;
	}
	enum player_state st = player_get_state();

	/* Left column: artwork. */
	float left_w = c.w * 0.30f;
	struct rect art_r = {c.x, c.y, left_w, left_w};
	draw_rect(art_r, theme.panel);
	const struct image *img = thumb_get(np->item_id);
	if (img)
		ui_draw_image_fit((struct rect){art_r.x + 6, art_r.y + 6, art_r.w - 12, art_r.h - 12}, img, 0xFFFFFFFFu);

	/* Right: track list. */
	float lx = c.x + left_w + 18, lw = c.w - left_w - 18;
	float row_h = (float)font_line_height(theme.body) + 12;
	float list_h = c.h - 112;
	int focus = ui_get_focus();
	if (focus >= ID_TRACK && focus < ID_TRACK + np->count) {
		float fy = (float)(focus - ID_TRACK) * row_h;
		if (fy < g_list_scroll)
			g_list_scroll = fy;
		if (fy + row_h > g_list_scroll + list_h)
			g_list_scroll = fy + row_h - list_h;
	}
	draw_clip((struct rect){lx, c.y, lw, list_h});
	for (int i = 0; i < np->count; i++) {
		struct rect r = {lx, c.y + (float)i * row_h - g_list_scroll, lw, row_h - 4};
		if (r.y + r.h < c.y - row_h || r.y > c.y + list_h + row_h) {
			if (ui_region(ID_TRACK + i, r))
				nowplaying_play(i);
			continue;
		}
		char label[200], len[16] = "";
		if (np->tracks[i].length > 0)
			nowplaying_format_time(np->tracks[i].length, len, sizeof len);
		snprintf(label, sizeof label, "%d. %s", i + 1, np->tracks[i].title);
		int hot = ui_get_focus() == ID_TRACK + i;
		if (ui_region(ID_TRACK + i, r))
			nowplaying_play(i);
		int playing = np->active && i == np->current;
		draw_rect(r, hot ? theme.panel_hi : playing ? RGBA(0x24, 0x3a, 0x52, 0xFF) : theme.panel);
		if (hot)
			draw_rect_outline(r, 2, theme.accent);
		int lw_len = font_measure(theme.small, len, -1);
		font_draw_clipped(theme.body, r.x + 12, r.y + 4, playing ? theme.accent : theme.text, label,
			(int)r.w - 36 - lw_len);
		font_draw(theme.small, r.x + r.w - 12 - (float)lw_len, r.y + 8, theme.text_dim, len, -1);
	}
	draw_clip_reset();

	/* Bottom: seek bar and transport. */
	float by = c.y + c.h - 100;
	double pos = player_position(), dur = player_duration();
	struct rect bar = {c.x, by, c.w, 18};
	int bar_hot = 0;
	if (ui_region(ID_SEEKBAR, bar) && dur > 0) {
		const struct input_state *in = ui_input();
		if (in->pointer_valid)
			player_seek(dur * (double)((in->px - bar.x) / bar.w));
	}
	bar_hot = ui_get_focus() == ID_SEEKBAR;
	draw_rect(bar, RGBA(0x16, 0x17, 0x1a, 0xFF));
	if (dur > 0)
		draw_rect((struct rect){bar.x, bar.y, bar.w * (float)(pos / dur), bar.h}, theme.accent);
	if (bar_hot)
		draw_rect_outline(bar, 2, theme.text);
	char t1[24], t2[24], times[64];
	nowplaying_format_time(pos, t1, sizeof t1);
	nowplaying_format_time(dur, t2, sizeof t2);
	snprintf(times, sizeof times, "%s / %s", t1, t2);
	font_draw(theme.small, c.x, by + 22, theme.text_dim, times, -1);
	const char *status = st == PL_LOADING ? "Loading..." : st == PL_ERROR ? player_error() : "";
	if (st == PL_PLAYING && player_buffer_level() < 0.05f && dur > 0 && pos < dur - 2)
		status = "Buffering...";
	if (!np->active)
		status = "Stopped";
	int sw = font_measure(theme.small, status, -1);
	font_draw(theme.small, c.x + c.w - (float)sw, by + 22, st == PL_ERROR ? theme.danger : theme.text_dim, status, -1);

	float bw = 92, bh = 46, gap = 10, total = 6 * bw + 5 * gap, bx = c.x + (c.w - total) / 2, cy = by + 46;
	if (ui_button(ID_PREV, (struct rect){bx, cy, bw, bh}, "|<<") && np->current > 0)
		nowplaying_play(np->current - 1);
	if (ui_button(ID_BACK30, (struct rect){bx + (bw + gap), cy, bw, bh}, "-30s"))
		player_seek(pos - 30);
	int paused = st == PL_PAUSED;
	if (ui_button(ID_PLAY, (struct rect){bx + 2 * (bw + gap), cy, bw, bh}, !np->active ? "Play" : paused ? "Play" : "Pause")) {
		if (!np->active)
			nowplaying_play(np->current);
		else
			player_pause(!paused);
	}
	if (ui_button(ID_FWD30, (struct rect){bx + 3 * (bw + gap), cy, bw, bh}, "+30s"))
		player_seek(pos + 30);
	if (ui_button(ID_NEXT, (struct rect){bx + 4 * (bw + gap), cy, bw, bh}, ">>|") && np->current + 1 < np->count)
		nowplaying_play(np->current + 1);
	if (ui_button(ID_STOP, (struct rect){bx + 5 * (bw + gap), cy, bw, bh}, "Stop"))
		nowplaying_stop();

	/* Remote shortcuts: +/- for next/previous track. */
	if (ui_pressed(BTN_PLUS) && np->current + 1 < np->count)
		nowplaying_play(np->current + 1);
	ui_hints("(A) Select    (B) Back, keep playing    (+) Next track");
}

const struct screen screen_player = {"player", enter, frame};
