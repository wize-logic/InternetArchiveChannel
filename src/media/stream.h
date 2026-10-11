/* A remote file read as a series of Range requests over the keep-alive HTTP client.
 *
 * The first request follows archive.org's redirect to a storage node; later chunks go straight
 * there. A few chunks are kept in flight ahead of the read position, which also spreads the
 * transfer over parallel connections (2x the throughput of one on the Wii).
 * stream_read blocks the calling thread (a decoder thread, never the UI). */
#ifndef IAC_STREAM_H
#define IAC_STREAM_H

#include <stddef.h>
#include <stdint.h>

typedef struct media_stream media_stream;

/* chunk_size: bytes per Range request; ahead: chunks prefetched past the current one. */
media_stream *stream_open(const char *url, size_t chunk_size, int ahead);
void stream_close(media_stream *s);

/* Returns bytes read (>0), 0 at end of file, <0 on error (or when interrupted). */
int stream_read(media_stream *s, uint8_t *buf, size_t n);
/* 0 on success. Positions past the end are clamped. */
int stream_seek(media_stream *s, int64_t pos);
int64_t stream_tell(const media_stream *s);
/* Total size, or -1 until the first chunk arrives (stream_read waits for it). */
int64_t stream_size(const media_stream *s);
/* Makes a blocked stream_read return -1 (from another thread), until stream_resume. */
void stream_interrupt(media_stream *s);
void stream_resume(media_stream *s);
/* Bytes buffered contiguously from the read position (for "buffering" indicators). */
int64_t stream_buffered(const media_stream *s);
const char *stream_error(const media_stream *s);

#endif
