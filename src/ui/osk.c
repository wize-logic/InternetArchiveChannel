#include "ui/osk.h"

#include <string.h>

#include "ui/ui.h"

#define OSK_MAX 256
#define ID_BASE 9000

enum { K_SHIFT = 1, K_SYMBOLS, K_SPACE, K_BACK, K_DONE };

static struct {
	int open;
	char title[64];
	char text[OSK_MAX];
	size_t max;
	int shift, symbols;
	int first_frame;
} g;

static const char *const rows_alpha[4] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-"};
static const char *const rows_sym[4] = {"1234567890", "!@#$%^&*()", "_+=:;\"/?[]", "<>{}~`|\\,."};

void osk_open(const char *title, const char *initial, size_t max_len)
{
	memset(&g, 0, sizeof g);
	g.open = 1;
	g.first_frame = 1;
	g.max = max_len < OSK_MAX - 1 ? max_len : OSK_MAX - 1;
	strncpy(g.title, title, sizeof g.title - 1);
	strncpy(g.text, initial ? initial : "", g.max);
}

int osk_is_open(void)
{
	return g.open;
}

const char *osk_text(void)
{
	return g.text;
}

static void put_char(char c)
{
	size_t n = strlen(g.text);
	if (n < g.max) {
		g.text[n] = c;
		g.text[n + 1] = '\0';
	}
}

static void backspace(void)
{
	size_t n = strlen(g.text);
	/* Remove one UTF-8 character: drop continuation bytes, then the lead byte. */
	while (n > 0 && ((unsigned char)g.text[n - 1] & 0xC0) == 0x80)
		g.text[--n] = '\0';
	if (n > 0)
		g.text[n - 1] = '\0';
}

int osk_update(void)
{
	if (!g.open)
		return OSK_CANCELLED;
	int result = OSK_EDITING;

	ui_modal_begin();
	float key = 54, gap = 6, cols = 10;
	float kb_w = cols * key + (cols - 1) * gap;
	float x0 = ((float)draw_width() - kb_w) / 2;
	float panel_h = 5 * (key + gap) + 120;
	float y0 = (UI_H - panel_h) / 2;
	draw_rect((struct rect){x0 - 24, y0, kb_w + 48, panel_h}, RGBA(0x1c, 0x1e, 0x22, 0xF4));
	draw_rect_outline((struct rect){x0 - 24, y0, kb_w + 48, panel_h}, 2, theme.panel_hi);

	font_draw(theme.small, x0, y0 + 10, theme.text_dim, g.title, -1);
	ui_text_field(ID_BASE, (struct rect){x0, y0 + 36, kb_w, 46}, g.text, "", NULL);
	/* Caret after the text. */
	int tw = font_measure(theme.body, g.text, -1);
	if (tw < kb_w - 40)
		draw_rect((struct rect){x0 + 14 + (float)tw + 2, y0 + 46, 2, 26}, theme.accent);

	float ky = y0 + 96;
	const char *const *rows = g.symbols ? rows_sym : rows_alpha;
	for (int r = 0; r < 4; r++) {
		const char *row = rows[r];
		for (int c = 0; row[c]; c++) {
			char ch = row[c];
			if (g.shift && ch >= 'a' && ch <= 'z')
				ch = (char)(ch - 'a' + 'A');
			char label[2] = {ch, 0};
			struct rect kr = {x0 + (float)c * (key + gap), ky + (float)r * (key + gap), key, key};
			if (ui_button(ID_BASE + 100 + r * 16 + c, kr, label)) {
				put_char(ch);
				if (g.shift == 1)
					g.shift = 0; /* one-shot shift */
			}
		}
	}
	float by = ky + 4 * (key + gap);
	float wide = 2 * key + gap;
	if (ui_button(ID_BASE + K_SHIFT, (struct rect){x0, by, wide, key}, g.shift ? "SHIFT" : "Shift"))
		g.shift = !g.shift;
	if (ui_button(ID_BASE + K_SYMBOLS, (struct rect){x0 + wide + gap, by, wide, key},
			g.symbols ? "ABC" : "#+="))
		g.symbols = !g.symbols;
	if (ui_button(ID_BASE + K_SPACE, (struct rect){x0 + 2 * (wide + gap), by, wide + key + gap, key},
			"Space"))
		put_char(' ');
	if (ui_button(ID_BASE + K_BACK, (struct rect){x0 + 3 * (wide + gap) + key + gap, by, key, key},
			"<-"))
		backspace();
	float done_x = 3 * (wide + gap) + 2 * (key + gap); /* after Back: the rest of the row */
	if (ui_button(ID_BASE + K_DONE, (struct rect){x0 + done_x, by, kb_w - done_x, key}, "Done"))
		result = OSK_DONE;

	/* USB keyboard and controller shortcuts. */
	const struct input_state *in = ui_input();
	for (int i = 0; i < in->text_len; i++) {
		char c = in->text[i];
		if (c == '\b')
			backspace();
		else if (c == '\n')
			result = OSK_DONE;
		else if (c == 0x1B)
			result = OSK_CANCELLED;
		else if ((unsigned char)c >= 0x20)
			put_char(c);
	}
	if (!g.first_frame) {
		if (ui_pressed(BTN_PLUS))
			result = OSK_DONE;
		else if (ui_pressed(BTN_B))
			result = OSK_CANCELLED;
		else if (ui_pressed(BTN_MINUS))
			backspace();
	}
	if (g.first_frame) {
		ui_focus(ID_BASE + 100 + 16 + 0); /* start on "q" */
		g.first_frame = 0;
	}
	ui_modal_end();

	if (result != OSK_EDITING)
		g.open = 0;
	return result;
}
