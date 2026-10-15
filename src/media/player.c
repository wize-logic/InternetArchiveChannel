#include "media/player.h"

#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

#include "core/platform.h"
#include "media/mp4idx.h"
#include "media/pcm_ring.h"
#include "media/stream.h"
#include "wl/base/base.h"
#include "wl/platform/platform.h"

#define OUT_RATE 48000
#define RING_FRAMES 32768 /* ~0.7 s of PCM */
#define VQUEUE 4      /* decoded frames queued ahead of the clock */
#define VQUEUE_MAX 24 /* hard limit, reached only when the read-ahead jams (see the decode loop) */
/* Compressed packets read ahead of their decoder, per stream and in all. Files interleave the
 * streams loosely (Ogg video runs up to ~1 s ahead of its audio, or audio ~0.4 s ahead of its
 * video), so each stream is fed from its own queue rather than in file order. */
#define PKTQ_SIZE 1024
#define PKTQ_BYTES (4u << 20)
#define AUDIO_CHUNK (256 * 1024)
#define VIDEO_CHUNK (512 * 1024)
/* FFmpeg's index costs ~24 bytes a sample and more while it grows. A 2 h file (~820k samples) opens
 * on the Wii; a 3.5 h one (~976k) leaves no heap, so refuse above this. */
#define MAX_INDEX_ENTRIES 900000
/* An MP4 whose moov is bigger than this (~450k samples: a 1.5 h+ recording) is indexed by media/mp4idx.c
 * instead of FFmpeg, at a fifth of the memory. */
#define MOOV_BIG_BYTES (6u << 20)
#define MP4_MAX_SAMPLE (8u << 20) /* sanity bound on one sample, bytes */
/* Video decoding for this long with no audio output at all: stop slaving the clock to the audio. */
#define AUDIO_STALL_US 8000000u

/* malloc's bytes in use. Once malloc spills from MEM1 into MEM2, newlib counts the address hole
 * between them (0x81800000..0x90000000) as in use too; take it back out. */
static int heap_used_kb(void)
{
	size_t used = mallinfo().uordblks, hole = 0x90000000u - 0x81800000u;
	if (used >= hole)
		used -= hole;
	return (int)(used / 1024);
}

struct vslot {
	AVFrame *frame;
	double pts;
};

static struct {
	/* shared with the audio callback */
	struct pcm_ring ring;
	volatile int out_paused;   /* callback plays silence and leaves the ring alone */
	volatile uint32_t fill_ack; /* bumped by every callback */
	volatile uint32_t played;  /* frames played since base_pts */

	/* decode thread */
	iac_thread thread;
	int thread_running;
	volatile int stop;
	volatile int seek_req;
	volatile double seek_to;
	media_stream *stream;
	char url[1024];
	int want_video, fast_video;

	/* state for the UI */
	volatile int state;
	volatile int paused;
	volatile double base_pts;
	volatile double duration;
	volatile int has_audio;
	char error[128];
	char status[64];

	/* video frames: decode thread -> UI thread */
	iac_mutex vlock;
	struct vslot vq[VQUEUE_MAX];
	int vcount;
	AVFrame *shown;
	struct player_frame shown_info;
	uint64_t video_epoch_us; /* wall clock origin when there is no audio */
	volatile int behind;     /* UI saw late frames: decoder skips non-reference frames */
	uint32_t shown_count, skipped, dropped; /* frames displayed, passed over as late, dropped queued */
	int oom_logged;
} g;

/* ---- audio output callback (WiiLibs audio thread; must not block) ------------------------ */

static void fill(void *user, int16_t *stereo, uint32_t frames)
{
	(void)user;
	g.fill_ack++;
	uint32_t got = 0;
	if (!g.out_paused && !g.paused)
		got = pcm_ring_read(&g.ring, stereo, frames);
	if (got < frames)
		memset(stereo + got * 2, 0, (frames - got) * 4);
	__atomic_add_fetch(&g.played, got, __ATOMIC_RELAXED);
}

/* Decode thread: stops the callback reading, then empties the ring. */
static void flush_output(void)
{
	g.out_paused = 1;
	uint32_t ack = g.fill_ack;
	for (int i = 0; i < 100 && g.fill_ack - ack < 2; i++)
		usleep(2000);
	pcm_ring_clear(&g.ring);
	g.played = 0;
	g.out_paused = 0;
}

/* ---- AVIO over media_stream ------------------------------------------------------------- */

static int io_read(void *opaque, uint8_t *buf, int size)
{
	int n = stream_read(opaque, buf, (size_t)size);
	if (n == 0)
		return AVERROR_EOF;
	return n < 0 ? AVERROR(EIO) : n;
}

static int64_t io_seek(void *opaque, int64_t off, int whence)
{
	media_stream *s = opaque;
	whence &= ~AVSEEK_FORCE;
	if (whence == AVSEEK_SIZE) {
		if (stream_size(s) < 0) {
			uint8_t probe;
			int64_t keep = stream_tell(s);
			if (stream_read(s, &probe, 1) >= 0)
				stream_seek(s, keep);
		}
		return stream_size(s);
	}
	int64_t base = whence == SEEK_CUR ? stream_tell(s) : whence == SEEK_END ? stream_size(s) : 0;
	if (whence == SEEK_END && base < 0)
		return -1;
	stream_seek(s, base + off);
	return stream_tell(s);
}

/* ---- video queue ------------------------------------------------------------------------ */

static double clock_now(void)
{
	if (g.has_audio)
		return g.base_pts + (double)g.played / OUT_RATE;
	if (!g.video_epoch_us || g.paused)
		return g.base_pts;
	return g.base_pts + (double)(wl_platform_time_us() - g.video_epoch_us) / 1e6;
}

static void vqueue_clear(void)
{
	plat_mutex_lock(&g.vlock);
	for (int i = 0; i < g.vcount; i++)
		av_frame_free(&g.vq[i].frame);
	g.vcount = 0;
	plat_mutex_unlock(&g.vlock);
}

/* Queues a decoded frame without waiting: the decode loop only decodes while there is room. At
 * VQUEUE_MAX (a jammed read-ahead) the oldest frame is dropped to make room. */
static void vqueue_push(AVFrame *f, double pts)
{
	plat_mutex_lock(&g.vlock);
	if (g.vcount >= VQUEUE_MAX) {
		av_frame_free(&g.vq[0].frame);
		memmove(&g.vq[0], &g.vq[1], sizeof g.vq[0] * (size_t)(g.vcount - 1));
		g.vcount--;
		g.dropped++;
	}
	AVFrame *copy = av_frame_clone(f);
	if (copy) {
		g.vq[g.vcount].frame = copy;
		g.vq[g.vcount].pts = pts;
		g.vcount++;
	} else {
		/* Out of memory: skip the frame rather than queue a hole. */
		if (!g.oom_logged++)
			WL_LOG_W("PLAYER", "out of memory queueing a video frame (heap %d KB)", heap_used_kb());
		g.dropped++;
	}
	plat_mutex_unlock(&g.vlock);
}

/* ---- packet queues (decode thread only) -------------------------------------------------- */

struct pktq {
	AVPacket *p[PKTQ_SIZE];
	int head, count;
	size_t bytes;
};

static struct pktq g_aq, g_vpq; /* audio and video packets read ahead */

/* Takes over pkt's data; 0 on success. */
static int pktq_put(struct pktq *q, AVPacket *pkt)
{
	AVPacket *c = q->count < PKTQ_SIZE ? av_packet_alloc() : NULL;
	if (!c)
		return -1;
	av_packet_move_ref(c, pkt);
	q->p[(q->head + q->count) % PKTQ_SIZE] = c;
	q->count++;
	q->bytes += (size_t)c->size;
	return 0;
}

static AVPacket *pktq_peek(struct pktq *q)
{
	return q->count ? q->p[q->head] : NULL;
}

static void pktq_pop(struct pktq *q)
{
	q->bytes -= (size_t)q->p[q->head]->size;
	av_packet_free(&q->p[q->head]);
	q->head = (q->head + 1) % PKTQ_SIZE;
	q->count--;
}

static void pktq_clear(struct pktq *q)
{
	while (q->count)
		pktq_pop(q);
}

/* The read-ahead holds all it may. */
static int pktq_full(void)
{
	return g_aq.bytes + g_vpq.bytes >= PKTQ_BYTES || g_aq.count == PKTQ_SIZE || g_vpq.count == PKTQ_SIZE;
}

const struct player_frame *player_video_frame(void)
{
	if (!g.want_video || (g.state != PL_PLAYING && g.state != PL_PAUSED))
		return g.shown ? &g.shown_info : NULL;
	double now = clock_now();
	AVFrame *pick = NULL;
	plat_mutex_lock(&g.vlock);
	/* Take every frame that is due; show the newest of them. */
	while (g.vcount > 0 && g.vq[0].pts <= now + 0.010) {
		if (pick) {
			av_frame_free(&pick);
			g.skipped++;
		}
		pick = g.vq[0].frame;
		g.behind = now - g.vq[0].pts > 0.1;
		memmove(&g.vq[0], &g.vq[1], sizeof g.vq[0] * (size_t)(g.vcount - 1));
		g.vcount--;
	}
	plat_mutex_unlock(&g.vlock);
	if (pick) {
		g.shown_count++;
		if (g.shown)
			av_frame_free(&g.shown);
		g.shown = pick;
		for (int i = 0; i < 3; i++) {
			g.shown_info.planes[i] = pick->data[i];
			g.shown_info.linesize[i] = pick->linesize[i];
		}
		g.shown_info.width = pick->width;
		g.shown_info.height = pick->height;
		g.shown_info.serial++;
	}
	return g.shown ? &g.shown_info : NULL;
}

/* ---- decode thread ---------------------------------------------------------------------- */

static void set_error(const char *fmt, const char *detail)
{
	snprintf(g.error, sizeof g.error, fmt, detail);
	WL_LOG_W("PLAYER", "%s", g.error);
	g.state = PL_ERROR;
}

static AVCodecContext *open_codec_par(const AVCodecParameters *par, int fast)
{
	const AVCodec *codec = avcodec_find_decoder(par->codec_id);
	if (!codec)
		return NULL;
	AVCodecContext *c = avcodec_alloc_context3(codec);
	if (!c)
		return NULL;
	avcodec_parameters_to_context(c, par);
	c->thread_count = 1;
	if (fast) {
		c->skip_loop_filter = AVDISCARD_ALL;
		c->flags2 |= AV_CODEC_FLAG2_FAST;
	}
	if (avcodec_open2(c, codec, NULL) < 0)
		avcodec_free_context(&c);
	return c;
}

static AVCodecContext *open_codec(AVStream *st, int fast)
{
	return open_codec_par(st->codecpar, fast);
}

/* A decoder for a track the compact MP4 index found: its avcC / AudioSpecificConfig is all the
 * decoder needs. */
static AVCodecContext *open_mp4_codec(const struct mp4_track_info *t, int fast)
{
	AVCodecParameters *par = avcodec_parameters_alloc();
	if (!par)
		return NULL;
	int audio = t->codec == MP4_CODEC_AAC;
	par->codec_type = audio ? AVMEDIA_TYPE_AUDIO : AVMEDIA_TYPE_VIDEO;
	par->codec_id = audio ? AV_CODEC_ID_AAC : AV_CODEC_ID_H264;
	par->extradata = av_mallocz((size_t)t->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
	if (!par->extradata) {
		avcodec_parameters_free(&par);
		return NULL;
	}
	memcpy(par->extradata, t->extradata, (size_t)t->extradata_size);
	par->extradata_size = t->extradata_size;
	if (audio) {
		par->sample_rate = t->sample_rate;
		av_channel_layout_default(&par->ch_layout, t->channels > 0 ? t->channels : 2);
	} else {
		par->width = t->width;
		par->height = t->height;
	}
	AVCodecContext *c = open_codec_par(par, fast);
	avcodec_parameters_free(&par);
	return c;
}

/* ---- compact MP4 index ------------------------------------------------------------------ */

static int mio_read(void *ctx, uint8_t *buf, size_t n)
{
	return stream_read(ctx, buf, n);
}

static int mio_seek(void *ctx, int64_t pos)
{
	return stream_seek(ctx, pos);
}

/* Indexes the file with mp4idx when it is an MP4 whose moov is big (or, with force, any MP4).
 * NULL if it isn't one, leaving the stream at the start; *fatal is set, with the error already
 * reported, when it should have worked but didn't. */
static mp4idx *open_mp4(media_stream *s, int force, int *fatal)
{
	uint8_t probe;
	if (stream_read(s, &probe, 1) < 0) {
		*fatal = !g.stop && !g.seek_req;
		if (*fatal)
			set_error("Couldn't open the file%s.", stream_error(s)[0] ? " (network)" : "");
		return NULL;
	}
	struct mp4_io io = {s, mio_read, mio_seek};
	int64_t off, len;
	if (mp4_find_moov(&io, stream_size(s), &off, &len) != 0 || (!force && len < MOOV_BIG_BYTES)) {
		stream_seek(s, 0);
		return NULL;
	}
	WL_LOG_I("PLAYER", "big moov (%lld KB), indexing it compactly, heap %d KB", (long long)(len / 1024),
		heap_used_kb());
	snprintf(g.status, sizeof g.status, "Reading the video index (%lld MB)...", (long long)((len + (1 << 20) - 1) >> 20));
	char err[128];
	mp4idx *x = mp4idx_open(&io, off, len, err, sizeof err);
	if (!x) {
		WL_LOG_W("PLAYER", "index failed: %s (stream: %s)", err, stream_error(s));
		set_error("%s", err[0] ? err : "Couldn't read the video index.");
		*fatal = 1;
		return NULL;
	}
	g.status[0] = '\0';
	WL_LOG_I("PLAYER", "indexed: %.0f s, %zu KB, heap %d KB", mp4idx_duration(x), mp4idx_bytes(x) / 1024,
		heap_used_kb());
	return x;
}

/* The next packet from the compact index: reads the sample's bytes from the stream. */
static int mp4_read_packet(mp4idx *x, media_stream *s, AVPacket *pkt)
{
	struct mp4_sample m;
	if (!mp4idx_next(x, &m))
		return AVERROR_EOF;
	if (m.size > MP4_MAX_SAMPLE || av_new_packet(pkt, (int)m.size) < 0)
		return AVERROR(ENOMEM);
	if (stream_seek(s, m.pos) != 0) {
		av_packet_unref(pkt);
		return AVERROR(EIO);
	}
	for (uint32_t got = 0; got < m.size;) {
		int n = stream_read(s, pkt->data + got, m.size - got);
		if (n <= 0) {
			av_packet_unref(pkt);
			return n == 0 ? AVERROR_EOF : AVERROR(EIO);
		}
		got += (uint32_t)n;
	}
	pkt->stream_index = m.kind == MP4_AUDIO ? 0 : 1;
	pkt->pts = m.pts;
	pkt->dts = m.dts;
	if (m.key)
		pkt->flags |= AV_PKT_FLAG_KEY;
	return 0;
}

/* Reads the next packet into its stream's read-ahead queue (or drops it, for a stream not being
 * played). <0 at the end of the file, on an error, or when a stop or seek interrupted the read. */
static int read_packet(mp4idx *midx, AVFormatContext *fmt, AVPacket *pkt, int aidx, int vidx, int audio, int video)
{
	int rc = midx ? mp4_read_packet(midx, g.stream, pkt) : av_read_frame(fmt, pkt);
	if (rc < 0)
		return rc;
	if (pkt->stream_index == aidx && audio)
		pktq_put(&g_aq, pkt);
	else if (pkt->stream_index == vidx && video)
		pktq_put(&g_vpq, pkt);
	av_packet_unref(pkt);
	return 0;
}

/* FFmpeg's demuxer over the stream. */
static int open_fmt(AVFormatContext **fmt, AVIOContext **io)
{
	uint8_t *iobuf = av_malloc(32768);
	*io = g.stream && iobuf ? avio_alloc_context(iobuf, 32768, 0, g.stream, io_read, NULL, io_seek) : NULL;
	*fmt = avformat_alloc_context();
	if (!*io || !*fmt) {
		if (!*io)
			av_free(iobuf);
		avformat_free_context(*fmt);
		*fmt = NULL;
		return -1;
	}
	(*fmt)->pb = *io;
	return 0;
}

static void close_fmt(AVFormatContext **fmt, AVIOContext **io)
{
	if (*fmt)
		avformat_close_input(fmt);
	if (*io) {
		av_freep(&(*io)->buffer);
		avio_context_free(io);
	}
}

static void *decode_thread(void *arg)
{
	(void)arg;
	AVFormatContext *fmt = NULL;
	AVIOContext *io = NULL;
	mp4idx *midx = NULL;
	AVCodecContext *adec = NULL, *vdec = NULL;
	SwrContext *swr = NULL;
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	int16_t *pcm = NULL;
	int pcm_cap = 0, aidx = -1, vidx = -1;
	AVRational vtb = {1, 1000};
	double duration = 0;
	int fatal = 0;

	g.stream = stream_open(g.url, g.want_video ? VIDEO_CHUNK : AUDIO_CHUNK, g.want_video ? 3 : 2);
	if (!pkt || !frame || !g.stream) {
		set_error("%s", "Out of memory.");
		goto done;
	}
	midx = open_mp4(g.stream, 0, &fatal);
	if (fatal)
		goto done;
	if (!midx) {
		if (open_fmt(&fmt, &io) < 0) {
			set_error("%s", "Out of memory.");
			goto done;
		}
		if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0 || avformat_find_stream_info(fmt, NULL) < 0) {
			if (stream_error(g.stream)[0])
				WL_LOG_W("PLAYER", "stream: %s", stream_error(g.stream));
			WL_LOG_W("PLAYER", "open failed, heap %d KB", heap_used_kb());
			set_error("Couldn't open the file%s.", stream_error(g.stream)[0] ? " (network)" : "");
			goto done;
		}
		long index_entries = 0;
		for (unsigned i = 0; i < fmt->nb_streams; i++)
			index_entries += avformat_index_get_entries_count(fmt->streams[i]);
		WL_LOG_I("PLAYER", "opened: %ld index entries, heap %d KB", index_entries, heap_used_kb());
		if (index_entries > MAX_INDEX_ENTRIES) {
			/* More than FFmpeg's index fits: start over with the compact one, if this is an MP4. */
			close_fmt(&fmt, &io);
			midx = open_mp4(g.stream, 1, &fatal);
			if (!midx) {
				if (!fatal)
					set_error("%s", "Too long for the Wii's memory.");
				goto done;
			}
		}
	}
	if (midx) {
		const struct mp4_track_info *vi = g.want_video ? mp4idx_track(midx, MP4_VIDEO) : NULL;
		const struct mp4_track_info *ai = mp4idx_track(midx, MP4_AUDIO);
		mp4idx_enable(midx, MP4_VIDEO, vi != NULL);
		mp4idx_enable(midx, MP4_AUDIO, ai != NULL);
		if (ai) {
			aidx = 0;
			adec = open_mp4_codec(ai, 0);
		}
		if (vi) {
			vidx = 1;
			vtb = (AVRational){1, (int)vi->timescale};
			vdec = open_mp4_codec(vi, g.fast_video);
		}
		duration = mp4idx_duration(midx);
	} else {
		aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
		if (g.want_video)
			vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
		if (aidx >= 0)
			adec = open_codec(fmt->streams[aidx], 0);
		if (vidx >= 0) {
			vdec = open_codec(fmt->streams[vidx], g.fast_video);
			vtb = fmt->streams[vidx]->time_base;
		}
		duration = fmt->duration > 0 ? (double)fmt->duration / AV_TIME_BASE : 0;
	}
	if (!adec && !vdec) {
		set_error("%s", "No playable audio or video in this file.");
		goto done;
	}
	g.has_audio = adec != NULL;
	g.duration = duration;
	if (adec) {
		AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
		if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, OUT_RATE, &adec->ch_layout, adec->sample_fmt,
			    adec->sample_rate, 0, NULL) < 0 ||
			swr_init(swr) < 0) {
			set_error("%s", "Audio format not supported.");
			goto done;
		}
	}
	WL_LOG_I("PLAYER", "%s%s%s, %.0f s", adec ? adec->codec->name : "", adec && vdec ? " + " : "",
		vdec ? vdec->codec->name : "", g.duration);
	flush_output();
	g.base_pts = 0;
	g.video_epoch_us = 0;
	g.state = g.paused ? PL_PAUSED : PL_PLAYING;

	int read_eof = 0;
	int jammed = 0;              /* reading is held up by the read-ahead limit while the audio is dry */
	int pcm_len = 0, pcm_done = 0; /* converted audio frames, and how many are in the ring */
	int audio_out = 0;           /* audio frames pushed to the ring since the start or last seek */
	uint64_t video_since_us = 0; /* when video began decoding with no audio output yet */
	while (!g.stop) {
		if (g.seek_req) {
			double to = g.seek_to;
			g.seek_req = 0;
			/* player_seek interrupted any blocked read; reading resumes from here. */
			stream_resume(g.stream);
			int seeked = 1;
			if (midx) {
				mp4idx_seek(midx, to);
			} else {
				io->eof_reached = 0;
				io->error = 0;
				seeked = av_seek_frame(fmt, -1, (int64_t)(to * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) >= 0;
			}
			if (seeked) {
				if (adec)
					avcodec_flush_buffers(adec);
				if (vdec)
					avcodec_flush_buffers(vdec);
				pktq_clear(&g_aq);
				pktq_clear(&g_vpq);
				pcm_len = pcm_done = 0;
				vqueue_clear();
				flush_output();
				g.base_pts = to;
				g.video_epoch_us = 0;
				g.has_audio = adec != NULL;
				audio_out = 0;
				video_since_us = 0;
				read_eof = jammed = 0;
				if (g.state == PL_ENDED)
					g.state = g.paused ? PL_PAUSED : PL_PLAYING;
			}
		}
		if (g.paused && g.state == PL_PLAYING)
			g.state = PL_PAUSED;
		else if (!g.paused && g.state == PL_PAUSED) {
			g.state = PL_PLAYING;
			if (!g.has_audio && g.video_epoch_us) /* resume the wall clock where it stopped */
				g.video_epoch_us = wl_platform_time_us();
		}

		/* Nothing here waits: each pass fills the audio ring as far as it can, takes one step of
		 * video decoding while the frame queue has room, and reads a packet for video if it has
		 * run out. Audio goes first and all the way: it costs little, and a dry ring stops the
		 * clock. (One audio frame per video step, 21 ms of sound per 33 ms frame, starved it
		 * whenever the video queue had room.) */
		int progress = 0;
		while (adec && g.has_audio && !g.stop && !g.seek_req) {
			if (pcm_done < pcm_len) {
				uint32_t n = pcm_ring_write(&g.ring, pcm + pcm_done * 2, (uint32_t)(pcm_len - pcm_done));
				pcm_done += (int)n;
				progress |= n > 0;
				if (pcm_done < pcm_len)
					break; /* the ring is full */
			}
			AVPacket *ap;
			if (avcodec_receive_frame(adec, frame) == 0) {
				int out_max = swr_get_out_samples(swr, frame->nb_samples);
				if (out_max > pcm_cap) {
					free(pcm);
					pcm = malloc((size_t)out_max * 4);
					pcm_cap = pcm ? out_max : 0;
				}
				if (pcm) {
					int out = swr_convert(swr, (uint8_t **)&pcm, out_max, (const uint8_t **)frame->extended_data,
						frame->nb_samples);
					pcm_len = out > 0 ? out : 0;
					pcm_done = 0;
					audio_out += out > 0;
				} else {
					WL_LOG_W("PLAYER", "out of memory for audio (heap %d KB)", heap_used_kb());
				}
				progress = 1;
			} else if ((ap = pktq_peek(&g_aq))) {
				if (avcodec_send_packet(adec, ap) == AVERROR(EAGAIN))
					break; /* can't happen after receive came back empty; don't spin on it */
				pktq_pop(&g_aq);
				progress = 1;
			} else if (!read_eof && !pktq_full()) {
				if (read_packet(midx, fmt, pkt, aidx, vidx, 1, vdec != NULL) < 0) {
					if (g.stop || g.seek_req)
						break;
					read_eof = 1;
				}
				progress = 1;
			} else {
				break;
			}
		}
		/* A jam (see below) lets the queue run to VQUEUE_MAX, then drop its oldest frames. */
		if (vdec && (g.vcount < VQUEUE || jammed)) {
			AVPacket *vp;
			if (avcodec_receive_frame(vdec, frame) == 0) {
				int64_t p = frame->best_effort_timestamp;
				double pts = p == AV_NOPTS_VALUE ? g.base_pts : (double)p * av_q2d(vtb);
				if (!g.has_audio && !g.video_epoch_us) {
					g.base_pts = pts;
					g.video_epoch_us = wl_platform_time_us();
				}
				vqueue_push(frame, pts);
				if (g.has_audio && !audio_out && !video_since_us)
					video_since_us = wl_platform_time_us();
				progress = 1;
			} else if ((vp = pktq_peek(&g_vpq))) {
				vdec->skip_frame = g.behind ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
				if (avcodec_send_packet(vdec, vp) != AVERROR(EAGAIN))
					pktq_pop(&g_vpq);
				progress = 1;
			}
		}
		/* The clock follows the audio, so audio that never arrives would freeze the picture at
		 * 0:00. Fall back to the wall clock (the no-audio path). */
		if (g.has_audio && !audio_out && video_since_us && wl_platform_time_us() - video_since_us > AUDIO_STALL_US) {
			WL_LOG_W("PLAYER", "no audio after %u s of video, playing on the video clock (heap %d KB)",
				AUDIO_STALL_US / 1000000u, heap_used_kb());
			g.video_epoch_us = 0;
			g.has_audio = 0;
			pktq_clear(&g_aq);
			pcm_len = pcm_done = 0;
		}

		int need_audio = adec && g.has_audio && pcm_done == pcm_len && !g_aq.count;
		int need_video = vdec && g_vpq.count < 2;
		int full = pktq_full();
		/* Audio that sits further into the file than the read-ahead holds: with the ring dry the
		 * clock stops, so decode video past VQUEUE (dropping frames) to make room to read on. */
		jammed = full && need_audio && pcm_ring_count(&g.ring) == 0;
		if (!read_eof && (need_audio || need_video) && !full) {
			if (read_packet(midx, fmt, pkt, aidx, vidx, adec && g.has_audio, vdec != NULL) < 0) {
				if (g.stop || g.seek_req)
					continue;
				read_eof = 1;
			}
			progress = 1;
		}
		if (read_eof && !g_aq.count && !g_vpq.count && pcm_done == pcm_len && pcm_ring_count(&g.ring) == 0 &&
			g.vcount == 0 && g.state != PL_ENDED)
			g.state = PL_ENDED; /* the output has drained */
		if (!progress)
			usleep(read_eof ? 20000 : 4000);
	}

done:
	pktq_clear(&g_aq);
	pktq_clear(&g_vpq);
	free(pcm);
	swr_free(&swr);
	avcodec_free_context(&adec);
	avcodec_free_context(&vdec);
	close_fmt(&fmt, &io);
	mp4idx_close(midx);
	av_frame_free(&frame);
	av_packet_free(&pkt);
	stream_close(g.stream);
	g.stream = NULL;
	vqueue_clear();
	return NULL;
}

/* ---- control (UI thread) ---------------------------------------------------------------- */

int player_init(void)
{
	av_log_set_level(AV_LOG_ERROR);
	g.ring.capacity = RING_FRAMES;
	g.ring.buf = memalign(32, RING_FRAMES * 4);
	if (!g.ring.buf)
		return -1;
	plat_mutex_init(&g.vlock);
	if (wl_platform_audio_start(OUT_RATE, 256, fill, NULL) != WL_OK) {
		WL_LOG_W("PLAYER", "audio output failed to start");
		return -1;
	}
	return 0;
}

void player_stop(void)
{
	if (g.thread_running) {
		g.stop = 1;
		if (g.stream)
			stream_interrupt(g.stream);
		plat_thread_join(g.thread);
		g.thread_running = 0;
	}
	g.out_paused = 1;
	pcm_ring_clear(&g.ring);
	g.out_paused = 0;
	if (g.shown)
		av_frame_free(&g.shown);
	g.state = PL_IDLE;
	g.stop = 0;
	g.seek_req = 0;
	g.paused = 0;
	g.played = 0;
	g.duration = 0;
}

void player_shutdown(void)
{
	player_stop();
	wl_platform_audio_stop();
}

int player_open(const char *url, int want_video, int fast_video)
{
	player_stop();
	snprintf(g.url, sizeof g.url, "%s", url);
	g.want_video = want_video;
	g.fast_video = fast_video;
	g.error[0] = '\0';
	g.status[0] = '\0';
	g.has_audio = 0;
	g.behind = 0;
	g.shown_count = g.skipped = g.dropped = 0;
	/* Nothing from the previous file: the position reads 0 until this one starts. */
	g.base_pts = 0;
	g.played = 0;
	g.video_epoch_us = 0;
	g.duration = 0;
	g.state = PL_LOADING;
	/* Big stack: FFmpeg's decoders keep sizeable locals. Video decoding takes most of the CPU, so
	 * it runs below the UI (main) thread, which only needs a slice of each frame: above it, the UI
	 * waited on every frame decode, fell to ~15 fps on SD H.264 and skipped half the frames it
	 * should have shown. Audio alone is cheap and stays above the UI, safe from busy screens. */
	if (plat_thread_create(&g.thread, decode_thread, NULL, 256 * 1024, want_video ? 60 : 70) != 0) {
		set_error("%s", "Couldn't start the player.");
		return -1;
	}
	g.thread_running = 1;
	return 0;
}

void player_pause(int paused)
{
	g.paused = paused;
}

void player_seek(double seconds)
{
	if (seconds < 0)
		seconds = 0;
	if (g.duration > 0 && seconds > g.duration - 1)
		seconds = g.duration - 1;
	g.seek_to = seconds;
	/* Interrupt first: a blocked read gives up, the decode thread sees seek_req and resumes the
	 * stream. (The other order could leave an interrupt behind a seek already handled.) */
	if (g.stream)
		stream_interrupt(g.stream);
	g.seek_req = 1;
}

enum player_state player_get_state(void)
{
	return (enum player_state)g.state;
}

double player_position(void)
{
	double p = clock_now();
	return g.duration > 0 && p > g.duration ? g.duration : p;
}

double player_duration(void)
{
	return g.duration;
}

int player_heap_used_kb(void)
{
	return heap_used_kb();
}

float player_buffer_level(void)
{
	if (!g.stream)
		return 0;
	int64_t target = (g.want_video ? VIDEO_CHUNK : AUDIO_CHUNK) * 2;
	float lvl = (float)stream_buffered(g.stream) / (float)target;
	return lvl > 1 ? 1 : lvl;
}

const char *player_error(void)
{
	return g.error;
}

const char *player_status(void)
{
	return g.status;
}

void player_video_stats(uint32_t *shown, uint32_t *skipped, uint32_t *dropped)
{
	*shown = g.shown_count;
	*skipped = g.skipped;
	*dropped = g.dropped;
}
