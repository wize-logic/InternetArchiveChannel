#include <stdio.h>

#include "app/app.h"

enum { ID_FAST = 1, ID_OVERLAY, ID_OS_MINUS, ID_OS_PLUS, ID_BACK };

static void enter(void *arg)
{
	(void)arg;
}

static void frame(void)
{
	struct rect c = app_chrome("Settings", NULL);
	float y = c.y, h = 54, gap = 10;
	int changed = 0;

	changed |= ui_toggle(ID_FAST, (struct rect){c.x, y, c.w, h},
		"Faster 480p video (slightly softer picture)", &app_settings.fast_h264);
	y += h + gap;
	changed |= ui_toggle(ID_OVERLAY, (struct rect){c.x, y, c.w, h}, "Show performance overlay",
		&app_settings.debug_overlay);
	y += h + gap;

	/* Overscan: label, value, -/+ buttons. */
	struct rect row = {c.x, y, c.w, h};
	draw_rect(row, theme.panel);
	char label[64];
	snprintf(label, sizeof label, "Screen border (overscan): %d%%", app_settings.overscan);
	font_draw(theme.body, row.x + 18, row.y + (h - (float)font_line_height(theme.body)) / 2,
		theme.text, label, -1);
	if (ui_button(ID_OS_MINUS, (struct rect){row.x + row.w - 128, row.y + 6, 54, h - 12}, "-") &&
		app_settings.overscan > 0) {
		app_settings.overscan--;
		changed = 1;
	}
	if (ui_button(ID_OS_PLUS, (struct rect){row.x + row.w - 66, row.y + 6, 54, h - 12}, "+") &&
		app_settings.overscan < 15) {
		app_settings.overscan++;
		changed = 1;
	}
	if (changed) {
		ui_set_overscan(app_settings.overscan);
		app_save_settings();
	}

	if (app_back())
		app_pop();
	ui_hints("(A) Change");
}

const struct screen screen_settings = {"settings", enter, frame};
