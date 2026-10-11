#include "media/player.h"

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
#include "media/pcm_ring.h"
#include "media/stream.h"
#include "wl/base/base.h"
#include "wl/platform/platform.h"

#define OUT_RATE 48000
#define RING_FRAMES 32768 /* ~0.7 s of PCM */
#define VQUEUE 4      /* decoded frames queued normally */
#define VQUEUE_MAX 24 /* while audio is short (see vqueue_push) */
#define AUDIO_CHUNK (256 * 1024)
#define VIDEO_CHUNK (512 * 1024)

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

	/* video frames: decode thread -> UI thread */
	iac_mutex vlock;
	struct vslot vq[VQUEUE_MAX];
	int vcount;
	AVFrame *shown;
	struct player_frame shown_info;
	uint64_t video_epoch_us; /* wall clock origin when there is no audio */
	volatile int behind;     /* UI saw late frames: decoder skips non-reference frames */
	uint32_t shown_count, skipped, dropped; /* frames displayed, passed over as late, dropped queued */
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

/* Blocks (politely) until there's room; returns 0 if stopped/seeking instead.
 * With audio, the clock only moves while audio plays. In Ogg files the audio for a stretch can
 * sit well after its video, so waiting on a full queue could starve the audio and freeze the
 * clock with it (the decoder never gets to the audio). While the audio ring is short the queue
 * may grow to VQUEUE_MAX, and past that the oldest frame is dropped rather than wait. */
static int vqueue_push(AVFrame *f, double pts)
{
	for (;;) {
		if (g.stop || g.seek_req)
			return 0;
		int starving = g.has_audio && pcm_ring_count(&g.ring) < OUT_RATE / 5;
		int cap = starving ? VQUEUE_MAX : VQUEUE;
		plat_mutex_lock(&g.vlock);
		/* Only a dry audio ring with a full long queue is the deadlock; dropping any earlier
		 * throws away frames that were about to be shown. */
		if (g.vcount >= VQUEUE_MAX && g.has_audio && pcm_ring_count(&g.ring) == 0) {
			av_frame_free(&g.vq[0].frame);
			memmove(&g.vq[0], &g.vq[1], sizeof g.vq[0] * (size_t)(g.vcount - 1));
			g.vcount--;
			g.dropped++;
		}
		if (g.vcount < cap) {
			g.vq[g.vcount].frame = av_frame_clone(f);
			g.vq[g.vcount].pts = pts;
			g.vcount++;
			plat_mutex_unlock(&g.vlock);
			return 1;
		}
		plat_mutex_unlock(&g.vlock);
		usleep(4000);
	}
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

static AVCodecContext *open_codec(AVStream *st, int fast)
{
	const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec)
		return NULL;
	AVCodecContext *c = avcodec_alloc_context3(codec);
	if (!c)
		return NULL;
	avcodec_parameters_to_context(c, st->codecpar);
	c->thread_count = 1;
	if (fast) {
		c->skip_loop_filter = AVDISCARD_ALL;
		c->flags2 |= AV_CODEC_FLAG2_FAST;
	}
	if (avcodec_open2(c, codec, NULL) < 0)
		avcodec_free_context(&c);
	return c;
}

static void *decode_thread(void *arg)
{
	(void)arg;
	AVFormatContext *fmt = NULL;
	AVIOContext *io = NULL;
	AVCodecContext *adec = NULL, *vdec = NULL;
	SwrContext *swr = NULL;
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	int16_t *pcm = NULL;
	int pcm_cap = 0, aidx = -1, vidx = -1;

	g.stream = stream_open(g.url, g.want_video ? VIDEO_CHUNK : AUDIO_CHUNK, g.want_video ? 3 : 2);
	uint8_t *iobuf = av_malloc(32768);
	io = g.stream && iobuf ? avio_alloc_context(iobuf, 32768, 0, g.stream, io_read, NULL, io_seek) : NULL;
	fmt = avformat_alloc_context();
	if (!pkt || !frame || !io || !fmt) {
		set_error("%s", "Out of memory.");
		goto done;
	}
	fmt->pb = io;
	if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0 || avformat_find_stream_info(fmt, NULL) < 0) {
		if (stream_error(g.stream)[0])
			WL_LOG_W("PLAYER", "stream: %s", stream_error(g.stream));
		set_error("Couldn't open the file%s.", stream_error(g.stream)[0] ? " (network)" : "");
		goto done;
	}
	aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (g.want_video)
		vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (aidx >= 0)
		adec = open_codec(fmt->streams[aidx], 0);
	if (vidx >= 0)
		vdec = open_codec(fmt->streams[vidx], g.fast_video);
	if (!adec && !vdec) {
		set_error("%s", "No playable audio or video in this file.");
		goto done;
	}
	g.has_audio = adec != NULL;
	g.duration = fmt->duration > 0 ? (double)fmt->duration / AV_TIME_BASE : 0;
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

	int eof = 0;
	while (!g.stop) {
		if (g.seek_req) {
			double to = g.seek_to;
			g.seek_req = 0;
			/* player_seek interrupted any blocked read; reading resumes from here. */
			stream_resume(g.stream);
			io->eof_reached = 0;
			io->error = 0;
			int64_t ts = (int64_t)(to * AV_TIME_BASE);
			if (av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
				if (adec)
					avcodec_flush_buffers(adec);
				if (vdec)
					avcodec_flush_buffers(vdec);
				vqueue_clear();
				flush_output();
				g.base_pts = to;
				g.video_epoch_us = 0;
				eof = 0;
				if (g.state == PL_ENDED)
					g.state = g.paused ? PL_PAUSED : PL_PLAYING;
			}
		}
		if (eof) {
			/* Let the output drain, then report the end. */
			if (pcm_ring_count(&g.ring) == 0 && g.vcount == 0 && g.state != PL_ENDED)
				g.state = PL_ENDED;
			usleep(20000);
			continue;
		}
		if (g.paused && g.state == PL_PLAYING)
			g.state = PL_PAUSED;
		else if (!g.paused && g.state == PL_PAUSED) {
			g.state = PL_PLAYING;
			if (!g.has_audio && g.video_epoch_us) /* resume the wall clock where it stopped */
				g.video_epoch_us = wl_platform_time_us();
		}

		int rc = av_read_frame(fmt, pkt);
		if (rc < 0) {
			if (g.stop || g.seek_req)
				continue;
			eof = 1;
			continue;
		}
		if (pkt->stream_index == aidx && adec) {
			if (avcodec_send_packet(adec, pkt) == 0) {
				while (avcodec_receive_frame(adec, frame) == 0) {
					int out_max = swr_get_out_samples(swr, frame->nb_samples);
					if (out_max > pcm_cap) {
						free(pcm);
						pcm = malloc((size_t)out_max * 4);
						pcm_cap = pcm ? out_max : 0;
					}
					if (!pcm)
						break;
					int out = swr_convert(swr, (uint8_t **)&pcm, out_max, (const uint8_t **)frame->extended_data,
						frame->nb_samples);
					/* Push it all, waiting for the callback to make room. */
					for (int done_frames = 0; out > 0 && done_frames < out && !g.stop && !g.seek_req;) {
						done_frames += (int)pcm_ring_write(&g.ring, pcm + done_frames * 2,
							(uint32_t)(out - done_frames));
						if (done_frames < out)
							usleep(5000);
					}
				}
			}
		} else if (pkt->stream_index == vidx && vdec) {
			vdec->skip_frame = g.behind ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
			if (avcodec_send_packet(vdec, pkt) == 0) {
				AVRational tb = fmt->streams[vidx]->time_base;
				while (avcodec_receive_frame(vdec, frame) == 0) {
					int64_t p = frame->best_effort_timestamp;
					double pts = p == AV_NOPTS_VALUE ? g.base_pts : (double)p * av_q2d(tb);
					if (!g.has_audio && !g.video_epoch_us) {
						g.base_pts = pts;
						g.video_epoch_us = wl_platform_time_us();
					}
					if (!vqueue_push(frame, pts))
						break;
				}
			}
		}
		av_packet_unref(pkt);
	}

done:
	free(pcm);
	swr_free(&swr);
	avcodec_free_context(&adec);
	avcodec_free_context(&vdec);
	if (fmt)
		avformat_close_input(&fmt);
	if (io) {
		av_freep(&io->buffer);
		avio_context_free(&io);
	} else {
		av_free(iobuf);
	}
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
	g.has_audio = 0;
	g.behind = 0;
	g.shown_count = g.skipped = g.dropped = 0;
	/* Nothing from the previous file: the position reads 0 until this one starts. */
	g.base_pts = 0;
	g.played = 0;
	g.video_epoch_us = 0;
	g.duration = 0;
	g.state = PL_LOADING;
	/* Big stack: FFmpeg's decoders keep sizeable locals. */
	if (plat_thread_create(&g.thread, decode_thread, NULL, 256 * 1024, 70) != 0) {
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

void player_video_stats(uint32_t *shown, uint32_t *skipped, uint32_t *dropped)
{
	*shown = g.shown_count;
	*skipped = g.skipped;
	*dropped = g.dropped;
}
