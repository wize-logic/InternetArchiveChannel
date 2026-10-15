/* A compact MP4/MOV sample index for files too big for FFmpeg's own.
 *
 * FFmpeg's mov demuxer keeps a 24-byte index entry for every sample, which for a 3.5 h recording
 * (~1M samples) is ~60 MB: more than the Wii has. This reader walks the `moov` once and keeps only
 * the raw sample tables (sizes, chunk offsets, run-length timing), ~8 bytes a sample. It then hands
 * out samples one at a time, in file order, for the player to feed straight to the decoders.
 *
 * Supports H.264 (avc1/avc3) video and AAC audio in plain (non-fragmented) files. Libav-free, so
 * the host tests can run it on a file. */
#ifndef IAC_MP4IDX_H
#define IAC_MP4IDX_H

#include <stddef.h>
#include <stdint.h>

/* Byte source with the semantics of media_stream: read returns bytes read, 0 at the end, <0 on error. */
struct mp4_io {
	void *ctx;
	int (*read)(void *ctx, uint8_t *buf, size_t n);
	int (*seek)(void *ctx, int64_t pos);
};

enum mp4_kind { MP4_VIDEO = 0, MP4_AUDIO = 1, MP4_KINDS };
enum mp4_codec { MP4_CODEC_NONE = 0, MP4_CODEC_H264, MP4_CODEC_AAC };

struct mp4_track_info {
	int present;
	enum mp4_codec codec;
	uint32_t timescale;
	int width, height;         /* video */
	int channels, sample_rate; /* audio, as the sample entry states them */
	const uint8_t *extradata;  /* avcC record / AudioSpecificConfig, owned by the index */
	int extradata_size;
	uint32_t sample_count;
};

struct mp4_sample {
	int kind;      /* enum mp4_kind */
	int64_t pos;   /* byte offset in the file */
	uint32_t size;
	int64_t dts;   /* track timescale units */
	int64_t pts;   /* dts plus the composition offset, less the edit-list start */
	int key;
};

typedef struct mp4idx mp4idx;

/* Looks for the moov box among the top-level boxes. 0 and its offset/size when found; -1 if this
 * doesn't look like an MP4 or has no moov. (Seeks around the file but reads only box headers.) */
int mp4_find_moov(const struct mp4_io *io, int64_t file_size, int64_t *off, int64_t *size);

/* Parses the moov at `off`. NULL with a message in err when it can't be indexed. */
mp4idx *mp4idx_open(const struct mp4_io *io, int64_t moov_off, int64_t moov_size, char *err, size_t errcap);
void mp4idx_close(mp4idx *x);

const struct mp4_track_info *mp4idx_track(const mp4idx *x, enum mp4_kind kind);
double mp4idx_duration(const mp4idx *x); /* seconds, 0 if unknown */
size_t mp4idx_bytes(const mp4idx *x);    /* memory held by the index */

/* Tracks that next() produces. All present tracks are on by default. */
void mp4idx_enable(mp4idx *x, enum mp4_kind kind, int on);
/* The next sample in file order among the enabled tracks: 1, or 0 at the end. */
int mp4idx_next(mp4idx *x, struct mp4_sample *out);
/* Repositions at `seconds`: video at the keyframe at or before it, audio at the sample at it. */
void mp4idx_seek(mp4idx *x, double seconds);

#endif
