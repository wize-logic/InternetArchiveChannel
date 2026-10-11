/* Immediate-mode 2D drawing in UI coordinates (y down). The UI is laid out on a virtual canvas
 * 480 pixels tall and draw_width() wide: 640 at 4:3, 854 when the console is set to 16:9, so
 * the same layout isn't stretched on widescreen TVs. */
#ifndef IAC_DRAW_H
#define IAC_DRAW_H

#include <stdint.h>

#include "wl/platform/platform.h"

#define UI_H 480

/* RGBA helpers: 0xRRGGBBAA. */
#define RGBA(r, g, b, a) (((uint32_t)(r) << 24) | ((uint32_t)(g) << 16) | ((uint32_t)(b) << 8) | (uint32_t)(a))
static inline uint32_t rgba_alpha(uint32_t c, uint8_t a)
{
	return (c & 0xFFFFFF00u) | (uint32_t)(((c & 0xFF) * a) / 255);
}

struct rect {
	float x, y, w, h;
};

static inline int rect_contains(struct rect r, float px, float py)
{
	return px >= r.x && py >= r.y && px < r.x + r.w && py < r.y + r.h;
}

void draw_init(int widescreen);
int draw_width(void);
/* Call after wl_gx_begin_frame: sets the projection and default state. */
void draw_begin_frame(void);
void draw_end_frame(void); /* flushes queued text */

void draw_rect(struct rect r, uint32_t rgba);
void draw_rect_vgradient(struct rect r, uint32_t top, uint32_t bottom);
void draw_rect_outline(struct rect r, float thickness, uint32_t rgba);
void draw_image(struct rect r, const WlGxTexObj *tex, uint32_t tint);

/* Restricts drawing to r (in UI coordinates); draw_clip_reset() restores the full screen. */
void draw_clip(struct rect r);
void draw_clip_reset(void);

#endif
