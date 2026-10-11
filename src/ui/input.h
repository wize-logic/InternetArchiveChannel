/* UI-level input: one logical pointer and a set of logical buttons merged from the first Wii
 * Remote (IR pointer, buttons, Classic Controller), GameCube pads and a USB keyboard. An autotest
 * script can drive the same state, so UI flows run unattended in Dolphin. */
#ifndef IAC_INPUT_H
#define IAC_INPUT_H

#include <stdint.h>

enum {
	BTN_A = 1 << 0,     /* select / activate */
	BTN_B = 1 << 1,     /* back */
	BTN_UP = 1 << 2,
	BTN_DOWN = 1 << 3,
	BTN_LEFT = 1 << 4,
	BTN_RIGHT = 1 << 5,
	BTN_PLUS = 1 << 6,
	BTN_MINUS = 1 << 7,
	BTN_HOME = 1 << 8,
	BTN_1 = 1 << 9,
	BTN_2 = 1 << 10,
};

#define INPUT_TEXT_MAX 32

struct input_state {
	uint32_t held, pressed, released;
	int pointer_valid; /* IR pointing at the screen (or injected) */
	float px, py;      /* UI coordinates */
	int pointer_moved; /* pointer moved since last frame: hover takes focus */
	/* Text typed on a USB keyboard this frame: UTF-8 chars, plus control codes
	 * '\b' backspace, '\n' enter, 0x1B escape. */
	char text[INPUT_TEXT_MAX];
	int text_len;
};

void input_init(void);
/* Polls hardware (or the autotest script) and fills `out`. ui_width maps IR x into UI units. */
void input_poll(struct input_state *out, int ui_width);

/* Autotest: when sd:/apps/iachannel/autotest.txt exists, input comes from it (see autotest.c). */
int input_autotest_active(void);

#endif
