#include "gfx/image.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#include <jpeglib.h>
#include <png.h>
#include <setjmp.h>

#include "wl/base/base.h"
#include "wl/codec/gx_tex.h"

int image_load_png(struct image *img, const void *png, size_t size, int smooth)
{
	memset(img, 0, sizeof *img);
	png_image pi;
	memset(&pi, 0, sizeof pi);
	pi.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&pi, png, size)) {
		WL_LOG_W("IMG", "png: %s", pi.message);
		return -1;
	}
	pi.format = PNG_FORMAT_RGBA;
	size_t rgba_size = PNG_IMAGE_SIZE(pi);
	uint8_t *rgba = malloc(rgba_size);
	if (!rgba || !png_image_finish_read(&pi, NULL, rgba, 0, NULL)) {
		WL_LOG_W("IMG", "png decode failed: %s", pi.message);
		free(rgba);
		png_image_free(&pi);
		return -1;
	}

	/* RGB5A3: 2 bytes/texel, 5-bit colour where opaque, 3-bit alpha at the edges. */
	size_t bytes = wl_gxc_tex_bytes(WL_GXC_RGB5A3, pi.width, pi.height);
	img->pixels = memalign(32, bytes);
	if (!img->pixels || wl_gxc_encode(WL_GXC_RGB5A3, rgba, pi.width, pi.height, img->pixels, bytes) == 0) {
		free(rgba);
		free(img->pixels);
		img->pixels = NULL;
		return -1;
	}
	free(rgba);
	wl_gpu_buffer_commit(img->pixels, bytes);
	img->w = (int)pi.width;
	img->h = (int)pi.height;
	if (wl_gx_tex_init(&img->tex, img->pixels, pi.width, pi.height, WL_GX_TEX_RGB5A3, false, smooth != 0) != WL_OK) {
		image_free(img);
		return -1;
	}
	return 0;
}

struct jpeg_err {
	struct jpeg_error_mgr mgr;
	jmp_buf jump;
};

static void jpeg_fail(j_common_ptr cinfo)
{
	struct jpeg_err *e = (struct jpeg_err *)cinfo->err;
	char msg[JMSG_LENGTH_MAX];
	cinfo->err->format_message(cinfo, msg);
	WL_LOG_W("IMG", "jpeg: %s", msg);
	longjmp(e->jump, 1);
}

static void jpeg_quiet(j_common_ptr cinfo, int level)
{
	(void)cinfo;
	(void)level; /* warnings on slightly broken files aren't worth a log line each */
}

int image_load_jpeg(struct image *img, const void *jpg, size_t size, int max_w, int max_h)
{
	memset(img, 0, sizeof *img);
	/* Copied before setjmp: arguments may be clobbered by a longjmp from libjpeg. */
	volatile const unsigned lim_w = max_w <= 0 || max_w > 1024 ? 1024u : (unsigned)max_w;
	volatile const unsigned lim_h = max_h <= 0 || max_h > 1024 ? 1024u : (unsigned)max_h;
	struct jpeg_decompress_struct cinfo;
	struct jpeg_err err;
	uint8_t *rgba = NULL;
	volatile int ok = 0;
	cinfo.err = jpeg_std_error(&err.mgr);
	err.mgr.error_exit = jpeg_fail;
	err.mgr.emit_message = jpeg_quiet;
	if (setjmp(err.jump)) {
		jpeg_destroy_decompress(&cinfo);
		free(rgba);
		return -1;
	}
	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, jpg, (unsigned long)size);
	jpeg_read_header(&cinfo, TRUE);
	cinfo.out_color_space = JCS_EXT_RGBA;
	cinfo.scale_num = 1;
	cinfo.scale_denom = 1;
	while (cinfo.scale_denom < 8 && (cinfo.image_width / cinfo.scale_denom > lim_w ||
					       cinfo.image_height / cinfo.scale_denom > lim_h))
		cinfo.scale_denom *= 2;
	cinfo.dct_method = JDCT_IFAST;
	jpeg_start_decompress(&cinfo);
	uint32_t w = cinfo.output_width, h = cinfo.output_height;
	if (w > 1024 || h > 1024) {
		WL_LOG_W("IMG", "jpeg %ux%u too large even at 1/8", (unsigned)w, (unsigned)h);
		jpeg_abort_decompress(&cinfo);
		jpeg_destroy_decompress(&cinfo);
		return -1;
	}
	rgba = malloc((size_t)w * h * 4);
	if (rgba) {
		while (cinfo.output_scanline < h) {
			JSAMPROW row = rgba + (size_t)cinfo.output_scanline * w * 4;
			jpeg_read_scanlines(&cinfo, &row, 1);
		}
		jpeg_finish_decompress(&cinfo);
		ok = 1;
	}
	jpeg_destroy_decompress(&cinfo);
	if (!ok)
		return -1;

	size_t bytes = wl_gxc_tex_bytes(WL_GXC_RGB565, w, h);
	img->pixels = memalign(32, bytes);
	if (!img->pixels || wl_gxc_encode(WL_GXC_RGB565, rgba, w, h, img->pixels, bytes) == 0) {
		free(rgba);
		image_free(img);
		return -1;
	}
	free(rgba);
	wl_gpu_buffer_commit(img->pixels, bytes);
	img->w = (int)w;
	img->h = (int)h;
	if (wl_gx_tex_init(&img->tex, img->pixels, w, h, WL_GX_TEX_RGB565, false, true) != WL_OK) {
		image_free(img);
		return -1;
	}
	return 0;
}

int image_load_any(struct image *img, const void *data, size_t size, int max_w, int max_h)
{
	const uint8_t *b = data;
	if (size >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
		return image_load_jpeg(img, data, size, max_w, max_h);
	if (size >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0)
		return image_load_png(img, data, size, 1);
	memset(img, 0, sizeof *img);
	return -1;
}

void image_free(struct image *img)
{
	free(img->pixels);
	memset(img, 0, sizeof *img);
}
