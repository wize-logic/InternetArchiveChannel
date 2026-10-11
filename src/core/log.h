#ifndef IAC_LOG_H
#define IAC_LOG_H

/* Opens an optional log file (may be NULL or unwritable; stdout logging still works). */
void log_init(const char *path);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_close(void);
/* Also hands each line (trailing newline included) to `fn`, e.g. the app's SD card log. */
void log_set_hook(void (*fn)(const char *line));

#endif
