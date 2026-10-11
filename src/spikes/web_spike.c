/* Phase 0 / Spike 3: can NetSurf lay out and render archived pages on the Wii?
 *
 * Loads 1999-era Wayback snapshots (HTML + GIFs, saved under sd:/apps/iachannel/web/) through
 * the NetSurf core with no JavaScript, renders the page into a 640x480 RGB565 RAM surface via
 * the framebuffer frontend's plotters, and shows it through a GX RGB565 texture.
 * Pass bar (plan): a 1999-era page renders in < 3 s using < 24 MB.
 * Results: sd:/apps/iachannel/spike_web.log. */
#include <dirent.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fat.h>
#include <gccore.h>
#include <wiiuse/wpad.h>

#include <libnsfb.h>

#include "netsurf/browser_window.h"
#include "netsurf/misc.h"
#include "netsurf/netsurf.h"
#include "netsurf/plotters.h"
#include "netsurf/window.h"
#include "desktop/gui_internal.h"
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

#include "core/log.h"
#include "core/platform.h"
#include "gfx/gx.h"

#define APP_DIR "sd:/apps/iachannel"
#define VIEW_W 640
#define VIEW_H 480
#define LOAD_TIMEOUT_MS 60000

extern struct gui_bitmap_table *framebuffer_bitmap_table;
extern struct gui_layout_table *framebuffer_layout_table;
extern struct gui_utf8_table *framebuffer_utf8_table;

static struct gx_state g_gx;

struct gui_window {
	struct browser_window *bw;
	int scroll_y;
	int started; /* throbber started: navigation really began */
	int done;    /* throbber stopped after starting */
};

static struct gui_window *g_window;

static struct gui_window *win_create(struct browser_window *bw, struct gui_window *existing,
	gui_window_create_flags flags)
{
	struct gui_window *gw = calloc(1, sizeof *gw);
	gw->bw = bw;
	g_window = gw;
	return gw;
}

static void win_destroy(struct gui_window *gw)
{
	if (g_window == gw)
		g_window = NULL;
	free(gw);
}

static nserror win_invalidate(struct gui_window *gw, const struct rect *rect)
{
	return NSERROR_OK;
}

static bool win_get_scroll(struct gui_window *gw, int *sx, int *sy)
{
	*sx = 0;
	*sy = gw->scroll_y;
	return true;
}

static nserror win_set_scroll(struct gui_window *gw, const struct rect *rect)
{
	gw->scroll_y = rect->y0;
	return NSERROR_OK;
}

static nserror win_get_dimensions(struct gui_window *gw, int *width, int *height)
{
	*width = VIEW_W;
	*height = VIEW_H;
	return NSERROR_OK;
}

static nserror win_event(struct gui_window *gw, enum gui_window_event event)
{
	/* Navigation also emits a STOP before anything has loaded; only a STOP that follows a
	 * START means the page (and its images/CSS) finished. */
	if (event == GW_EVENT_START_THROBBER)
		gw->started = 1;
	else if (event == GW_EVENT_STOP_THROBBER && gw->started)
		gw->done = 1;
	return NSERROR_OK;
}

static struct gui_window_table g_window_table = {
	.create = win_create,
	.destroy = win_destroy,
	.invalidate = win_invalidate,
	.get_scroll = win_get_scroll,
	.set_scroll = win_set_scroll,
	.get_dimensions = win_get_dimensions,
	.event = win_event,
};

static void misc_quit(void)
{
}

static struct gui_misc_table g_misc_table = {
	.schedule = framebuffer_schedule,
	.quit = misc_quit,
};

/* libfat paths are "sd:/x/y"; file URLs carry them as file:///sd/x/y. */
static nserror wii_nsurl_to_path(struct nsurl *url, char **path_out)
{
	lwc_string *scheme = nsurl_get_component(url, NSURL_SCHEME);
	bool is_file = scheme && lwc_string_length(scheme) == 4 &&
		strncasecmp(lwc_string_data(scheme), "file", 4) == 0;
	if (scheme)
		lwc_string_unref(scheme);
	if (!is_file)
		return NSERROR_BAD_PARAMETER;
	lwc_string *upath = nsurl_get_component(url, NSURL_PATH);
	if (!upath)
		return NSERROR_BAD_PARAMETER;
	const char *p = lwc_string_data(upath);
	size_t len = lwc_string_length(upath);
	/* "/sd/apps/x" -> "sd:/apps/x" */
	const char *slash = len > 1 ? memchr(p + 1, '/', len - 1) : NULL;
	size_t dev = slash ? (size_t)(slash - p - 1) : len - 1;
	char *out = malloc(len + 3);
	if (!out) {
		lwc_string_unref(upath);
		return NSERROR_NOMEM;
	}
	snprintf(out, len + 3, "%.*s:%.*s", (int)dev, p + 1, (int)(len - 1 - dev), p + 1 + dev);
	lwc_string_unref(upath);
	*path_out = out;
	return NSERROR_OK;
}

static nserror wii_path_to_nsurl(const char *path, struct nsurl **url_out)
{
	char buf[1024];
	/* fetch.c passes NULL when a resource file isn't on the SD card. */
	if (!path)
		return NSERROR_NOT_FOUND;
	const char *colon = strchr(path, ':');
	if (colon && colon - path < 8)
		snprintf(buf, sizeof buf, "file:///%.*s%s", (int)(colon - path), path, colon + 1);
	else
		snprintf(buf, sizeof buf, "file://%s", path);
	return nsurl_create(buf, url_out);
}

static struct gui_file_table g_file_table;

static nserror set_defaults(struct nsoption_s *defaults)
{
	nsoption_setnull_charp(cookie_file, strdup(APP_DIR "/netsurf/Cookies"));
	nsoption_setnull_charp(cookie_jar, strdup(APP_DIR "/netsurf/Cookies"));
	return NSERROR_OK;
}

/* nsfb RGB565 (native big-endian) -> GX RGB565 4x4 tiles. */
static void upload_rgb565(uint16_t *tiles, const uint8_t *src, int linelen, int w, int h)
{
	uint16_t *d = tiles;
	for (int ty = 0; ty < h; ty += 4)
		for (int tx = 0; tx < w; tx += 4)
			for (int r = 0; r < 4; r++) {
				const uint16_t *row = (const uint16_t *)(src + (size_t)(ty + r) * linelen) + tx;
				*d++ = row[0];
				*d++ = row[1];
				*d++ = row[2];
				*d++ = row[3];
			}
	DCFlushRange(tiles, (u32)w * h * 2);
}

static void show_surface(nsfb_t *fb)
{
	static uint16_t *tex;
	static GXTexObj obj;
	uint8_t *ptr;
	int linelen;
	if (!tex) {
		tex = memalign(32, VIEW_W * VIEW_H * 2);
		GX_InitTexObj(&obj, tex, VIEW_W, VIEW_H, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
	}
	nsfb_get_buffer(fb, &ptr, &linelen);
	upload_rgb565(tex, ptr, linelen, VIEW_W, VIEW_H);

	GX_InvVtxCache();
	GX_InvalidateTexAll();
	GX_LoadTexObj(&obj, GX_TEXMAP0);
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	GX_Position2f32(0, 0);
	GX_TexCoord2f32(0, 0);
	GX_Position2f32(VIEW_W, 0);
	GX_TexCoord2f32(1, 0);
	GX_Position2f32(VIEW_W, VIEW_H);
	GX_TexCoord2f32(1, 1);
	GX_Position2f32(0, VIEW_H);
	GX_TexCoord2f32(0, 1);
	GX_End();
	gx_present(&g_gx, 1);
}

struct page_result {
	const char *name;
	int ok;
	uint64_t load_ms, first_render_ms, full_render_ms;
	int width, height, chunks;
	int heap_kb;
};

static void run_page(const char *name, nsfb_t *fb, struct page_result *r)
{
	char url_s[600];
	nsurl *url;
	struct browser_window *bw;

	memset(r, 0, sizeof *r);
	r->name = name;
	snprintf(url_s, sizeof url_s, "file:///sd/apps/iachannel/web/%s.html", name);
	log_printf("\n== %s\n", url_s);

	struct mallinfo before = mallinfo();
	uint64_t t0 = plat_ms();
	if (nsurl_create(url_s, &url) != NSERROR_OK ||
		browser_window_create(BW_CREATE_HISTORY, url, NULL, NULL, &bw) != NSERROR_OK) {
		log_printf("  could not create window\n");
		return;
	}
	nsurl_unref(url);

	while (!(g_window && g_window->done) && plat_ms() - t0 < LOAD_TIMEOUT_MS) {
		int next = schedule_run();
		if (next != 0)
			usleep(1000);
	}
	r->load_ms = plat_ms() - t0;
	if (!(g_window && g_window->done)) {
		log_printf("  load timed out after %llu ms\n", (unsigned long long)r->load_ms);
		browser_window_destroy(bw);
		return;
	}
	browser_window_get_extents(bw, false, &r->width, &r->height);
	struct mallinfo loaded = mallinfo();
	r->heap_kb = (loaded.uordblks - before.uordblks) / 1024;
	log_printf("  loaded in %llu ms, document %dx%d, heap +%d KB (total in use %d KB)\n",
		(unsigned long long)r->load_ms, r->width, r->height, r->heap_kb, loaded.uordblks / 1024);

	struct redraw_context ctx = {
		.interactive = false,
		.background_images = true,
		.plot = &fb_plotters,
	};
	struct rect clip = {0, 0, VIEW_W, VIEW_H};
	uint64_t render_total = 0;
	for (int y = 0; y < r->height && r->chunks < 20; y += VIEW_H, r->chunks++) {
		uint64_t t = plat_ms();
		g_window->scroll_y = y;
		nsfb_bbox_t box = {0, 0, VIEW_W, VIEW_H};
		nsfb_claim(fb, &box);
		browser_window_redraw(bw, 0, -y, &clip, &ctx);
		nsfb_update(fb, &box);
		uint64_t ms = plat_ms() - t;
		render_total += ms;
		if (y == 0) {
			r->first_render_ms = ms;
			/* Sanity check that something was drawn: count distinct colours on a sample grid. */
			uint8_t *px;
			int ll;
			nsfb_get_buffer(fb, &px, &ll);
			uint16_t seen[16];
			int nseen = 0;
			for (int sy = 8; sy < VIEW_H; sy += 16)
				for (int sx = 8; sx < VIEW_W; sx += 16) {
					uint16_t c = ((uint16_t *)(px + (size_t)sy * ll))[sx];
					int k = 0;
					while (k < nseen && seen[k] != c)
						k++;
					if (k == nseen && nseen < 16)
						seen[nseen++] = c;
				}
			log_printf("  first screen: %d distinct colours sampled, top-left 0x%04x\n", nseen,
				((uint16_t *)px)[0]);
			show_surface(fb);
			show_surface(fb);
			/* Leave the first screen up briefly so it can be seen (and frame-dumped). */
			for (int i = 0; i < 90; i++)
				VIDEO_WaitVSync();
		}
	}
	r->full_render_ms = render_total;
	log_printf("  render: first screen %llu ms, %d screen(s) %llu ms total\n",
		(unsigned long long)r->first_render_ms, r->chunks, (unsigned long long)render_total);
	r->ok = 1;

	browser_window_destroy(bw);
	for (int i = 0; i < 50; i++)
		schedule_run();
}

static int setup_netsurf(void)
{
	/* -v: NetSurf INFO logging to stderr, which SYS_STDIO_Report routes to Dolphin's log.
	 * Only under Dolphin (it exposes /dev/dolphin): on hardware it would cost time on the UART. */
	s32 dev = IOS_Open("/dev/dolphin", 0);
	int in_dolphin = dev >= 0;
	if (in_dolphin)
		IOS_Close(dev);
	char *argv[] = {"iachannel", in_dolphin ? "-v" : NULL, NULL};
	int argc = in_dolphin ? 2 : 1;

	g_file_table = *default_file_table;
	g_file_table.nsurl_to_path = wii_nsurl_to_path;
	g_file_table.path_to_nsurl = wii_path_to_nsurl;

	/* netsurf_register() keeps this pointer for the program's lifetime. */
	static struct netsurf_table table;
	table.misc = &g_misc_table;
	table.window = &g_window_table;
	table.file = &g_file_table;
	table.fetch = framebuffer_fetch_table;
	table.utf8 = framebuffer_utf8_table;
	table.bitmap = framebuffer_bitmap_table;
	table.layout = framebuffer_layout_table;
	log_printf("setup: register\n");
	if (netsurf_register(&table) != NSERROR_OK) {
		log_printf("FAIL: netsurf_register\n");
		return -1;
	}
	log_printf("setup: respaths\n");
	respaths = fb_init_resource_path(APP_DIR "/netsurf");
	log_printf("setup: nslog\n");
	nslog_init(NULL, &argc, argv);
	log_printf("setup: options\n");
	if (nsoption_init(set_defaults, &nsoptions, &nsoptions_default) != NSERROR_OK) {
		log_printf("FAIL: nsoption_init\n");
		return -1;
	}
	log_printf("setup: messages\n");
	char *messages = filepath_find(respaths, "Messages");
	if (!messages || messages_add_from_file(messages) != NSERROR_OK)
		log_printf("warning: no Messages file in %s/netsurf\n", APP_DIR);
	free(messages);
	log_printf("setup: netsurf_init\n");
	if (netsurf_init(NULL) != NSERROR_OK) {
		log_printf("FAIL: netsurf_init\n");
		return -1;
	}
	nsoption_set_bool(core_select_menu, true);
	return 0;
}

int main(void)
{
	gx_init(&g_gx);
	SYS_STDIO_Report(false);
	WPAD_Init();
	int have_sd = fatInitDefault();
	log_init(have_sd ? APP_DIR "/spike_web.log" : NULL);
	log_printf("IAChannel spike 3: NetSurf page rendering\n");
	struct mallinfo mi = mallinfo();
	log_printf("heap at start: %d KB\n", mi.uordblks / 1024);

	uint64_t t = plat_ms();
	if (setup_netsurf() != 0)
		goto done;
	nsfb_t *fb = framebuffer_initialise("ram", VIEW_W, VIEW_H, 16);
	if (!fb || !fb_font_init()) {
		log_printf("FAIL: framebuffer/font init\n");
		goto done;
	}
	mi = mallinfo();
	log_printf("netsurf init %llu ms, heap %d KB\n", (unsigned long long)(plat_ms() - t),
		mi.uordblks / 1024);

	/* Every *.html in the web directory, up to MAX_PAGES. */
	enum { MAX_PAGES = 8 };
	static char names[MAX_PAGES][64];
	int npages = 0;
	DIR *dir = opendir(APP_DIR "/web");
	struct dirent *de;
	while (dir && npages < MAX_PAGES && (de = readdir(dir)) != NULL) {
		size_t len = strlen(de->d_name);
		if (len > 5 && len < 64 && strcasecmp(de->d_name + len - 5, ".html") == 0) {
			memcpy(names[npages], de->d_name, len - 5);
			names[npages++][len - 5] = '\0';
		}
	}
	if (dir)
		closedir(dir);
	if (npages == 0)
		log_printf("no pages found in %s/web\n", APP_DIR);

	struct page_result res[MAX_PAGES];
	for (int i = 0; i < npages; i++)
		run_page(names[i], fb, &res[i]);

	mi = mallinfo();
	log_printf("\nheap after all pages: %d KB in use, %d KB arena\n", mi.uordblks / 1024,
		mi.arena / 1024);
	log_printf("\n=== Spike 3 results ===\n");
	int pass = 1, any = 0;
	for (int i = 0; i < npages; i++) {
		if (!res[i].ok) {
			log_printf("%-12s FAILED\n", names[i]);
			pass = 0;
			continue;
		}
		any = 1;
		uint64_t first = res[i].load_ms + res[i].first_render_ms;
		log_printf("%-12s %4dx%-5d load %5llu ms + first screen %4llu ms = %5llu ms, heap +%d KB\n",
			res[i].name, res[i].width, res[i].height, (unsigned long long)res[i].load_ms,
			(unsigned long long)res[i].first_render_ms, (unsigned long long)first, res[i].heap_kb);
		if (first >= 3000 || res[i].heap_kb >= 24 * 1024)
			pass = 0;
	}
	log_printf("bar: first screen < 3 s and < 24 MB per page: %s\n", any && pass ? "PASS" : "FAIL");
	netsurf_exit();

done:
	log_printf("\nPress HOME to exit.\n");
	log_close();
	/* Replace the last page with a clear "finished" screen; on hardware a page left up looked hung. */
	console_init(gx_front_xfb(&g_gx), 20, 20, g_gx.mode->fbWidth, g_gx.mode->xfbHeight,
		g_gx.mode->fbWidth * VI_DISPLAY_PIX_SZ);
	printf("\x1b[2J\x1b[2;0HSpike 3 done. Results in %s/spike_web.log\nPress HOME to exit.\n", APP_DIR);
	for (;;) {
		WPAD_ScanPads();
		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
			break;
		VIDEO_WaitVSync();
	}
	return 0;
}
