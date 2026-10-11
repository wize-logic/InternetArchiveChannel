#include "ui/draw.h"

#include "ui/font.h"

static int g_width = 640;
static WlVideoInfo g_video;

void draw_init(int widescreen)
{
	g_width = widescreen ? 854 : 640;
	wl_platform_video_info(&g_video);
}

int draw_width(void)
{
	return g_width;
}

void draw_begin_frame(void)
{
	wl_gx_set_ortho(0.0f, 0.0f, (float)g_width, (float)UI_H, -1.0f, 1.0f);
	wl_gx_set_cull(WL_GX_CULL_NONE);
	wl_gx_set_zmode(false, WL_GX_ALWAYS, false);
	wl_gx_set_blend(WL_GX_BLEND_ALPHA);
	draw_clip_reset();
}

void draw_end_frame(void)
{
	font_flush();
}

static void quad(struct rect r, uint32_t c0, uint32_t c1, float u1, float v1)
{
	WlGxVertex v[4] = {
		{r.x, r.y, 0, c0, 0, 0},
		{r.x + r.w, r.y, 0, c0, u1, 0},
		{r.x + r.w, r.y + r.h, 0, c1, u1, v1},
		{r.x, r.y + r.h, 0, c1, 0, v1},
	};
	wl_gx_draw(WL_GX_QUADS, v, 4);
}

void draw_rect(struct rect r, uint32_t rgba)
{
	draw_rect_vgradient(r, rgba, rgba);
}

void draw_rect_vgradient(struct rect r, uint32_t top, uint32_t bottom)
{
	font_flush(); /* keep painter's order between queued text and shapes */
	wl_gx_set_recipe(WL_GX_RECIPE_VERTEX_COLOR);
	quad(r, top, bottom, 0, 0);
}

void draw_rect_outline(struct rect r, float t, uint32_t rgba)
{
	draw_rect((struct rect){r.x, r.y, r.w, t}, rgba);
	draw_rect((struct rect){r.x, r.y + r.h - t, r.w, t}, rgba);
	draw_rect((struct rect){r.x, r.y + t, t, r.h - 2 * t}, rgba);
	draw_rect((struct rect){r.x + r.w - t, r.y + t, t, r.h - 2 * t}, rgba);
}

void draw_image(struct rect r, const WlGxTexObj *tex, uint32_t tint)
{
	font_flush();
	wl_gx_set_recipe(WL_GX_RECIPE_TEX_MODULATE);
	wl_gx_tex_bind(tex);
	quad(r, tint, tint, 1, 1);
}

void draw_clip(struct rect r)
{
	font_flush();
	/* UI units -> EFB pixels (the canvas is scaled to fb_width x efb_height). */
	float sx = (float)g_video.fb_width / (float)g_width, sy = (float)g_video.efb_height / UI_H;
	int x0 = (int)(r.x * sx), y0 = (int)(r.y * sy);
	int x1 = (int)((r.x + r.w) * sx + 0.999f), y1 = (int)((r.y + r.h) * sy + 0.999f);
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	wl_gx_set_scissor(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
}

void draw_clip_reset(void)
{
	font_flush();
	wl_gx_set_scissor(0, 0, g_video.fb_width, g_video.efb_height);
}
