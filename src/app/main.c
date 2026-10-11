/* Internet Archive Channel: app entry point and frame loop. */
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "app/app.h"
#include "core/log.h"
#include "media/player.h"
#include "net/http.h"
#include "NotoSans-Bold_ttf.h"
#include "NotoSans-Regular_ttf.h"
#include "ui/autotest.h"
#include "ui/osk.h"
#include "wl/platform/mem.h"
#include "wl/platform/platform.h"

#define SETTINGS_PATH APP_DIR "/settings.ini"
#define STACK_MAX 8

struct settings app_settings;

static struct {
	const struct screen *stack[STACK_MAX];
	int focus[STACK_MAX]; /* focused widget per screen, restored when returning to it */
	int depth;
	const struct screen *pending_push;
	void *pending_arg;
	int pending_pop;
	int exit_dialog;
	int quit;
} g;

void app_push(const struct screen *s, void *arg)
{
	/* Applied between frames so a screen never changes under its own frame(). */
	g.pending_push = s;
	g.pending_arg = arg;
}

void app_pop(void)
{
	g.pending_pop = 1;
}

void app_request_exit(void)
{
	g.exit_dialog = 1;
}

void app_save_settings(void)
{
	if (settings_save(&app_settings, SETTINGS_PATH) != 0)
		WL_LOG_W("APP", "could not save %s", SETTINGS_PATH);
}

static void apply_stack_changes(void)
{
	if (g.pending_pop && g.depth > 1) {
		g.depth--;
		ui_focus(g.focus[g.depth - 1]);
	}
	g.pending_pop = 0;
	if (g.pending_push && g.depth < STACK_MAX) {
		if (g.depth > 0)
			g.focus[g.depth - 1] = ui_get_focus();
		g.stack[g.depth++] = g.pending_push;
		ui_focus(0);
		WL_LOG_I("APP", "screen: %s", g.pending_push->name);
		if (g.pending_push->enter)
			g.pending_push->enter(g.pending_arg);
	}
	g.pending_push = NULL;
}

static void exit_dialog(void)
{
	enum { ID_YES = 50001, ID_NO };
	static int opened_frame;
	ui_modal_begin();
	float w = 460, h = 190, x = ((float)draw_width() - w) / 2, y = (UI_H - h) / 2;
	draw_rect((struct rect){x, y, w, h}, RGBA(0x1c, 0x1e, 0x22, 0xF8));
	draw_rect_outline((struct rect){x, y, w, h}, 2, theme.panel_hi);
	font_draw(theme.title, x + 24, y + 20, theme.text, "Exit the channel?", -1);
	font_draw(theme.small, x + 24, y + 66, theme.text_dim, "You'll return to the Homebrew Channel.", -1);
	if (ui_button(ID_YES, (struct rect){x + 24, y + h - 70, 190, 50}, "Exit"))
		g.quit = 1;
	if (ui_button(ID_NO, (struct rect){x + w - 214, y + h - 70, 190, 50}, "Cancel") ||
		ui_pressed(BTN_B))
		g.exit_dialog = 0;
	/* Open on Cancel, so a stray A press doesn't quit. */
	if (!opened_frame) {
		ui_focus(ID_NO);
		opened_frame = 1;
	}
	if (!g.exit_dialog)
		opened_frame = 0;
	ui_modal_end();
}

static void overlay(void)
{
	static uint64_t last_us;
	static uint32_t frames, last_frames;
	static float fps;
	uint64_t now = wl_platform_time_us();
	frames++; /* frames this loop drew while the overlay was up */
	if (now - last_us >= 1000000) {
		fps = (float)(frames - last_frames) * 1e6f / (float)(now - last_us);
		last_us = now;
		last_frames = frames;
	}
	struct mallinfo mi = mallinfo();
	char line[128];
	snprintf(line, sizeof line, "%.1f fps  heap %d KB  dropped %u", (double)fps, mi.uordblks / 1024,
		wl_platform_video_dropped_count());
	struct http_stats hs;
	http_get_stats(&hs);
	char net[128];
	snprintf(net, sizeof net, "net %s  conn %u  active %u  queued %u  %u KB in",
		net_state() == NET_READY ? "up" : net_state() == NET_FAILED ? "FAILED" : "starting", hs.connections,
		hs.active, hs.queued, hs.bytes_in / 1024);
	struct rect s = ui_safe_area();
	draw_rect((struct rect){s.x + s.w - 380, s.y, 380, 46}, RGBA(0, 0, 0, 0xB0));
	font_draw(theme.small, s.x + s.w - 372, s.y + 2, RGBA(0x80, 0xff, 0x80, 0xff), line, -1);
	font_draw(theme.small, s.x + s.w - 372, s.y + 22, RGBA(0x80, 0xff, 0x80, 0xff), net, -1);
}

/* The network code's log_printf lines (HTTP retries, TLS and socket errors) go to the SD log too. */
static void net_log_line(const char *line)
{
	int n = (int)strlen(line);
	while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
		n--;
	WL_LOG_I("NET", "%.*s", n, line);
}

static int init_platform(void)
{
	wl_platform_fs_ios_init(NULL); /* before WPAD, sockets and mounts */
	wl_platform_system_init();
	wl_platform_fs_mount(WL_DEVICE_SD);
	wl_platform_log_init(APP_DIR "/logs");
	WL_LOG_I("APP", "Internet Archive Channel %s", APP_VERSION);
	log_set_hook(net_log_line);

	/* wl_mem first. It claims only what its MEM1 budgets need, from the top of MEM1, so libogc's
	 * malloc keeps the rest of MEM1 and spills into MEM2 for FFmpeg and NetSurf. MEM2 tags stay
	 * disabled until the app sizes its own (textures, media buffers). */
	WlMemConfig cfg;
	wl_mem_config_default(&cfg);
	static WlMemTagDef tags[WL_MEM_MAX_TAGS];
	for (uint32_t i = 0; i < cfg.tag_count; i++) {
		tags[i] = cfg.tags[i];
		if (tags[i].region == WL_MEM2)
			tags[i].budget = 0;
	}
	/* Only the GX FIFO, the audio thread's buffers and thread stacks come from wl_mem; everything
	 * else (FFmpeg, NetSurf, textures) uses malloc. The DOL is ~10 MB with NetSurf and FFmpeg,
	 * so MEM1 is tight: keep the claim small. */
	tags[WL_TAG_HOT].budget = 0;
	tags[WL_TAG_NET].budget = 0;
	tags[WL_TAG_AUDIO].budget = 256 * 1024;
	tags[WL_TAG_STACK].budget = 256 * 1024;
	cfg.frame_arena_size = 32 * 1024;
	cfg.scene_stack_size = 32 * 1024;
	cfg.tags = tags;
	cfg.claim[WL_MEM1] = WL_MEM_CLAIM_NEEDED; /* only the budgets: the rest of MEM1 stays with malloc */
	if (wl_mem_init(&cfg) != WL_OK) {
		WL_LOG_E("APP", "wl_mem_init failed");
		return -1;
	}
	if (wl_platform_video_init() != WL_OK || wl_gx_init() != WL_OK) {
		WL_LOG_E("APP", "video/GX init failed");
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	/* The channel's forwarder passes --channel: exit to the Wii Menu, as there's no Homebrew
	 * Channel to return to. */
	WlExitTarget exit_to = WL_EXIT_LOADER;
	for (int i = 1; i < argc; i++)
		if (argv[i] && strcmp(argv[i], "--channel") == 0)
			exit_to = WL_EXIT_WII_MENU;

	if (init_platform() != 0)
		wl_platform_exit(exit_to);
	if (exit_to == WL_EXIT_WII_MENU)
		WL_LOG_I("APP", "started from the channel");

	settings_defaults(&app_settings);
	settings_load(&app_settings, SETTINGS_PATH);

	WlVideoInfo vi;
	wl_platform_video_info(&vi);
	WL_LOG_I("APP", "video %ux%u %s%s%s", vi.fb_width, vi.efb_height, vi.progressive ? "480p" : "interlaced",
		vi.widescreen ? " 16:9" : "", vi.pal ? " PAL" : "");
	draw_init(vi.widescreen);
	input_init();

	if (font_system_init() != 0) {
		WL_LOG_E("APP", "font init failed");
		wl_platform_exit(exit_to);
	}
	ui_init();
	ui_set_overscan(app_settings.overscan);
	theme.small = font_load_mem(NotoSans_Regular_ttf, NotoSans_Regular_ttf_size, 18);
	theme.body = font_load_mem(NotoSans_Regular_ttf, NotoSans_Regular_ttf_size, 22);
	theme.heading = font_load_mem(NotoSans_Bold_ttf, NotoSans_Bold_ttf_size, 26);
	theme.title = font_load_mem(NotoSans_Bold_ttf, NotoSans_Bold_ttf_size, 30);
	theme.huge = font_load_mem(NotoSans_Bold_ttf, NotoSans_Bold_ttf_size, 44);
	if (!theme.small || !theme.body || !theme.heading || !theme.title || !theme.huge) {
		WL_LOG_E("APP", "font load failed");
		wl_platform_exit(exit_to);
	}

	art_load();
	if (player_init() != 0)
		WL_LOG_W("APP", "audio output unavailable");
	net_boot_start();
	app_push(&screen_home, NULL);
	apply_stack_changes();

	struct input_state in;
	while (!g.quit) {
		uint32_t sys = wl_platform_system_events();
		if (sys & WL_SYS_POWER) {
			wl_platform_log_flush();
			wl_platform_exit(WL_EXIT_POWER_OFF);
		}
		if (sys & WL_SYS_RESET)
			break;

		input_poll(&in, draw_width());
		ui_begin(&in);
		wl_gx_begin_frame(0x000000FFu);
		draw_begin_frame();

		g.stack[g.depth - 1]->frame();
		if (!g.exit_dialog && !ui_modal_active() && ui_pressed(BTN_HOME))
			g.exit_dialog = 1;
		if (g.exit_dialog)
			exit_dialog();
		if (app_settings.debug_overlay)
			overlay();

		thumbs_frame();
		nowplaying_frame();
		const struct screen *top = g.stack[g.depth - 1];
		if (top != &screen_player && top != &screen_video)
			nowplaying_minibar();
		ui_end();
		draw_end_frame();
		wl_gx_end_frame();
		apply_stack_changes();
		wl_platform_log_flush();
		if (autotest_quit_requested())
			break;
	}

	player_shutdown();
	WL_LOG_I("APP", exit_to == WL_EXIT_WII_MENU ? "exiting to the Wii Menu" : "exiting to loader");
	wl_platform_log_flush();
	wl_platform_exit(exit_to);
}
