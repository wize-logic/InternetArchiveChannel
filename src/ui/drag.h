/* Hold-B dragging, shared by the page views (web pages, book pages and text) and the results grid:
 * holding B and moving the pointer drags the content with the hand, as in the Wii's own Internet
 * Channel. B does nothing else; going back is the on-screen Back button (app_chrome). */
#ifndef IAC_DRAG_H
#define IAC_DRAG_H

#include "ui/input.h"

struct drag {
	int down, active;
	int seen; /* the pointer was on screen last frame (lx/ly are valid) */
	float lx, ly; /* the pointer last frame */
};

enum { DRAG_NONE, DRAG_MOVING };

/* Call every frame. While B is held, *dx and *dy get the pointer's movement this frame (UI pixels;
 * the content moves by the same amount) and DRAG_MOVING is returned. A drag needs the pointer on
 * screen; with it off, holding B moves nothing. */
static inline int drag_update(struct drag *d, const struct input_state *in, float *dx, float *dy)
{
	*dx = *dy = 0;
	if (in->pressed & BTN_B) {
		d->down = 1;
		d->lx = in->px;
		d->ly = in->py;
		d->seen = in->pointer_valid;
	}
	if (!d->down || !(in->held & BTN_B)) {
		d->down = d->active = 0;
		return DRAG_NONE;
	}
	d->active = 1;
	if (in->pointer_valid) {
		if (d->seen) { /* back on screen: no jump from where it left */
			*dx = in->px - d->lx;
			*dy = in->py - d->ly;
		}
		d->lx = in->px;
		d->ly = in->py;
		d->seen = 1;
	} else {
		d->seen = 0;
	}
	return DRAG_MOVING;
}

static inline void drag_reset(struct drag *d)
{
	d->down = d->active = 0;
}

#endif
