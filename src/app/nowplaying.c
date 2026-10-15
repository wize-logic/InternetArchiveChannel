/* The playlist behind the player: survives screen changes, so audio keeps playing while you
 * browse. Advances to the next track when one ends. */
#include <stdio.h>
#include <string.h>

#include "app/app.h"
#include "media/player.h"

static struct nowplaying g_np;

struct nowplaying *nowplaying(void)
{
	return &g_np;
}

void nowplaying_play(int index)
{
	if (index < 0 || index >= g_np.count)
		return;
	g_np.current = index;
	g_np.active = 1;
	WL_LOG_I("NP", "track %d/%d: %s", index + 1, g_np.count, g_np.tracks[index].title);
	player_open(g_np.tracks[index].url, g_np.is_video, g_np.fast_video);
}

void nowplaying_stop(void)
{
	player_stop();
	g_np.active = 0;
}

void nowplaying_frame(void)
{
	/* A progress line every 10 s while playing: position, state, buffer, output underruns. */
	static uint64_t last_log;
	uint64_t now = wl_platform_time_us();
	if (g_np.active && now - last_log > 10000000u) {
		last_log = now;
		uint32_t shown, skipped, dropped;
		player_video_stats(&shown, &skipped, &dropped);
		WL_LOG_I("NP", "%.1f / %.1f s, state %d, buffer %.0f%%, underruns %u, video %u shown %u late %u dropped, heap %d KB",
			player_position(), player_duration(), (int)player_get_state(), (double)(player_buffer_level() * 100),
			wl_platform_audio_underruns(), shown, skipped, dropped, player_heap_used_kb());
	}
	if (g_np.active && player_get_state() == PL_ENDED) {
		if (g_np.current + 1 < g_np.count)
			nowplaying_play(g_np.current + 1);
		else
			nowplaying_stop();
	}
}

void nowplaying_format_time(double secs, char *out, size_t cap)
{
	int s = secs < 0 ? 0 : (int)secs;
	if (s >= 3600)
		snprintf(out, cap, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(out, cap, "%d:%02d", s / 60, s % 60);
}

void nowplaying_minibar(void)
{
	if (!g_np.active || g_np.is_video)
		return;
	struct rect s = ui_safe_area();
	char t1[32], t2[32], line[256];
	nowplaying_format_time(player_position(), t1, sizeof t1);
	nowplaying_format_time(player_duration(), t2, sizeof t2);
	const char *st = player_get_state() == PL_LOADING ? "Loading" : player_get_state() == PL_PAUSED ? "Paused" : "";
	snprintf(line, sizeof line, "%s%s%s  %s / %s", st, st[0] ? ": " : "", g_np.tracks[g_np.current].title, t1, t2);
	float w = 360, h = 30;
	struct rect r = {s.x + s.w - w, s.y + s.h - h - 34, w, h};
	draw_rect(r, RGBA(0x10, 0x11, 0x14, 0xE8));
	draw_rect((struct rect){r.x, r.y + r.h - 3,
			  player_duration() > 0 ? w * (float)(player_position() / player_duration()) : 0, 3},
		theme.accent);
	if (art_icon(ICON_AUDIO))
		ui_draw_image_fit((struct rect){r.x + 4, r.y + 3, 24, 24}, art_icon(ICON_AUDIO), 0xFFFFFFFFu);
	font_draw_clipped(theme.small, r.x + 34, r.y + 4, theme.text, line, (int)w - 40);
}
