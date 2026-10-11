/* Wayback bookmarks: sd:/apps/iachannel/bookmarks.txt, one per line as "timestamp<TAB>url<TAB>title". */
#include <stdio.h>
#include <string.h>

#include "app/app.h"

#define BOOKMARKS_PATH APP_DIR "/bookmarks.txt"

static struct bookmark g_marks[BOOKMARKS_MAX];
static int g_count, g_loaded;

static void load(void)
{
	if (g_loaded)
		return;
	g_loaded = 1;
	FILE *f = fopen(BOOKMARKS_PATH, "r");
	if (!f)
		return;
	char line[1024];
	while (g_count < BOOKMARKS_MAX && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *url = strchr(line, '\t');
		if (!url)
			continue;
		*url++ = '\0';
		char *title = strchr(url, '\t');
		if (title)
			*title++ = '\0';
		struct bookmark *b = &g_marks[g_count++];
		snprintf(b->timestamp, sizeof b->timestamp, "%s", line);
		snprintf(b->url, sizeof b->url, "%s", url);
		snprintf(b->title, sizeof b->title, "%s", title ? title : "");
	}
	fclose(f);
}

static void save(void)
{
	FILE *f = fopen(BOOKMARKS_PATH, "w");
	if (!f) {
		WL_LOG_W("APP", "can't write " BOOKMARKS_PATH);
		return;
	}
	for (int i = 0; i < g_count; i++)
		fprintf(f, "%s\t%s\t%s\n", g_marks[i].timestamp, g_marks[i].url, g_marks[i].title);
	fclose(f);
}

int bookmarks_count(void)
{
	load();
	return g_count;
}

const struct bookmark *bookmarks_get(int i)
{
	load();
	return i >= 0 && i < g_count ? &g_marks[i] : NULL;
}

int bookmarks_find(const char *url, const char *timestamp)
{
	load();
	for (int i = 0; i < g_count; i++)
		if (strcmp(g_marks[i].url, url) == 0 && strncmp(g_marks[i].timestamp, timestamp, 8) == 0)
			return i;
	return -1;
}

/* Adds it (newest first), or removes it if it's already there. Returns 1 if now bookmarked. */
int bookmarks_toggle(const char *url, const char *timestamp, const char *title)
{
	int i = bookmarks_find(url, timestamp);
	if (i >= 0) {
		memmove(&g_marks[i], &g_marks[i + 1], (size_t)(g_count - i - 1) * sizeof g_marks[0]);
		g_count--;
		save();
		return 0;
	}
	if (g_count == BOOKMARKS_MAX)
		g_count--;
	memmove(&g_marks[1], &g_marks[0], (size_t)g_count * sizeof g_marks[0]);
	struct bookmark *b = &g_marks[0];
	snprintf(b->timestamp, sizeof b->timestamp, "%s", timestamp);
	snprintf(b->url, sizeof b->url, "%s", url);
	/* Tabs and newlines would break the file format. */
	snprintf(b->title, sizeof b->title, "%s", title && *title ? title : url);
	for (char *c = b->title; *c; c++)
		if (*c == '\t' || *c == '\n' || *c == '\r')
			*c = ' ';
	g_count++;
	save();
	return 1;
}
