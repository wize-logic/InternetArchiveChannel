/* Media player: one item at a time, streamed over HTTP, decoded by FFmpeg on its own thread.
 * Audio goes through the WiiLibs PCM output (48 kHz stereo); video frames (Phase 5) are handed
 * to the UI thread for GX upload. All functions are called from the UI thread. */
#ifndef IAC_PLAYER_H
#define IAC_PLAYER_H

#include <stdint.h>

enum player_state { PL_IDLE = 0, PL_LOADING, PL_PLAYING, PL_PAUSED, PL_ENDED, PL_ERROR };

int player_init(void);
void player_shutdown(void);

/* Starts playing `url` (stopping whatever played). fast_video: skip the H.264 loop filter. */
int player_open(const char *url, int want_video, int fast_video);
void player_stop(void);
void player_pause(int paused);
/* Seeks to an absolute position in seconds. */
void player_seek(double seconds);

enum player_state player_get_state(void);
double player_position(void); /* seconds */
double player_duration(void); /* seconds, 0 if unknown */
/* 0..1 of the read-ahead buffer target, for a "buffering" indicator. */
float player_buffer_level(void);
const char *player_error(void);

/* Video (Phase 5): the newest decoded frame due for display, or NULL. The planes stay valid
 * until the next call. */
struct player_frame {
	uint8_t *planes[3];
	int linesize[3];
	int width, height;
	uint32_t serial; /* changes with every new frame */
};
const struct player_frame *player_video_frame(void);

/* Video frames shown, passed over because they were late, and dropped from a full queue. */
void player_video_stats(uint32_t *shown, uint32_t *skipped, uint32_t *dropped);

#endif
