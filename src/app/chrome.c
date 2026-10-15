#include "app/app.h"

/* A generic "archive building" mark: pediment, columns, base. Drawn, not an image, so it
 * scales and isn't anyone's logo. */
static void draw_mark(float x, float y, float s, uint32_t c)
{
	draw_rect((struct rect){x, y + 6 * s, 40 * s, 4 * s}, c);           /* roof beam */
	draw_rect((struct rect){x + 4 * s, y + 2 * s, 32 * s, 4 * s}, c);   /* pediment */
	draw_rect((struct rect){x + 12 * s, y, 16 * s, 2 * s}, c);
	for (int i = 0; i < 5; i++)                                          /* columns */
		draw_rect((struct rect){x + (3 + (float)i * 7.75f) * s, y + 12 * s, 3 * s, 18 * s}, c);
	draw_rect((struct rect){x, y + 32 * s, 40 * s, 4 * s}, c);          /* base */
}

#define ID_APP_BACK 59000

static int g_back; /* the chrome's Back button, this frame */

int app_back_button(struct rect r)
{
	ui_no_default_focus(ID_APP_BACK);
	return ui_button(ID_APP_BACK, r, "Back");
}

int app_back(void)
{
	int b = g_back;
	g_back = 0;
	return b;
}

struct rect app_chrome(const char *title, const char *subtitle)
{
	float w = (float)draw_width();
	draw_rect_vgradient((struct rect){0, 0, w, UI_H}, theme.bg_top, theme.bg_bottom);
	struct rect s = ui_safe_area();
	float tx = s.x + 62;
	g_back = 0;
	if (app_depth() > 1) {
		g_back = app_back_button((struct rect){s.x, s.y + 4, APP_BACK_W, 46});
		tx = s.x + APP_BACK_W + 14;
	} else if (art.logo.w) {
		ui_draw_image_fit((struct rect){s.x, s.y, 52, 52}, &art.logo, 0xFFFFFFFFu);
	} else {
		draw_mark(s.x + 4, s.y + 6, 1.1f, theme.text);
	}
	font_draw_clipped(theme.title, tx, s.y + 2, theme.text, title, (int)(s.x + s.w - tx));
	if (subtitle)
		font_draw_clipped(theme.small, tx, s.y + 2 + (float)font_line_height(theme.title) - 4,
			theme.text_dim, subtitle, (int)(s.x + s.w - tx));
	float top = s.y + 78;
	draw_rect((struct rect){s.x, top - 10, s.w, 2}, RGBA(0xff, 0xff, 0xff, 0x18));
	float hint_h = (float)font_line_height(theme.small) + 12;
	return (struct rect){s.x, top, s.w, s.h - (top - s.y) - hint_h - 8};
}

void app_spinner(struct rect area, const char *message)
{
	/* Eight dots around a circle; the bright one walks round once a second. */
	static uint32_t tick;
	tick++;
	float cx = area.x + area.w / 2, cy = area.y + area.h / 2 - 20;
	for (int i = 0; i < 8; i++) {
		float a = (float)i * 0.785398f;
		int age = (int)((tick / 8 + 8 - (uint32_t)i) % 8);
		uint8_t alpha = (uint8_t)(255 - age * 28);
		draw_rect((struct rect){cx + 22 * __builtin_cosf(a) - 4, cy + 22 * __builtin_sinf(a) - 4, 8, 8},
			rgba_alpha(theme.accent, alpha));
	}
	int w = font_measure(theme.body, message, -1);
	font_draw(theme.body, cx - (float)w / 2, cy + 40, theme.text_dim, message, -1);
}
