/* Text rendering: FreeType glyphs rasterised into one shared I8 atlas texture and drawn as
 * batched quads with WiiLibs' TEXT recipe (colour from the vertex, alpha from the glyph).
 * Coordinates are UI pixels, y down; a string's y is the top of its line box. */
#ifndef IAC_FONT_H
#define IAC_FONT_H

#include <stddef.h>
#include <stdint.h>

typedef struct ui_font ui_font;

int font_system_init(void);
void font_system_shutdown(void);

/* `data` must stay valid for the font's lifetime (FreeType reads it lazily). */
ui_font *font_load_mem(const void *data, size_t size, int pixel_size);

int font_line_height(const ui_font *f);
/* Width in pixels of `len` bytes of UTF-8 (len < 0: up to the NUL). */
int font_measure(ui_font *f, const char *utf8, int len);
/* Byte length of the longest prefix of `utf8` that fits in max_width. */
int font_fit(ui_font *f, const char *utf8, int max_width);

void font_draw(ui_font *f, float x, float y, uint32_t rgba, const char *utf8, int len);
/* Draws, truncating with "..." when wider than max_width. */
void font_draw_clipped(ui_font *f, float x, float y, uint32_t rgba, const char *utf8, int max_width);
/* Word-wraps into a box; returns the height used. */
int font_draw_wrapped(ui_font *f, float x, float y, int width, uint32_t rgba, const char *utf8);

/* Submits queued glyph quads. The draw layer calls this before switching render state. */
void font_flush(void);

#endif
