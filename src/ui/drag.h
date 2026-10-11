/* Hold-B dragging, shared by the page views (web pages, book pages and text): holding B and
 * moving the pointer drags the content with the hand; a quick tap of B (released within 250 ms,
 * without moving) means "back" as usual. */
#ifndef IAC_DRAG_H
#define IAC_DRAG_H

#include <stdint.h>

#include "ui/input.h"

struct drag {
	int down, active;
	int seen; /* the pointer was on screen last frame (lx/ly are valid) */
	float x0, y0, lx, ly; /* where B went down; the pointer last frame */
	uint64_t t0;
};

enum { DRAG_NONE, DRAG_MOVING, DRAG_TAP };

/* Call every frame. While dragging, *dx and *dy get the pointer's movement this frame (UI pixels;
 * the content moves by the same amount) and DRAG_MOVING is returned. DRAG_TAP is returned once,
 * on the frame B is released after a tap. */
static inline int drag_update(struct drag *d, const struct input_state *in, uint64_t now_us, float *dx, float *dy)
{
	*dx = *dy = 0;
	if (in->pressed & BTN_B) {
		d->down = 1;
		d->active = 0;
		d->t0 = now_us;
		d->x0 = d->lx = in->px;
		d->y0 = d->ly = in->py;
		d->seen = in->pointer_valid;
	}
	if (!d->down)
		return DRAG_NONE;
	if (!(in->held & BTN_B)) {
		int tap = !d->active;
		d->down = d->active = 0;
		return tap ? DRAG_TAP : DRAG_NONE;
	}
	if (in->pointer_valid) {
		if (!d->seen) { /* back on screen: no jump from where it left */
			d->lx = in->px;
			d->ly = in->py;
			d->seen = 1;
		}
		float mx = in->px - d->x0, my = in->py - d->y0;
		if (!d->active && (mx * mx + my * my > 36 || now_us - d->t0 > 250000u))
			d->active = 1;
		if (d->active) {
			*dx = in->px - d->lx;
			*dy = in->py - d->ly;
		}
		d->lx = in->px;
		d->ly = in->py;
	} else {
		d->seen = 0;
		if (now_us - d->t0 > 250000u)
			d->active = 1; /* held: not a tap, even with the pointer off-screen */
	}
	return d->active ? DRAG_MOVING : DRAG_NONE;
}

static inline void drag_reset(struct drag *d)
{
	d->down = d->active = 0;
}

#endif
