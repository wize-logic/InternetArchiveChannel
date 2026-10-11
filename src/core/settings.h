/* User settings, stored as key=value lines (sd:/apps/iachannel/settings.ini).
 * Unknown keys are ignored and out-of-range values clamped, so old/new builds share a file. */
#ifndef IAC_SETTINGS_H
#define IAC_SETTINGS_H

struct settings {
	int overscan;      /* % of screen kept clear at each edge, 0..15 */
	int fast_h264;     /* skip the H.264 loop filter for 480p video */
	int debug_overlay; /* fps / memory overlay */
};

void settings_defaults(struct settings *s);
/* Parses text into s (on top of whatever it holds). */
void settings_parse(struct settings *s, const char *text);
/* Writes s as text; returns the length (truncated to cap - 1). */
int settings_format(const struct settings *s, char *out, int cap);

int settings_load(struct settings *s, const char *path);
int settings_save(const struct settings *s, const char *path);

#endif
