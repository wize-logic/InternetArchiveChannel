/* Minimal GX bring-up: one 640x480 (or 480p) EFB, double-buffered XFBs, 2D ortho projection. */
#ifndef IAC_GX_H
#define IAC_GX_H

#include <gccore.h>

struct gx_state {
	GXRModeObj *mode;
	void *xfb[2];
	int cur;
};

void gx_init(struct gx_state *g);
/* Copies the EFB to the back XFB and flips. vsync=0 doesn't wait (for benchmarks). */
void gx_present(struct gx_state *g, int vsync);
/* Front XFB, e.g. for console_init() after GX work is done. */
void *gx_front_xfb(struct gx_state *g);

#endif
