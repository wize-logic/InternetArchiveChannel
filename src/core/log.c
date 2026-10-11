#include "core/log.h"

#include <stdarg.h>
#include <stdio.h>

#include "core/platform.h"

#ifdef HW_RVL
#include <ogc/system.h>
#endif

static FILE *g_log_file;
static iac_mutex g_log_lock;
static int g_log_ready;
static void (*g_hook)(const char *line);

void log_set_hook(void (*fn)(const char *line))
{
	g_hook = fn;
}

void log_init(const char *path)
{
	if (!g_log_ready) {
		plat_mutex_init(&g_log_lock);
		g_log_ready = 1;
	}
	if (path)
		g_log_file = fopen(path, "w");
}

void log_printf(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);

	if (g_log_ready)
		plat_mutex_lock(&g_log_lock);
	fputs(line, stdout);
	fflush(stdout);
#ifdef HW_RVL
	/* Shows up in Dolphin's log (OSREPORT) and on a USB Gecko-style UART. */
	SYS_Report("%s", line);
#endif
	if (g_log_file) {
		fputs(line, g_log_file);
		fflush(g_log_file);
	}
	if (g_log_ready)
		plat_mutex_unlock(&g_log_lock);
	if (g_hook)
		g_hook(line);
}

void log_close(void)
{
	if (g_log_file) {
		fclose(g_log_file);
		g_log_file = NULL;
	}
}
