/* The Wayback Machine page view: NetSurf lays pages out at 800 px wide (the typical design width
 * of the sites being browsed); the visible part is rendered to a RAM surface and drawn as a GX
 * texture scaled into the screen area. Only UI-thread calls. */
#ifndef IAC_WEBVIEW_H
#define IAC_WEBVIEW_H

#include <stddef.h>
#include <stdint.h>

struct input_state;

/* Starts NetSurf on first use (resources from sd:/apps/iachannel/netsurf). 0 = ok. */
int webview_init(void);
/* Opens `url` as captured at `timestamp` (YYYYMMDDhhmmss prefix, e.g. "19990508"). */
int webview_open(const char *url, const char *timestamp);
/* Pumps NetSurf, handles input inside the area (UI coordinates) and renders the visible part.
 * Controls: A clicks, holding B drags the page (any direction), +/- zoom, the D-pad scrolls.
 * (Going back is the caller's toolbar: webview_back.) (NetSurf has its own `struct rect`, so plain
 * floats.) */
void webview_frame(float ax, float ay, float aw, float ah, const struct input_state *in, int *want_close);
/* What to draw: the page texture covering the area's full width and `*draw_h` UI pixels tall,
 * plus the scroll bar position (0..1) and size (0..1, >= 1 when everything fits). NULL before
 * the first render. */
const void *webview_texture(float aw, float *draw_h, float *bar_pos, float *bar_size);
/* Horizontal scroll bar position and size (0..1; size >= 1 when the page fits the width). */
void webview_hbar(float *pos, float *size);
void webview_close(void);

int webview_loading(void);
/* The page's original URL and title (may be empty while loading). */
const char *webview_url(void);
const char *webview_title(void);
void webview_status(char *out, size_t cap); /* "Loading 3 files...", link under the pointer, ... */

/* History, zoom (steps of the scale table; returns the new scale in percent; 2 also zooms) and
 * forms: when a text field has the caret, the screen asks for text and types it in. */
int webview_can_back(void);
int webview_can_forward(void);
void webview_back(void);
void webview_forward(void);
int webview_zoom(int steps);
int webview_zoom_percent(void);
/* B is held and the page is being dragged. */
int webview_dragging(void);
/* The pointer is over a link, a form field or another clickable part of the page. */
int webview_over_link(void);
int webview_text_focus(void);
void webview_type(const char *utf8, int submit);

/* Used by the fetcher. */
void wb_fetch_set_timestamp(const char *ts);
void wb_fetch_rewrite(const char *orig, char *out, size_t cap);
int wb_fetch_register(void);
int wb_fetch_active(void);
void webview_note_bytes(size_t n);

#endif
