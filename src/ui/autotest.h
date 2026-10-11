/* Scripted input for unattended UI runs (Dolphin frame dumps, regression checks).
 *
 * Script (one command per line, '#' comments):
 *   wait N            idle N frames
 *   waitms N          idle N milliseconds of wall-clock time (for slow software rendering)
 *   point X Y         move the pointer to UI coordinates (stays valid until "nopoint")
 *   nopoint           pointer off-screen
 *   press BTN         hold BTN for 2 frames: A B UP DOWN LEFT RIGHT PLUS MINUS HOME 1 2
 *   click X Y         point X Y, then press A
 *   hold BTN          keep BTN down while the following commands run, until "release BTN"
 *   release BTN
 *   type TEXT         USB-keyboard text for one frame (\n = enter, \b = backspace)
 *   log TEXT          write TEXT to the log (marks a moment for frame-dump correlation)
 *   quit              ask the app to exit
 * Each command after a wait/press takes effect on the following frame. */
#ifndef IAC_AUTOTEST_H
#define IAC_AUTOTEST_H

#include <stdint.h>

#include "ui/input.h"

struct autotest_frame {
	uint32_t held;
	int pointer_valid;
	float px, py;
	char text[INPUT_TEXT_MAX];
	int text_len;
};

/* Returns 1 if a script was loaded. */
int autotest_load(const char *path);
void autotest_step(struct autotest_frame *out);
int autotest_quit_requested(void);

#endif
