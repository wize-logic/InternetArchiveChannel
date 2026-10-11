#include "ui/font.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "wl/base/utf8.h"
#include "wl/platform/platform.h"

/* I8 atlas: 1024x512 = 512 KiB. GX I8 texels are 8x4 tiles of 32 bytes, tiles row-major. */
#define ATLAS_W 1024
#define ATLAS_H 512
#define GLYPH_SLOTS 2048 /* open-addressed cache, power of two */
#define MAX_FONTS 8
#define BATCH_GLYPHS 256
#define PAD 1

struct ui_font {
	FT_Face face;
	int id;
	int pixel_size;
	int ascender, line_height;
};

struct glyph {
	uint32_t key; /* (font id << 24) | codepoint; 0 = empty slot */
	int16_t x, y;  /* atlas position */
	uint8_t w, h;
	int8_t left, top; /* bearing: pen-x offset, pen-baseline-to-top */
	int16_t advance;
};

static struct {
	FT_Library lib;
	uint8_t *atlas;
	WlGxTexObj tex;
	int shelf_x, shelf_y, shelf_h;
	int dirty;
	struct glyph cache[GLYPH_SLOTS];
	ui_font fonts[MAX_FONTS];
	int font_count;
	WlGxVertex batch[BATCH_GLYPHS * 4];
	int batch_glyphs;
} g;

int font_system_init(void)
{
	if (FT_Init_FreeType(&g.lib) != 0)
		return -1;
	g.atlas = memalign(32, ATLAS_W * ATLAS_H);
	if (!g.atlas)
		return -1;
	memset(g.atlas, 0, ATLAS_W * ATLAS_H);
	wl_gpu_buffer_commit(g.atlas, ATLAS_W * ATLAS_H);
	if (wl_gx_tex_init(&g.tex, g.atlas, ATLAS_W, ATLAS_H, WL_GX_TEX_I8, false, true) != WL_OK)
		return -1;
	return 0;
}

void font_system_shutdown(void)
{
	for (int i = 0; i < g.font_count; i++)
		FT_Done_Face(g.fonts[i].face);
	FT_Done_FreeType(g.lib);
	free(g.atlas);
	memset(&g, 0, sizeof g);
}

ui_font *font_load_mem(const void *data, size_t size, int pixel_size)
{
	if (g.font_count >= MAX_FONTS)
		return NULL;
	ui_font *f = &g.fonts[g.font_count];
	if (FT_New_Memory_Face(g.lib, data, (FT_Long)size, 0, &f->face) != 0)
		return NULL;
	FT_Set_Pixel_Sizes(f->face, 0, (FT_UInt)pixel_size);
	f->id = g.font_count + 1;
	f->pixel_size = pixel_size;
	f->ascender = (int)(f->face->size->metrics.ascender >> 6);
	f->line_height = (int)(f->face->size->metrics.height >> 6);
	g.font_count++;
	return f;
}

int font_line_height(const ui_font *f)
{
	return f->line_height;
}

static void atlas_reset(void)
{
	memset(g.cache, 0, sizeof g.cache);
	memset(g.atlas, 0, ATLAS_W * ATLAS_H);
	g.shelf_x = g.shelf_y = g.shelf_h = 0;
	g.dirty = 1;
}

/* Writes a w x h coverage bitmap into the tiled atlas at (ax, ay). */
static void atlas_blit(int ax, int ay, const uint8_t *src, int pitch, int w, int h)
{
	for (int y = 0; y < h; y++) {
		int ty = ay + y;
		uint8_t *row_tiles = g.atlas + (size_t)(ty >> 2) * (ATLAS_W / 8) * 32 + (ty & 3) * 8;
		for (int x = 0; x < w; x++) {
			int tx = ax + x;
			row_tiles[(tx >> 3) * 32 + (tx & 7)] = src[y * pitch + x];
		}
	}
	g.dirty = 1;
}

static struct glyph *glyph_get(ui_font *f, uint32_t cp)
{
	uint32_t key = ((uint32_t)f->id << 24) | (cp & 0xFFFFFF);
	uint32_t h = (key * 2654435761u) & (GLYPH_SLOTS - 1);
	for (int probe = 0; probe < GLYPH_SLOTS; probe++) {
		struct glyph *gl = &g.cache[(h + (uint32_t)probe) & (GLYPH_SLOTS - 1)];
		if (gl->key == key)
			return gl;
		if (gl->key != 0)
			continue;

		/* Miss: rasterise into this slot. */
		if (FT_Load_Char(f->face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0 &&
			FT_Load_Char(f->face, '?', FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0)
			return NULL;
		FT_GlyphSlot s = f->face->glyph;
		int w = (int)s->bitmap.width, hgt = (int)s->bitmap.rows;
		if (w > 255 || hgt > 255)
			return NULL;
		if (g.shelf_x + w + PAD > ATLAS_W) {
			g.shelf_x = 0;
			g.shelf_y += g.shelf_h + PAD;
			g.shelf_h = 0;
		}
		if (g.shelf_y + hgt + PAD > ATLAS_H) {
			/* Full: start over. Glyphs already queued this frame keep their (now stale) UVs
			 * until the next frame redraws them, which is acceptable for a rare event. */
			font_flush();
			atlas_reset();
			return glyph_get(f, cp);
		}
		gl->key = key;
		gl->x = (int16_t)g.shelf_x;
		gl->y = (int16_t)g.shelf_y;
		gl->w = (uint8_t)w;
		gl->h = (uint8_t)hgt;
		gl->left = (int8_t)s->bitmap_left;
		gl->top = (int8_t)s->bitmap_top;
		gl->advance = (int16_t)(s->advance.x >> 6);
		if (w && hgt)
			atlas_blit(gl->x, gl->y, s->bitmap.buffer, s->bitmap.pitch, w, hgt);
		g.shelf_x += w + PAD;
		if (hgt > g.shelf_h)
			g.shelf_h = hgt;
		return gl;
	}
	return NULL;
}

static uint32_t next_cp(const char **s, const char *end)
{
	uint32_t cp;
	size_t n = wl_utf8_decode(*s, (size_t)(end - *s), &cp);
	if (n == 0) {
		/* Invalid byte: show it as U+FFFD and move on. */
		*s += 1;
		return 0xFFFD;
	}
	*s += n;
	return cp;
}

int font_measure(ui_font *f, const char *utf8, int len)
{
	const char *s = utf8, *end = utf8 + (len < 0 ? (int)strlen(utf8) : len);
	int x = 0;
	while (s < end) {
		struct glyph *gl = glyph_get(f, next_cp(&s, end));
		if (gl)
			x += gl->advance;
	}
	return x;
}

int font_fit(ui_font *f, const char *utf8, int max_width)
{
	const char *s = utf8, *end = utf8 + strlen(utf8);
	int x = 0;
	while (s < end) {
		const char *before = s;
		struct glyph *gl = glyph_get(f, next_cp(&s, end));
		int adv = gl ? gl->advance : 0;
		if (x + adv > max_width)
			return (int)(before - utf8);
		x += adv;
	}
	return (int)(end - utf8);
}

void font_flush(void)
{
	if (g.batch_glyphs == 0)
		return;
	if (g.dirty) {
		wl_gpu_buffer_commit(g.atlas, ATLAS_W * ATLAS_H);
		wl_gx_tex_invalidate();
		g.dirty = 0;
	}
	wl_gx_set_recipe(WL_GX_RECIPE_TEXT);
	wl_gx_set_blend(WL_GX_BLEND_ALPHA);
	wl_gx_tex_bind(&g.tex);
	wl_gx_draw(WL_GX_QUADS, g.batch, (uint32_t)g.batch_glyphs * 4);
	g.batch_glyphs = 0;
}

static void emit(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
	uint32_t rgba)
{
	if (g.batch_glyphs == BATCH_GLYPHS)
		font_flush();
	WlGxVertex *v = &g.batch[g.batch_glyphs * 4];
	v[0] = (WlGxVertex){x0, y0, 0, rgba, u0, v0};
	v[1] = (WlGxVertex){x1, y0, 0, rgba, u1, v0};
	v[2] = (WlGxVertex){x1, y1, 0, rgba, u1, v1};
	v[3] = (WlGxVertex){x0, y1, 0, rgba, u0, v1};
	g.batch_glyphs++;
}

void font_draw(ui_font *f, float x, float y, uint32_t rgba, const char *utf8, int len)
{
	const char *s = utf8, *end = utf8 + (len < 0 ? (int)strlen(utf8) : len);
	float pen = (float)(int)x, base = (float)(int)y + (float)f->ascender;
	while (s < end) {
		struct glyph *gl = glyph_get(f, next_cp(&s, end));
		if (!gl)
			continue;
		if (gl->w && gl->h) {
			float x0 = pen + gl->left, y0 = base - gl->top;
			emit(x0, y0, x0 + gl->w, y0 + gl->h, (float)gl->x / ATLAS_W, (float)gl->y / ATLAS_H,
				(float)(gl->x + gl->w) / ATLAS_W, (float)(gl->y + gl->h) / ATLAS_H, rgba);
		}
		pen += gl->advance;
	}
}

void font_draw_clipped(ui_font *f, float x, float y, uint32_t rgba, const char *utf8, int max_width)
{
	if (font_measure(f, utf8, -1) <= max_width) {
		font_draw(f, x, y, rgba, utf8, -1);
		return;
	}
	int dots = font_measure(f, "...", 3);
	int n = max_width > dots ? font_fit(f, utf8, max_width - dots) : 0;
	font_draw(f, x, y, rgba, utf8, n);
	font_draw(f, x + (float)font_measure(f, utf8, n), y, rgba, "...", 3);
}

int font_draw_wrapped(ui_font *f, float x, float y, int width, uint32_t rgba, const char *utf8)
{
	const char *line = utf8;
	int lines = 0;
	while (*line) {
		if (*line == '\n') { /* blank line */
			lines++;
			line++;
			continue;
		}
		/* Take words while they fit; a single over-long word is split by font_fit. */
		const char *p = line, *last_break = NULL;
		while (*p && *p != '\n') {
			const char *word_end = p;
			while (*word_end && *word_end != ' ' && *word_end != '\n')
				word_end++;
			if (font_measure(f, line, (int)(word_end - line)) > width)
				break;
			last_break = word_end;
			p = *word_end == ' ' ? word_end + 1 : word_end;
		}
		const char *cut = last_break ? last_break : line + font_fit(f, line, width);
		if (cut == line)
			cut = line + 1; /* always make progress */
		font_draw(f, x, y + (float)(lines * f->line_height), rgba, line, (int)(cut - line));
		lines++;
		line = cut;
		while (*line == ' ')
			line++;
		if (*line == '\n')
			line++;
	}
	return lines * f->line_height;
}
