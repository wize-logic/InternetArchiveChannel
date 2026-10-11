#include "core/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

void settings_defaults(struct settings *s)
{
	memset(s, 0, sizeof *s);
	s->overscan = 5;
	s->fast_h264 = 1;
}

static void set_kv(struct settings *s, const char *k, const char *v)
{
	if (strcmp(k, "overscan") == 0)
		s->overscan = clampi(atoi(v), 0, 15);
	else if (strcmp(k, "fast_h264") == 0)
		s->fast_h264 = atoi(v) != 0;
	else if (strcmp(k, "debug_overlay") == 0)
		s->debug_overlay = atoi(v) != 0;
}

void settings_parse(struct settings *s, const char *text)
{
	char line[256];
	while (*text) {
		size_t n = strcspn(text, "\r\n");
		size_t copy = n < sizeof line - 1 ? n : sizeof line - 1;
		memcpy(line, text, copy);
		line[copy] = '\0';
		text += n;
		while (*text == '\r' || *text == '\n')
			text++;

		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq)
			continue;
		*eq = '\0';
		/* Trim spaces around the key and the value. */
		char *k = line, *v = eq + 1;
		while (*k == ' ')
			k++;
		for (char *e = eq - 1; e >= k && *e == ' '; e--)
			*e = '\0';
		while (*v == ' ')
			v++;
		for (char *e = v + strlen(v) - 1; e >= v && *e == ' '; e--)
			*e = '\0';
		set_kv(s, k, v);
	}
}

int settings_format(const struct settings *s, char *out, int cap)
{
	int n = snprintf(out, (size_t)cap,
		"# Internet Archive Channel settings\n"
		"overscan=%d\n"
		"fast_h264=%d\n"
		"debug_overlay=%d\n",
		s->overscan, s->fast_h264, s->debug_overlay);
	return n < cap ? n : cap - 1;
}

int settings_load(struct settings *s, const char *path)
{
	char buf[2048];
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = '\0';
	settings_parse(s, buf);
	return 0;
}

int settings_save(const struct settings *s, const char *path)
{
	char buf[2048];
	int n = settings_format(s, buf, sizeof buf);
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	size_t w = fwrite(buf, 1, (size_t)n, f);
	return (fclose(f) == 0 && w == (size_t)n) ? 0 : -1;
}
