/* Placeholder (Wayback, until Phase 6) and About screens. */
#include <stdio.h>
#include <string.h>

#include "app/app.h"

enum { ID_BACK = 1 };

static char g_title[160];

static void placeholder_frame(const char *subtitle, const char *body)
{
	struct rect c = app_chrome(g_title, subtitle);
	font_draw_wrapped(theme.body, c.x, c.y + 10, (int)c.w, theme.text_dim, body);
	if (ui_button(ID_BACK, (struct rect){c.x, c.y + c.h - 50, 160, 46}, "Back") || ui_pressed(BTN_B))
		app_pop();
	ui_hints("(A) Select    (B) Back");
}

static void placeholder_enter(void *arg)
{
	snprintf(g_title, sizeof g_title, "%s", arg ? (const char *)arg : "Coming soon");
}

static void placeholder_screen_frame(void)
{
	placeholder_frame("Coming soon",
		"The Wayback Machine viewer arrives in Phase 6. Its page renderer (NetSurf) already runs on "
		"the Wii: archived 1999 pages from Yahoo and CNN render in well under a second.");
}

const struct screen screen_placeholder = {"placeholder", placeholder_enter, placeholder_screen_frame};

static void about_enter(void *arg)
{
	(void)arg;
	snprintf(g_title, sizeof g_title, "About");
}

static void about_frame(void)
{
	struct rect c = app_chrome(g_title, "Internet Archive Channel " APP_VERSION);
	static const char *const text =
		"An unofficial Internet Archive (archive.org) browser for the Nintendo Wii. "
		"Not affiliated with or endorsed by the Internet Archive.\n"
		"Tested on a Wii with System Menu 4.3U. Not yet tested: vWii (Wii U) and other System Menu regions.\n"
		"Source: MIT. The binary is GPL-2.0 because it links NetSurf. Uses NetSurf, FFmpeg, mbedTLS, "
		"FreeType, libjpeg-turbo, libpng, zlib, cJSON, Noto Sans, libogc and WiiLibs; their licences are in "
		"THIRD_PARTY_NOTICES.md in the app folder.";
	font_draw_wrapped(theme.small, c.x, c.y + 4, (int)c.w, theme.text, text);
	if (ui_button(ID_BACK, (struct rect){c.x, c.y + c.h - 50, 160, 46}, "Back") || ui_pressed(BTN_B))
		app_pop();
	ui_hints("(A) Select    (B) Back");
}

const struct screen screen_about = {"about", about_enter, about_frame};
