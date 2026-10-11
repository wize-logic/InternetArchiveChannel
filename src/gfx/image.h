/* Decoded images as GX textures: PNG (UI art, alpha) and JPEG (thumbnails, pages). */
#ifndef IAC_IMAGE_H
#define IAC_IMAGE_H

#include <stddef.h>

#include "wl/platform/platform.h"

struct image {
	WlGxTexObj tex;
	int w, h;
	void *pixels; /* GX-tiled texel data (RGB5A3 or RGB565), owned */
};

/* Decodes a PNG held in memory. `smooth` = linear filtering (photos); pixel art wants 0. */
int image_load_png(struct image *img, const void *png, size_t size, int smooth);
/* Decodes a JPEG, shrinking by 1/2, 1/4 or 1/8 during decode until it fits max_w x max_h
 * (GX textures are at most 1024x1024). Opaque, so stored as RGB565. */
int image_load_jpeg(struct image *img, const void *jpg, size_t size, int max_w, int max_h);
/* Picks PNG or JPEG by signature (GIF and others are rejected). */
int image_load_any(struct image *img, const void *data, size_t size, int max_w, int max_h);
void image_free(struct image *img);

#endif
