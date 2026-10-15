/* App shell: screen stack, shared settings, exit handling. */
#ifndef IAC_APP_H
#define IAC_APP_H

#include "core/settings.h"
#include "ui/ui.h"

#define APP_DIR "sd:/apps/iachannel"
#define APP_VERSION "1.0.0"

struct screen {
	const char *name;
	void (*enter)(void *arg);
	void (*frame)(void); /* draw + handle input for one frame */
};

extern struct settings app_settings;

/* Pixel art (assets/ui), decoded once at start-up. Missing images stay zeroed and the
 * UI falls back to drawn shapes. */
enum { ICON_MOVIES, ICON_AUDIO, ICON_BOOKS, ICON_IMAGES, ICON_WAYBACK, ICON_SEARCH, ICON_SETTINGS,
	ICON_ABOUT, ICON_FAVOURITE, ICON_COLLECTIONS, ICON_COUNT };
#define POINTER_TAP_FRAMES 5
struct app_art {
	struct image icons[ICON_COUNT];
	struct image logo, pointer, pointer_grab, pointer_wait;
	struct image pointer_tap[POINTER_TAP_FRAMES]; /* over something clickable: the finger taps */
};
extern struct app_art art;
void art_load(void);
/* NULL when the image failed to load. */
const struct image *art_icon(int which);

/* Screens deep the stack goes (collections open inside collections); pushes past it are dropped. */
#define APP_STACK_MAX 12
void app_push(const struct screen *s, void *arg);
void app_pop(void);
int app_depth(void); /* screens on the stack; 1 on the home screen */
void app_request_exit(void); /* shows the exit confirmation */
void app_save_settings(void);

/* Shared chrome: background, header with the screen title, safe-area content rect. Below the home
 * screen the header starts with a Back button in place of the logo. */
struct rect app_chrome(const char *title, const char *subtitle);
/* Going back is an on-screen button (B is for holding and dragging, as in the Internet Channel).
 * app_back: the chrome's Back button was activated this frame (call after app_chrome).
 * app_back_button: one placed by a screen without the chrome; 1 when activated. */
int app_back(void);
int app_back_button(struct rect r);
#define APP_BACK_W 116

extern const struct screen screen_home, screen_results, screen_item, screen_settings, screen_about,
	screen_placeholder;

/* Networking comes up on a background thread (DHCP takes seconds on the Wii). */
enum { NET_DOWN = 0, NET_STARTING, NET_READY, NET_FAILED };
void net_boot_start(void);
int net_state(void);

/* Thumbnails: the texture once loaded, else NULL (and a fetch is started). */
const struct image *thumb_get(const char *id);
void thumbs_frame(void);
void thumbs_clear(void);

/* What screen_results shows. */
struct results_args {
	char title[96];
	char query[768]; /* Lucene query */
	char fallback[768]; /* run instead when `query` finds nothing ("" for none) */
	int sort;        /* enum ia_sort */
	/* A category's list: searches from it stay inside its mediatype ("" for none). */
	char mediatype[16];
	char category[24]; /* its name, e.g. "Books" */
	char search[128];  /* the words a search inside a category looked for ("" for the list itself) */
};

/* The current playlist (audio tracks or one video). */
#define NP_MAX_TRACKS 256
struct np_track {
	char title[128];
	char url[512];
	float length;
};
struct nowplaying {
	char item_id[128];
	char item_title[192];
	char creator[160];
	struct np_track tracks[NP_MAX_TRACKS];
	int count, current;
	int active;
	int is_video, fast_video;
};
struct nowplaying *nowplaying(void);
void nowplaying_play(int index);
void nowplaying_stop(void);
void nowplaying_frame(void);   /* auto-advance; call once per frame */
void nowplaying_minibar(void); /* "now playing" strip for screens other than the player */
void nowplaying_format_time(double secs, char *out, size_t cap);
extern const struct screen screen_player, screen_video, screen_viewer, screen_wayback, screen_web;

/* Wayback bookmarks (src/app/bookmarks.c). */
#define BOOKMARKS_MAX 50
struct bookmark {
	char timestamp[16];
	char url[512];
	char title[128];
};
int bookmarks_count(void);
const struct bookmark *bookmarks_get(int i);
int bookmarks_find(const char *url, const char *timestamp);
int bookmarks_toggle(const char *url, const char *timestamp, const char *title);

/* What screen_web opens. */
struct web_args {
	char url[512];
	char timestamp[16];
};

/* What screen_viewer shows: a book's pages, or an item's images. */
#define VIEWER_MAX_FILES 300
struct viewer_args {
	char title[160];
	char id[128];
	int is_book;
	int count;            /* pages (a book's from its metadata, maybe 0), or entries in files */
	char (*files)[192];   /* image file names (not for books) */
	char text_url[512];   /* book text: OCR or a plain-text edition ("" if none) */
	char count_url[512];  /* book page list (page_numbers.json/scandata.xml): the real page count */
};

/* Draws a centred loading indicator (animated) with a message. */
void app_spinner(struct rect area, const char *message);

#endif
