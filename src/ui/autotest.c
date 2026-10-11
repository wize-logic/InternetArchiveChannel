#include "ui/autotest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "wl/base/base.h"
#include "wl/platform/platform.h"

#define MAX_SCRIPT 32768

static struct {
	char *script;
	char *pos;
	int wait;
	int press_frames;
	uint32_t press_btn;
	uint32_t hold_btn; /* hold/release: buttons kept down while later commands run */
	int pointer_valid;
	float px, py;
	int quit;
	uint64_t wait_until_us; /* waitms: wall-clock wait (slow renderers draw few frames) */
} g;

int autotest_load(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	g.script = calloc(1, MAX_SCRIPT);
	size_t n = g.script ? fread(g.script, 1, MAX_SCRIPT - 1, f) : 0;
	fclose(f);
	if (n == 0) {
		free(g.script);
		g.script = NULL;
		return 0;
	}
	g.pos = g.script;
	WL_LOG_I("TEST", "autotest script %s (%u bytes)", path, (unsigned)n);
	return 1;
}

int autotest_quit_requested(void)
{
	return g.quit;
}

static uint32_t button_from_name(const char *s)
{
	static const struct {
		const char *name;
		uint32_t bit;
	} map[] = {
		{"A", BTN_A}, {"B", BTN_B}, {"UP", BTN_UP}, {"DOWN", BTN_DOWN}, {"LEFT", BTN_LEFT},
		{"RIGHT", BTN_RIGHT}, {"PLUS", BTN_PLUS}, {"MINUS", BTN_MINUS}, {"HOME", BTN_HOME},
		{"1", BTN_1}, {"2", BTN_2},
	};
	for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
		if (strcasecmp(s, map[i].name) == 0)
			return map[i].bit;
	return 0;
}

/* Copies TEXT, turning the escapes \n and \b into control characters. */
static int unescape(const char *in, char *out, int cap)
{
	int n = 0;
	for (; *in && n < cap - 1; in++) {
		if (in[0] == '\\' && in[1] == 'n') {
			out[n++] = '\n';
			in++;
		} else if (in[0] == '\\' && in[1] == 'b') {
			out[n++] = '\b';
			in++;
		} else {
			out[n++] = *in;
		}
	}
	out[n] = '\0';
	return n;
}

void autotest_step(struct autotest_frame *out)
{
	memset(out, 0, sizeof *out);
	out->held = g.hold_btn;
	if (g.press_frames > 0) {
		out->held |= g.press_btn;
		g.press_frames--;
	}
	if (g.wait_until_us && wl_platform_time_us() < g.wait_until_us)
		goto out;
	g.wait_until_us = 0;
	/* Run commands until one consumes frames. */
	while (g.wait == 0 && g.press_frames == 0 && !g.quit && g.pos && *g.pos) {
		char line[256];
		char *nl = strchr(g.pos, '\n');
		size_t len = nl ? (size_t)(nl - g.pos) : strlen(g.pos);
		if (len >= sizeof line)
			len = sizeof line - 1;
		memcpy(line, g.pos, len);
		line[len] = '\0';
		g.pos = nl ? nl + 1 : g.pos + strlen(g.pos);
		char *cr = strchr(line, '\r');
		if (cr)
			*cr = '\0';
		if (line[0] == '#' || line[0] == '\0')
			continue;

		char cmd[16] = {0};
		const char *arg = line;
		sscanf(line, "%15s", cmd);
		arg = line + strlen(cmd);
		while (*arg == ' ')
			arg++;

		if (strcmp(cmd, "wait") == 0) {
			g.wait = atoi(arg);
		} else if (strcmp(cmd, "waitms") == 0) {
			g.wait_until_us = wl_platform_time_us() + (uint64_t)atoi(arg) * 1000u;
			break;
		} else if (strcmp(cmd, "point") == 0 || strcmp(cmd, "click") == 0) {
			if (sscanf(arg, "%f %f", &g.px, &g.py) == 2)
				g.pointer_valid = 1;
			if (cmd[0] == 'c') {
				g.press_btn = BTN_A;
				g.press_frames = 2;
				g.wait = 1; /* release frame */
			} else {
				g.wait = 1; /* let hover settle */
			}
		} else if (strcmp(cmd, "nopoint") == 0) {
			g.pointer_valid = 0;
		} else if (strcmp(cmd, "press") == 0) {
			g.press_btn = button_from_name(arg);
			g.press_frames = 2;
			g.wait = 1;
		} else if (strcmp(cmd, "hold") == 0) {
			g.hold_btn |= button_from_name(arg);
			g.wait = 1;
		} else if (strcmp(cmd, "release") == 0) {
			g.hold_btn &= ~button_from_name(arg);
			g.wait = 1;
		} else if (strcmp(cmd, "type") == 0) {
			out->text_len = unescape(arg, out->text, INPUT_TEXT_MAX);
			g.wait = 1;
		} else if (strcmp(cmd, "log") == 0) {
			/* Frame number = Dolphin's framedump index (roughly): pairs a moment with a dump. */
			WL_LOG_I("TEST", "frame %u: %s", wl_platform_video_frame_count(), arg);
		} else if (strcmp(cmd, "quit") == 0) {
			WL_LOG_I("TEST", "autotest: quit");
			g.quit = 1;
		} else {
			WL_LOG_W("TEST", "autotest: unknown command '%s'", cmd);
		}
	}
	if (g.wait > 0)
		g.wait--;
out:
	out->pointer_valid = g.pointer_valid;
	out->px = g.px;
	out->py = g.py;
}
