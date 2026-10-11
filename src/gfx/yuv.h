/* Planar YUV 4:2:0 (BT.601, limited range) video frames drawn by the GPU.
 *
 * GX has no YUV texture format, so each plane is an I8 texture. Four TEV stages do the matrix,
 * so the CPU's only per-frame work is re-tiling the planes into GX's 8x4 I8 layout. */
#ifndef IAC_YUV_H
#define IAC_YUV_H

#include <stdint.h>

#include <gccore.h>

struct yuv_tex {
	int w, h;           /* visible luma size */
	int yw, yh, cw, ch; /* texture sizes, padded to 8x4 tiles */
	uint8_t *y, *u, *v; /* 32-byte aligned tile buffers */
	GXTexObj ty, tu, tv;
};

int yuv_tex_init(struct yuv_tex *t, int width, int height, int nearest);
void yuv_tex_free(struct yuv_tex *t);
/* Re-tiles planes (as AVFrame data/linesize) and flushes them for the GPU. */
void yuv_tex_upload(struct yuv_tex *t, uint8_t *const planes[3], const int linesize[3]);
/* Configures TEV, vertex format and texture maps. Call before yuv_draw() each frame. */
void yuv_begin(struct yuv_tex *t);
void yuv_draw(struct yuv_tex *t, float x, float y, float w, float h);

#endif
