/* On-screen keyboard: a modal editing a UTF-8 buffer. Pointer or D-pad; a USB keyboard types
 * straight in. PLUS or "Done" confirms, B cancels. */
#ifndef IAC_OSK_H
#define IAC_OSK_H

#include <stddef.h>

enum { OSK_EDITING = 0, OSK_DONE, OSK_CANCELLED };

/* Starts editing a copy of `initial`. */
void osk_open(const char *title, const char *initial, size_t max_len);
int osk_is_open(void);
/* Draws and handles the keyboard; call every frame while open, after the screen's own UI. */
int osk_update(void);
/* The edited text (valid after OSK_DONE until the next osk_open). */
const char *osk_text(void);

#endif
