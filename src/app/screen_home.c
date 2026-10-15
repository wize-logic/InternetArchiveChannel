#include <stdio.h>
#include <string.h>

#include "app/app.h"
#include "ia/ia.h"
#include "ui/osk.h"

enum { ID_SEARCH = 1, ID_MOVIES, ID_AUDIO, ID_TEXTS, ID_IMAGES, ID_WAYBACK, ID_SETTINGS, ID_ABOUT, ID_SCOPE,
	ID_COLLECTIONS };

struct category {
	int id;
	const char *title, *subtitle, *query;
	uint32_t accent;
	int icon;
	const char *mediatype;
};

static const struct category k_categories[] = {
	/* Curated collections give a better first page than "most downloaded" across a mediatype,
	 * which is dominated by uploads that aren't films. */
	{ID_MOVIES, "Movies", "Film & TV",
		"mediatype:movies AND collection:(feature_films OR prelinger OR classic_tv OR "
		"classic_cartoons OR silent_films OR film_noir OR moviesandfilms)", RGBA(0xe8, 0x6a, 0x4a, 0xFF), ICON_MOVIES,
		"movies"},
	{ID_AUDIO, "Audio", "Music & radio",
		"mediatype:audio AND collection:(etree OR oldtimeradio OR 78rpm OR audio_music OR "
		"GratefulDead OR radioprograms)", RGBA(0x4a, 0xc0, 0x7e, 0xFF), ICON_AUDIO, "audio"},
	/* Books: library scans and Project Gutenberg, only items with page images or text (the most
	 * downloaded "texts" overall are mostly uploads that aren't books). */
	{ID_TEXTS, "Books", "Books & texts",
		"mediatype:texts AND collection:(gutenberg OR americana OR toronto OR cdl OR universallibrary OR "
		"europeanlibraries OR library_of_congress OR bostonpubliclibrary OR americanlibraries OR "
		"canadian_libraries) AND (format:\"Single Page Processed JP2 ZIP\" OR format:Text OR format:DjVuTXT)",
		RGBA(0xe8, 0xc0, 0x4a, 0xFF), ICON_BOOKS, "texts"},
	{ID_IMAGES, "Images", "Photos & scans", "mediatype:image", RGBA(0xb0, 0x6a, 0xe8, 0xFF), ICON_IMAGES, "image"},
	/* Everything else: the collections under the archive's video, audio, text and image sections, most
	 * visited first; one opens as its own list. (Every search leaves out the lending, print-disabled
	 * and stream-only ones, TV news included: the channel can't open their items.) */
	{ID_COLLECTIONS, "Collections", "Browse them all",
		"mediatype:collection AND collection:(movies OR audio OR etree OR texts OR image) AND NOT identifier:fav-*",
		RGBA(0x4a, 0xc8, 0xc0, 0xFF), ICON_COLLECTIONS, "collection"},
	{ID_WAYBACK, "Wayback", "Old websites", "", RGBA(0x4a, 0x9e, 0xe8, 0xFF), ICON_WAYBACK, NULL},
};
#define CATEGORY_COUNT (int)(sizeof k_categories / sizeof k_categories[0])

/* What the search box searches: everything, or one category (the button beside it cycles). */
static const struct {
	const char *label, *mediatype;
} k_scopes[] = {{"All", ""}, {"Movies", "movies"}, {"Audio", "audio"}, {"Books", "texts"}, {"Images", "image"}};
#define SCOPE_COUNT (int)(sizeof k_scopes / sizeof k_scopes[0])
static int g_scope;

static int g_searching;
/* The search box text: kept while the app runs, empty on the next start. */
static char g_search[128];

static void enter(void *arg)
{
	(void)arg;
	g_searching = 0;
}

static void frame(void)
{
	struct rect c = app_chrome("Internet Archive", "Unofficial channel for the Wii");

	float scope_w = 170;
	char hint[64];
	snprintf(hint, sizeof hint, g_scope ? "Search %s..." : "Search the Internet Archive...", k_scopes[g_scope].label);
	if (ui_text_field(ID_SEARCH, (struct rect){c.x, c.y, c.w - scope_w - 10, 50}, g_search, hint,
			art_icon(ICON_SEARCH))) {
		osk_open(g_scope ? hint : "Search", g_search, sizeof g_search - 1);
		g_searching = 1;
	}
	char scope[32];
	snprintf(scope, sizeof scope, "In: %s", k_scopes[g_scope].label);
	if (ui_button(ID_SCOPE, (struct rect){c.x + c.w - scope_w, c.y, scope_w, 50}, scope))
		g_scope = (g_scope + 1) % SCOPE_COUNT;

	/* Tile grid under the search bar: 3 x 2 on the 854-wide (16:9) canvas, 2 x 3 on 4:3, so tiles
	 * stay wide enough for their titles and every icon is the same size. */
	float gap = 12, top = c.y + 62;
	int cols = draw_width() > 700 ? 3 : 2;
	int rows = (CATEGORY_COUNT + cols - 1) / cols;
	float tile_h = (c.h - 62 - 58 - gap * (float)(rows - 1)) / (float)rows;
	float tile_w = (c.w - gap * (float)(cols - 1)) / (float)cols;
	for (int i = 0; i < CATEGORY_COUNT; i++) {
		const struct category *k = &k_categories[i];
		struct rect r = {c.x + (float)(i % cols) * (tile_w + gap), top + (float)(i / cols) * (tile_h + gap), tile_w, tile_h};
		if (ui_tile(k->id, r, k->title, k->subtitle, k->accent, art_icon(k->icon))) {
			if (k->id == ID_WAYBACK) {
				app_push(&screen_wayback, NULL);
			} else {
				static struct results_args args;
				memset(&args, 0, sizeof args);
				snprintf(args.title, sizeof args.title, "%s", k->title);
				snprintf(args.query, sizeof args.query, "%s", k->query);
				snprintf(args.mediatype, sizeof args.mediatype, "%s", k->mediatype);
				snprintf(args.category, sizeof args.category, "%s", k->title);
				args.sort = IA_SORT_POPULAR;
				app_push(&screen_results, &args);
			}
		}
	}

	float by = top + (float)rows * (tile_h + gap);
	if (ui_button_icon(ID_SETTINGS, (struct rect){c.x, by, 200, 46}, "Settings", art_icon(ICON_SETTINGS)))
		app_push(&screen_settings, NULL);
	if (ui_button_icon(ID_ABOUT, (struct rect){c.x + 214, by, 200, 46}, "About", art_icon(ICON_ABOUT)))
		app_push(&screen_about, NULL);

	ui_hints("(A) Select    (HOME) Exit");

	if (g_searching) {
		int r = osk_update();
		if (r != OSK_EDITING) {
			g_searching = 0;
			if (r == OSK_DONE && osk_text()[0]) {
				snprintf(g_search, sizeof g_search, "%s", osk_text());
				static struct results_args args;
				memset(&args, 0, sizeof args);
				if (g_scope)
					snprintf(args.title, sizeof args.title, "\"%s\" in %s", g_search, k_scopes[g_scope].label);
				else
					snprintf(args.title, sizeof args.title, "\"%s\"", g_search);
				ia_user_query(g_search, k_scopes[g_scope].mediatype, 1, args.query, sizeof args.query);
				ia_user_query(g_search, k_scopes[g_scope].mediatype, 0, args.fallback, sizeof args.fallback);
				args.sort = IA_SORT_POPULAR;
				if (args.query[0])
					app_push(&screen_results, &args);
			}
		}
	}
}

const struct screen screen_home = {"home", enter, frame};
