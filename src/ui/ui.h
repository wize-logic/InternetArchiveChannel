/* Immediate-mode widgets for a TV UI driven by a Wii Remote pointer or a D-pad.
 *
 * Each frame, screens call widget functions with a stable id; a widget returns non-zero when
 * activated. Focus follows the pointer when it moves; the D-pad moves focus to the nearest
 * widget in that direction (using the previous frame's layout). A activates the widget under
 * the pointer, or the focused one when the pointer isn't on any widget. */
#ifndef IAC_UI_H
#define IAC_UI_H

#include "gfx/image.h"
#include "ui/draw.h"
#include "ui/font.h"
#include "ui/input.h"

struct ui_theme {
	ui_font *small, *body, *heading, *title, *huge;
	uint32_t bg_top, bg_bottom, panel, panel_hi, text, text_dim, accent, accent_dark, danger;
};

extern struct ui_theme theme;

void ui_init(void);
void ui_begin(const struct input_state *in);
void ui_end(void); /* focus navigation and pointer cursor */

/* While a modal is up, widgets outside it neither hover nor activate. Widgets drawn between
 * ui_modal_begin/end belong to the modal. */
void ui_modal_begin(void);
void ui_modal_end(void);
int ui_modal_active(void);

const struct input_state *ui_input(void);
/* True once per press of the button this frame, unless a modal swallowed it. */
int ui_pressed(uint32_t btn);

/* Safe area: the screen inset by the configured overscan margin. */
struct rect ui_safe_area(void);
void ui_set_overscan(int percent);

int ui_button(int id, struct rect r, const char *label);
/* Registers a focusable region without drawing it (e.g. off-screen grid cells, so the D-pad
 * can reach them). Returns 1 when activated. */
int ui_region(int id, struct rect r);
/* Button with an icon before the label (icon may be NULL). */
int ui_button_icon(int id, struct rect r, const char *label, const struct image *icon);
/* A large card: optional icon, title, optional subtitle, accent stripe. */
int ui_tile(int id, struct rect r, const char *title, const char *subtitle, uint32_t accent,
	const struct image *icon);
/* Shows text (or a dimmed placeholder); activation means "open the keyboard". Icon optional. */
int ui_text_field(int id, struct rect r, const char *text, const char *placeholder,
	const struct image *icon);
/* Pointer shapes. By default the pointer is a pointing hand over widgets and an open hand
 * elsewhere; a screen can ask for another shape each frame (ui_set_cursor, reset by ui_begin). */
enum ui_cursor { UI_CURSOR_AUTO, UI_CURSOR_NORMAL, UI_CURSOR_HAND, UI_CURSOR_WAIT, UI_CURSOR_GRAB, UI_CURSOR_COUNT };
/* Sprite for a shape; (hot_x, hot_y) is the pixel that points. Without one, a drawn arrow is
 * used. WAIT is a clock face: its hands are drawn on top, turning. */
void ui_set_cursor_image(enum ui_cursor shape, const struct image *img, int hot_x, int hot_y);
/* An animated shape: `count` frames (an array of images, same size), `ms` each, looping from the
 * first frame whenever the pointer takes this shape. */
void ui_set_cursor_frames(enum ui_cursor shape, const struct image *frames, int count, int ms, int hot_x,
	int hot_y);
void ui_set_cursor(enum ui_cursor shape);
/* Draws an image fitted (aspect kept) and centred in r. */
void ui_draw_image_fit(struct rect r, const struct image *img, uint32_t tint);
/* Label with an on/off switch; flips *value when activated, returns 1 then. */
int ui_toggle(int id, struct rect r, const char *label, int *value);
void ui_focus(int id);
/* `id` (e.g. a header Back button) is reachable with the D-pad, but when nothing has focus it goes
 * to the first other widget, so A doesn't act on it by surprise. */
void ui_no_default_focus(int id);
int ui_get_focus(void);

/* Bottom hint bar: "(A) Select  (B) Back" etc. */
void ui_hints(const char *text);

#endif
