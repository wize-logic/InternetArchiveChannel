#include "ui/input.h"

#include <math.h>
#include <string.h>

#include "ui/autotest.h"
#include "wl/platform/platform.h"

static struct {
	uint32_t prev_held;
	float px, py;
	int have_pointer;
	int autotest;
} g;

void input_init(void)
{
	wl_platform_input_init();
	g.autotest = autotest_load("sd:/apps/iachannel/autotest.txt");
}

int input_autotest_active(void)
{
	return g.autotest;
}

static uint32_t map_remote(const WlRawRemote *r)
{
	uint32_t b = r->buttons, out = 0;
	/* The pointer-holding Remote is used upright: its D-pad maps directly. */
	if (b & (WL_WM_A | WL_CC_A))
		out |= BTN_A;
	if (b & (WL_WM_B | WL_CC_B))
		out |= BTN_B;
	if (b & (WL_WM_UP | WL_CC_UP))
		out |= BTN_UP;
	if (b & (WL_WM_DOWN | WL_CC_DOWN))
		out |= BTN_DOWN;
	if (b & (WL_WM_LEFT | WL_CC_LEFT))
		out |= BTN_LEFT;
	if (b & (WL_WM_RIGHT | WL_CC_RIGHT))
		out |= BTN_RIGHT;
	if (b & (WL_WM_PLUS | WL_CC_PLUS))
		out |= BTN_PLUS;
	if (b & (WL_WM_MINUS | WL_CC_MINUS))
		out |= BTN_MINUS;
	if (b & (WL_WM_HOME | WL_CC_HOME))
		out |= BTN_HOME;
	if (b & WL_WM_1)
		out |= BTN_1;
	if (b & WL_WM_2)
		out |= BTN_2;
	return out;
}

static uint32_t map_pad(const WlRawPad *p)
{
	uint32_t b = p->buttons, out = 0;
	if (b & WL_PAD_A)
		out |= BTN_A;
	if (b & WL_PAD_B)
		out |= BTN_B;
	if (b & WL_PAD_UP)
		out |= BTN_UP;
	if (b & WL_PAD_DOWN)
		out |= BTN_DOWN;
	if (b & WL_PAD_LEFT)
		out |= BTN_LEFT;
	if (b & WL_PAD_RIGHT)
		out |= BTN_RIGHT;
	if (b & WL_PAD_START)
		out |= BTN_PLUS;
	if (b & WL_PAD_Z)
		out |= BTN_MINUS;
	/* Main stick as a D-pad, with a dead zone. */
	if (p->stick[0][1] > 64)
		out |= BTN_UP;
	if (p->stick[0][1] < -64)
		out |= BTN_DOWN;
	if (p->stick[0][0] < -64)
		out |= BTN_LEFT;
	if (p->stick[0][0] > 64)
		out |= BTN_RIGHT;
	return out;
}

/* USB HID usage -> character (US layout). */
static char hid_char(uint16_t code, int shift)
{
	static const char lower[] = "abcdefghijklmnopqrstuvwxyz1234567890";
	static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()";
	if (code >= 0x04 && code <= 0x27)
		return (shift ? upper : lower)[code - 0x04];
	switch (code) {
	case 0x28: return '\n';
	case 0x29: return 0x1B;
	case 0x2A: return '\b';
	case 0x2C: return ' ';
	case 0x2D: return shift ? '_' : '-';
	case 0x2E: return shift ? '+' : '=';
	case 0x33: return shift ? ':' : ';';
	case 0x34: return shift ? '"' : '\'';
	case 0x36: return shift ? '<' : ',';
	case 0x37: return shift ? '>' : '.';
	case 0x38: return shift ? '?' : '/';
	default: return 0;
	}
}

void input_poll(struct input_state *out, int ui_width)
{
	uint32_t held = 0;
	float old_x = g.px, old_y = g.py;
	memset(out, 0, sizeof *out);

	if (g.autotest) {
		struct autotest_frame f;
		autotest_step(&f);
		held = f.held;
		if (f.pointer_valid) {
			g.px = f.px;
			g.py = f.py;
		}
		out->pointer_valid = f.pointer_valid;
		memcpy(out->text, f.text, sizeof out->text);
		out->text_len = f.text_len;
	} else {
		WlRawInput raw;
		wl_platform_input_poll(&raw);
		for (unsigned i = 0; i < WL_INPUT_MAX_REMOTES; i++)
			if (raw.remotes[i].connected)
				held |= map_remote(&raw.remotes[i]);
		for (unsigned i = 0; i < WL_INPUT_MAX_PADS; i++)
			if (raw.pads[i].connected)
				held |= map_pad(&raw.pads[i]);

		const WlRawRemote *r0 = &raw.remotes[0];
		if (r0->connected && r0->ir_valid && r0->ir_x >= 0 && r0->ir_x < 640 && r0->ir_y >= 0 &&
			r0->ir_y < 480) {
			/* Light smoothing: the raw IR position jitters by a pixel or two. */
			float tx = (float)r0->ir_x * (float)ui_width / 640.0f, ty = (float)r0->ir_y;
			if (!g.have_pointer) {
				g.px = tx;
				g.py = ty;
			} else {
				g.px += (tx - g.px) * 0.5f;
				g.py += (ty - g.py) * 0.5f;
			}
			g.have_pointer = 1;
			out->pointer_valid = 1;
		} else {
			g.have_pointer = 0;
		}

		const WlRawKeyboard *kb = &raw.keyboard;
		for (int i = 0; i < kb->count && out->text_len < INPUT_TEXT_MAX - 1; i++) {
			if (!kb->keys[i].down)
				continue;
			char c = hid_char(kb->keys[i].code, (kb->keys[i].modifiers & 0x22) != 0);
			if (c)
				out->text[out->text_len++] = c;
		}
	}

	out->held = held;
	out->pressed = held & ~g.prev_held;
	out->released = g.prev_held & ~held;
	g.prev_held = held;
	out->px = g.px;
	out->py = g.py;
	out->pointer_moved = out->pointer_valid && (fabsf(g.px - old_x) > 0.5f || fabsf(g.py - old_y) > 0.5f);
}
