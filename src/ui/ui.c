#include "ui/ui.h"

#include <math.h>
#include <string.h>

#define MAX_WIDGETS 128

struct ui_theme theme;

struct widget {
	int id;
	struct rect r;
	int modal;
};

static struct {
	const struct input_state *in;
	struct widget prev[MAX_WIDGETS], cur[MAX_WIDGETS];
	int nprev, ncur;
	int focus;      /* widget id, 0 = none */
	int hover;      /* id under the pointer (from last frame's layout), 0 = none */
	int in_modal;   /* currently declaring modal widgets */
	int modal_up;   /* a modal was declared last frame */
	int modal_seen; /* ... and this frame */
	int overscan;
	int frame;
	int restored; /* focus set explicitly: keep it even if last frame's layout lacks it */
	int no_default; /* a widget the D-pad reaches but focus never starts on (ui_no_default_focus) */
	int defaulted;  /* focus is there by default, not chosen: it may move once other widgets appear */
	struct {
		const struct image *img; /* first frame */
		int frames, ms;          /* animated: frame count and time per frame */
		int hot_x, hot_y;
	} cursors[UI_CURSOR_COUNT];
	enum ui_cursor shown;   /* last frame's shape */
	uint64_t shown_since_us; /* animations start from their first frame */
	enum ui_cursor cursor; /* this frame's request */
} g;

void ui_set_cursor_image(enum ui_cursor shape, const struct image *img, int hot_x, int hot_y)
{
	if (shape <= UI_CURSOR_AUTO || shape >= UI_CURSOR_COUNT)
		return;
	g.cursors[shape].img = img && img->w ? img : NULL;
	g.cursors[shape].frames = 1;
	g.cursors[shape].ms = 0;
	g.cursors[shape].hot_x = hot_x;
	g.cursors[shape].hot_y = hot_y;
}

void ui_set_cursor_frames(enum ui_cursor shape, const struct image *frames, int count, int ms, int hot_x,
	int hot_y)
{
	ui_set_cursor_image(shape, frames, hot_x, hot_y);
	if (shape <= UI_CURSOR_AUTO || shape >= UI_CURSOR_COUNT || !g.cursors[shape].img)
		return;
	for (int i = 0; i < count; i++)
		if (!frames[i].w)
			count = i; /* stop at a frame that didn't load */
	g.cursors[shape].frames = count > 0 ? count : 1;
	g.cursors[shape].ms = ms > 0 ? ms : 100;
}

void ui_set_cursor(enum ui_cursor shape)
{
	g.cursor = shape;
}

void ui_draw_image_fit(struct rect r, const struct image *img, uint32_t tint)
{
	if (!img || !img->w || !img->h)
		return;
	float sx = r.w / (float)img->w, sy = r.h / (float)img->h, sc = sx < sy ? sx : sy;
	float w = (float)img->w * sc, h = (float)img->h * sc;
	draw_image((struct rect){r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h}, &img->tex, tint);
}

void ui_init(void)
{
	theme.bg_top = RGBA(0x24, 0x26, 0x2b, 0xFF);
	theme.bg_bottom = RGBA(0x10, 0x11, 0x14, 0xFF);
	theme.panel = RGBA(0x2e, 0x31, 0x38, 0xFF);
	theme.panel_hi = RGBA(0x3b, 0x40, 0x4a, 0xFF);
	theme.text = RGBA(0xf2, 0xf2, 0xf2, 0xFF);
	theme.text_dim = RGBA(0x9a, 0xa0, 0xa8, 0xFF);
	theme.accent = RGBA(0x4a, 0x9e, 0xe8, 0xFF);
	theme.accent_dark = RGBA(0x2b, 0x6c, 0xa8, 0xFF);
	theme.danger = RGBA(0xd9, 0x53, 0x4f, 0xFF);
	g.overscan = 5;
}

void ui_set_overscan(int percent)
{
	g.overscan = percent < 0 ? 0 : percent > 15 ? 15 : percent;
}

struct rect ui_safe_area(void)
{
	float w = (float)draw_width(), mx = w * (float)g.overscan / 100.0f,
	      my = UI_H * (float)g.overscan / 100.0f;
	return (struct rect){mx, my, w - 2 * mx, UI_H - 2 * my};
}

const struct input_state *ui_input(void)
{
	return g.in;
}

static struct widget *find(struct widget *list, int n, int id)
{
	for (int i = 0; i < n; i++)
		if (list[i].id == id)
			return &list[i];
	return NULL;
}

void ui_begin(const struct input_state *in)
{
	g.in = in;
	g.frame++;
	memcpy(g.prev, g.cur, sizeof(struct widget) * (size_t)g.ncur);
	g.nprev = g.ncur;
	g.ncur = 0;
	g.modal_up = g.modal_seen;
	g.modal_seen = 0;
	g.cursor = UI_CURSOR_AUTO;

	/* Hover from last frame's layout; topmost (last declared) wins, modal-only when up. */
	g.hover = 0;
	if (in->pointer_valid) {
		for (int i = g.nprev - 1; i >= 0; i--) {
			if ((g.modal_up && !g.prev[i].modal) || !rect_contains(g.prev[i].r, in->px, in->py))
				continue;
			g.hover = g.prev[i].id;
			break;
		}
		if (g.hover && in->pointer_moved) {
			g.focus = g.hover;
			g.defaulted = 0;
		}
	}
	/* Keep focus valid (unless it was just restored for a screen whose widgets come next). Focus
	 * that fell on the no-default widget only because it was alone (a screen still loading) moves
	 * on once there is something else. */
	struct widget *f = find(g.prev, g.nprev, g.focus);
	if (g.restored) {
		g.restored = 0;
	} else if (g.nprev && (!f || (g.modal_up && !f->modal) || (g.defaulted && g.focus == g.no_default && g.nprev > 1))) {
		g.defaulted = 1;
		g.focus = 0;
		for (int i = 0; i < g.nprev; i++)
			if ((!g.modal_up || g.prev[i].modal) && (!g.focus || g.focus == g.no_default)) {
				g.focus = g.prev[i].id;
				if (g.focus != g.no_default)
					break;
			}
	}
}

void ui_modal_begin(void)
{
	g.in_modal = 1;
	g.modal_seen = 1;
	/* Dim everything drawn so far. */
	draw_rect((struct rect){0, 0, (float)draw_width(), UI_H}, RGBA(0, 0, 0, 0xA0));
}

void ui_modal_end(void)
{
	g.in_modal = 0;
}

int ui_modal_active(void)
{
	return g.modal_up;
}

int ui_pressed(uint32_t btn)
{
	/* Outside a modal, buttons are swallowed while one is up. */
	if (g.modal_up && !g.in_modal)
		return 0;
	return (g.in->pressed & btn) != 0;
}

void ui_no_default_focus(int id)
{
	g.no_default = id;
}

void ui_focus(int id)
{
	g.focus = id;
	g.restored = 1;
	g.defaulted = 0;
}

int ui_get_focus(void)
{
	return g.focus;
}

/* Registers a widget; returns 1 if it is activated this frame. Sets *hot when focused. */
static int declare(int id, struct rect r, int *hot)
{
	if (g.ncur < MAX_WIDGETS)
		g.cur[g.ncur++] = (struct widget){id, r, g.in_modal};
	int interactive = !g.modal_up || g.in_modal;
	*hot = interactive && g.focus == id;
	if (!interactive || !(g.in->pressed & BTN_A))
		return 0;
	if (g.in->pointer_valid && g.hover)
		return g.hover == id;
	return g.focus == id;
}

static void frame_box(struct rect r, int hot)
{
	draw_rect(r, hot ? theme.panel_hi : theme.panel);
	if (hot)
		draw_rect_outline(r, 3, theme.accent);
}

int ui_region(int id, struct rect r)
{
	int hot;
	return declare(id, r, &hot);
}

int ui_button(int id, struct rect r, const char *label)
{
	int hot, act = declare(id, r, &hot);
	frame_box(r, hot);
	int tw = font_measure(theme.body, label, -1), lh = font_line_height(theme.body);
	font_draw_clipped(theme.body, r.x + (r.w - (float)(tw < r.w - 16 ? tw : (int)r.w - 16)) / 2,
		r.y + (r.h - (float)lh) / 2, theme.text, label, (int)r.w - 16);
	return act;
}

int ui_button_icon(int id, struct rect r, const char *label, const struct image *icon)
{
	if (!icon)
		return ui_button(id, r, label);
	int hot, act = declare(id, r, &hot);
	frame_box(r, hot);
	float isz = r.h - 14;
	int lh = font_line_height(theme.body);
	int tw = font_measure(theme.body, label, -1);
	float total = isz + 10 + (float)tw, x = r.x + (r.w - total) / 2;
	if (x < r.x + 8)
		x = r.x + 8;
	ui_draw_image_fit((struct rect){x, r.y + 7, isz, isz}, icon, 0xFFFFFFFFu);
	font_draw_clipped(theme.body, x + isz + 10, r.y + (r.h - (float)lh) / 2, theme.text, label,
		(int)(r.x + r.w - (x + isz + 10)) - 8);
	return act;
}

int ui_tile(int id, struct rect r, const char *title, const char *subtitle, uint32_t accent,
	const struct image *icon)
{
	int hot, act = declare(id, r, &hot);
	draw_rect_vgradient(r, hot ? theme.panel_hi : theme.panel, theme.panel);
	draw_rect((struct rect){r.x, r.y, 8, r.h}, accent);
	if (hot)
		draw_rect_outline(r, 3, theme.accent);
	/* Icon left, title (and subtitle when the tile is tall enough) right. Icons are the same
	 * size in every tile of a given height, so a grid looks even on 4:3 and 16:9 alike. */
	float isz = icon ? (r.h - 14 < 56 ? r.h - 14 : 56) : 0;
	float tx = r.x + 22 + (icon ? isz + 12 : 0);
	int maxw = (int)(r.x + r.w - tx) - 10;
	int show_sub = subtitle && r.h >= 66;
	if (icon)
		ui_draw_image_fit((struct rect){r.x + 20, r.y + (r.h - isz) / 2 - (hot ? 2 : 0), isz, isz}, icon,
			0xFFFFFFFFu);
	int text_h = font_line_height(theme.heading) + (show_sub ? font_line_height(theme.small) + 2 : 0);
	float ty = r.y + (r.h - (float)text_h) / 2;
	font_draw_clipped(theme.heading, tx, ty, theme.text, title, maxw);
	if (show_sub)
		font_draw_clipped(theme.small, tx, ty + (float)font_line_height(theme.heading) + 2, theme.text_dim,
			subtitle, maxw);
	return act;
}

int ui_text_field(int id, struct rect r, const char *text, const char *placeholder,
	const struct image *icon)
{
	int hot, act = declare(id, r, &hot);
	draw_rect(r, RGBA(0x16, 0x17, 0x1a, 0xFF));
	draw_rect_outline(r, hot ? 3 : 2, hot ? theme.accent : theme.panel_hi);
	float tx = r.x + 14;
	if (icon) {
		float isz = r.h - 16;
		ui_draw_image_fit((struct rect){r.x + 10, r.y + 8, isz, isz}, icon, 0xFFFFFFFFu);
		tx = r.x + 10 + isz + 10;
	}
	int lh = font_line_height(theme.body);
	int empty = !text || !*text;
	font_draw_clipped(theme.body, tx, r.y + (r.h - (float)lh) / 2, empty ? theme.text_dim : theme.text,
		empty ? placeholder : text, (int)(r.x + r.w - tx) - 14);
	return act;
}

int ui_toggle(int id, struct rect r, const char *label, int *value)
{
	int hot, act = declare(id, r, &hot);
	frame_box(r, hot);
	int lh = font_line_height(theme.body);
	font_draw_clipped(theme.body, r.x + 18, r.y + (r.h - (float)lh) / 2, theme.text, label,
		(int)r.w - 120);
	/* Switch on the right. */
	struct rect sw = {r.x + r.w - 86, r.y + (r.h - 30) / 2, 64, 30};
	draw_rect(sw, *value ? theme.accent : RGBA(0x55, 0x5a, 0x63, 0xFF));
	draw_rect((struct rect){*value ? sw.x + 36 : sw.x + 4, sw.y + 4, 24, 22}, theme.text);
	if (act)
		*value = !*value;
	return act;
}

void ui_hints(const char *text)
{
	struct rect s = ui_safe_area();
	float h = (float)font_line_height(theme.small) + 12;
	struct rect bar = {0, s.y + s.h - h, (float)draw_width(), h + (UI_H - s.y - s.h)};
	draw_rect(bar, RGBA(0, 0, 0, 0x60));
	font_draw(theme.small, s.x + 8, bar.y + 6, theme.text_dim, text, -1);
}

/* Moves focus to the nearest widget whose centre lies in the pressed direction. */
static void navigate(void)
{
	uint32_t p = g.in->pressed;
	int dx = (p & BTN_RIGHT) ? 1 : (p & BTN_LEFT) ? -1 : 0;
	int dy = (p & BTN_DOWN) ? 1 : (p & BTN_UP) ? -1 : 0;
	if (!dx && !dy)
		return;
	struct widget *f = find(g.cur, g.ncur, g.focus);
	if (!f) {
		g.focus = 0;
		for (int i = 0; i < g.ncur && (!g.focus || g.focus == g.no_default); i++)
			g.focus = g.cur[i].id;
		return;
	}
	float fx = f->r.x + f->r.w / 2, fy = f->r.y + f->r.h / 2;
	float best = 1e9f;
	int best_id = 0;
	for (int i = 0; i < g.ncur; i++) {
		struct widget *w = &g.cur[i];
		if (w->id == g.focus || (g.modal_seen && !w->modal))
			continue;
		float wx = w->r.x + w->r.w / 2, wy = w->r.y + w->r.h / 2;
		float along = dx ? (wx - fx) * (float)dx : (wy - fy) * (float)dy;
		float across = dx ? fabsf(wy - fy) : fabsf(wx - fx);
		if (along <= 1.0f)
			continue;
		/* Prefer widgets straight ahead: sideways distance costs more. */
		float score = along + across * 2.5f;
		if (score < best) {
			best = score;
			best_id = w->id;
		}
	}
	if (best_id) {
		g.focus = best_id;
		g.defaulted = 0;
	}
}

/* A dark quad from the centre along angle a (radians, clockwise from 12), len long. */
static void hand(float cx, float cy, float a, float len, float half_w, uint32_t c)
{
	float dx = sinf(a), dy = -cosf(a), nx = -dy * half_w, ny = dx * half_w;
	float bx = cx - dx * 1.5f, by = cy - dy * 1.5f, tx = cx + dx * len, ty = cy + dy * len;
	WlGxVertex v[4] = {
		{bx + nx, by + ny, 0, c, 0, 0},
		{tx + nx, ty + ny, 0, c, 0, 0},
		{tx - nx, ty - ny, 0, c, 0, 0},
		{bx - nx, by - ny, 0, c, 0, 0},
	};
	wl_gx_set_recipe(WL_GX_RECIPE_VERTEX_COLOR);
	wl_gx_draw(WL_GX_QUADS, v, 4);
}

/* The busy clock: the minute hand goes round once a second, the hour hand once in 12. */
static void draw_clock_hands(float cx, float cy, float r)
{
	float t = (float)(wl_platform_time_us() % 12000000u) / 1000000.0f;
	const float two_pi = 6.2831853f;
	uint32_t ink = RGBA(0x22, 0x26, 0x30, 0xFF);
	hand(cx, cy, two_pi * t / 12.0f, r * 0.45f, 1.1f, ink);
	hand(cx, cy, two_pi * t, r * 0.65f, 0.8f, ink);
	draw_rect((struct rect){cx - 1.5f, cy - 1.5f, 3, 3}, theme.danger);
}

static void draw_pointer(void)
{
	if (!g.in->pointer_valid)
		return;
	font_flush();
	float x = g.in->px, y = g.in->py;
	enum ui_cursor shape = g.cursor != UI_CURSOR_AUTO ? g.cursor : g.hover ? UI_CURSOR_HAND : UI_CURSOR_NORMAL;
	if (!g.cursors[shape].img && shape != UI_CURSOR_NORMAL)
		shape = g.cursors[UI_CURSOR_HAND].img && shape == UI_CURSOR_GRAB ? UI_CURSOR_HAND : UI_CURSOR_NORMAL;
	uint64_t now = wl_platform_time_us();
	if (shape != g.shown) {
		g.shown = shape;
		g.shown_since_us = now;
	}
	const struct image *img = g.cursors[shape].img;
	if (img && g.cursors[shape].frames > 1)
		img += (int)((now - g.shown_since_us) / 1000u / (uint64_t)g.cursors[shape].ms) % g.cursors[shape].frames;
	if (img) {
		float x0 = x - (float)g.cursors[shape].hot_x, y0 = y - (float)g.cursors[shape].hot_y;
		draw_image((struct rect){x0, y0, (float)img->w, (float)img->h}, &img->tex, 0xFFFFFFFFu);
		if (shape == UI_CURSOR_WAIT)
			draw_clock_hands(x0 + (float)img->w / 2, y0 + (float)img->h / 2, (float)img->w / 2);
		return;
	}
	/* Arrow: black outline then white body. */
	const float pts[7][2] = {{0, 0}, {0, 26}, {7, 20}, {12, 31}, {17, 29}, {12, 18}, {21, 18}};
	for (int pass = 0; pass < 2; pass++) {
		uint32_t c = pass ? theme.text : RGBA(0, 0, 0, 0xFF);
		float grow = pass ? 0.0f : 2.0f;
		WlGxVertex v[15];
		/* Fan from the tip: (0,1,2) (0,2,6) (2,3,4) (2,4,5) (2,5,6). */
		static const int tri[15] = {0, 1, 2, 0, 2, 6, 2, 3, 4, 2, 4, 5, 2, 5, 6};
		for (int i = 0; i < 15; i++) {
			float px = pts[tri[i]][0], py = pts[tri[i]][1];
			/* Crude outline: scale about an interior point. */
			px = 8 + (px - 8) * (1 + grow / 12);
			py = 16 + (py - 16) * (1 + grow / 18);
			v[i] = (WlGxVertex){x + px, y + py, 0, c, 0, 0};
		}
		wl_gx_set_recipe(WL_GX_RECIPE_VERTEX_COLOR);
		wl_gx_draw(WL_GX_TRIANGLES, v, 15);
	}
}

void ui_end(void)
{
	navigate();
	draw_pointer();
}
