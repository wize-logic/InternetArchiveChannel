/* Phase 0 / Spike 2: can Broadway decode IA's video derivatives in real time?
 *
 * Files are read from sd:/apps/iachannel/media/:
 *   122Eyes1950_512kb.mp4        MPEG-4 Part 2, 320x240   ("512Kb MPEG4" derivative)
 *   122Eyes1950_h264_640x480.mp4 H.264, 640x480           (same film, "h.264" derivative)
 *   SS14E04_h264_854x480.mp4     H.264, 854x480           (typical modern IA upload)
 *
 * For each file:
 *   1. decode-only (video + audio), giving CPU % needed for real-time playback;
 *   2. H.264 only: again with the loop filter skipped and FLAG2_FAST;
 *   3. decode + re-tile + GX draw, unthrottled.
 * First run only: checks the TEV YUV->RGB output against a CPU reference by reading the EFB back.
 * Pass bar (plan): 320x240 MPEG-4 at 30 fps with > 40% CPU headroom, i.e. load < 60%. */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <fat.h>
#include <gccore.h>
#include <wiiuse/wpad.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>

#include "core/log.h"
#include "core/platform.h"
#include "gfx/gx.h"
#include "gfx/yuv.h"

#define APP_DIR "sd:/apps/iachannel"
#define MEDIA_DIR APP_DIR "/media"
#define DECODE_SECONDS 20.0 /* media time per decode-only pass */
#define DRAW_FRAMES 300

static struct gx_state g_gx;

struct media {
	FILE *f;
	AVFormatContext *fmt;
	AVIOContext *io;
	AVCodecContext *vdec, *adec;
	int vidx, aidx;
	double fps;
};


static int io_read(void *opaque, uint8_t *buf, int size)
{

	size_t n = fread(buf, 1, (size_t)size, opaque);
	return n ? (int)n : AVERROR_EOF;
}

static int64_t io_seek(void *opaque, int64_t off, int whence)
{
	FILE *f = opaque;

	if (whence == AVSEEK_SIZE) {
		long cur = ftell(f);
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, cur, SEEK_SET);
		return size;
	}
	if (fseek(f, (long)off, whence & ~AVSEEK_FORCE) != 0)
		return -1;
	return ftell(f);
}

static AVCodecContext *open_decoder(AVStream *st, int fast)
{
	const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec)
		return NULL;
	AVCodecContext *c = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(c, st->codecpar);
	c->thread_count = 1;
	if (fast) {
		c->skip_loop_filter = AVDISCARD_ALL;
		c->flags2 |= AV_CODEC_FLAG2_FAST;
	}
	if (avcodec_open2(c, codec, NULL) < 0) {
		avcodec_free_context(&c);
		return NULL;
	}
	return c;
}

static void media_close(struct media *m)
{
	avcodec_free_context(&m->vdec);
	avcodec_free_context(&m->adec);
	if (m->fmt)
		avformat_close_input(&m->fmt);
	if (m->io) {
		av_freep(&m->io->buffer);
		avio_context_free(&m->io);
	}
	if (m->f)
		fclose(m->f);
	memset(m, 0, sizeof *m);
}

static int media_open(struct media *m, const char *path, int fast)
{
	memset(m, 0, sizeof *m);
	m->f = fopen(path, "rb");
	if (!m->f)
		return -1;
	setvbuf(m->f, NULL, _IOFBF, 256 * 1024);
	unsigned char *iobuf = av_malloc(64 * 1024);
	m->io = avio_alloc_context(iobuf, 64 * 1024, 0, m->f, io_read, NULL, io_seek);
	m->fmt = avformat_alloc_context();
	m->fmt->pb = m->io;
	if (avformat_open_input(&m->fmt, NULL, NULL, NULL) < 0 || avformat_find_stream_info(m->fmt, NULL) < 0) {
		log_printf("  cannot parse %s\n", path);
		media_close(m);
		return -1;
	}
	m->vidx = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	m->aidx = av_find_best_stream(m->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (m->vidx < 0 || !(m->vdec = open_decoder(m->fmt->streams[m->vidx], fast))) {
		log_printf("  no usable video stream\n");
		media_close(m);
		return -1;
	}
	if (m->aidx >= 0)
		m->adec = open_decoder(m->fmt->streams[m->aidx], 0);
	AVRational r = m->fmt->streams[m->vidx]->avg_frame_rate;
	m->fps = r.num && r.den ? av_q2d(r) : 30.0;
	return 0;
}

static void log_heap(const char *tag)
{
	struct mallinfo mi = mallinfo();
	log_printf("  [heap %s] in use %d KB, arena %d KB\n", tag, mi.uordblks / 1024, mi.arena / 1024);
}

struct decode_stats {
	int frames;
	double audio_seconds;
	uint64_t demux_us, video_us, audio_us, upload_us, draw_us;
};

/* Decodes up to max_frames video frames. on_frame (optional) runs after each one. */
static void decode_loop(struct media *m, int max_frames, struct decode_stats *s,
	void (*on_frame)(AVFrame *, struct decode_stats *))
{
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	memset(s, 0, sizeof *s);
	while (s->frames < max_frames) {
		uint64_t t = plat_us();
		int rc = av_read_frame(m->fmt, pkt);
		s->demux_us += plat_us() - t;
		if (rc < 0)
			break;
		if (pkt->stream_index == m->vidx) {
			t = plat_us();
			avcodec_send_packet(m->vdec, pkt);
			while (avcodec_receive_frame(m->vdec, frame) == 0) {
				s->video_us += plat_us() - t;
				s->frames++;
				if (on_frame)
					on_frame(frame, s);
				t = plat_us();
			}
			s->video_us += plat_us() - t;
		} else if (m->adec && pkt->stream_index == m->aidx) {
			t = plat_us();
			avcodec_send_packet(m->adec, pkt);
			while (avcodec_receive_frame(m->adec, frame) == 0)
				s->audio_seconds += (double)frame->nb_samples / frame->sample_rate;
			s->audio_us += plat_us() - t;
		}
		av_packet_unref(pkt);
	}
	av_frame_free(&frame);
	av_packet_free(&pkt);
}

/* CPU reference for the TEV maths: BT.601 limited range. */
static void ref_rgb(int y, int u, int v, int out[3])
{
	double c = 1.164 * (y - 16), d = u - 128, e = v - 128;
	double r = c + 1.596 * e, g = c - 0.392 * d - 0.813 * e, b = c + 2.017 * d;
	double rgb[3] = {r, g, b};
	for (int i = 0; i < 3; i++)
		out[i] = rgb[i] < 0 ? 0 : rgb[i] > 255 ? 255 : (int)(rgb[i] + 0.5);
}

/* Reads one pixel from an RGBA8 texture copy (4x4 tiles: 32 bytes AR, then 32 bytes GB). */
static void rgba8_pixel(const uint8_t *buf, int width, int x, int y, int out[3])
{
	int tile = (y / 4) * (width / 4) + x / 4;
	int i = ((y % 4) * 4 + x % 4) * 2;
	const uint8_t *t = buf + tile * 64;
	out[0] = t[i + 1];
	out[1] = t[32 + i];
	out[2] = t[32 + i + 1];
}

static int g_verify_done;

static uint8_t *efb_readback(int cw, int ch)
{
	uint8_t *copy = memalign(32, (size_t)cw * ch * 4);
	GX_SetTexCopySrc(0, 0, (u16)cw, (u16)ch);
	GX_SetTexCopyDst((u16)cw, (u16)ch, GX_TF_RGBA8, GX_FALSE);
	DCInvalidateRange(copy, (u32)cw * ch * 4);
	GX_CopyTex(copy, GX_FALSE);
	GX_PixModeSync();
	GX_DrawDone();
	DCInvalidateRange(copy, (u32)cw * ch * 4);
	return copy;
}

/* Control for the TEV check: a quad in a known konst colour must read back exactly. */
static int readback_selftest(void)
{
	GXColor k = {200, 100, 50, 255};
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_InvVtxCache();
	GX_SetNumChans(1);
	GX_SetNumTexGens(0);
	GX_SetNumTevStages(1);
	GX_SetTevKColor(GX_KCOLOR0, k);
	GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	/* Opaque alpha: blending is on (SRCALPHA/INVSRCALPHA). */
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_Begin(GX_QUADS, GX_VTXFMT1, 4);
	GX_Position2f32(0, 0);
	GX_Position2f32(64, 0);
	GX_Position2f32(64, 64);
	GX_Position2f32(0, 64);
	GX_End();
	GX_DrawDone();
	uint8_t *copy = efb_readback(64, 64);
	int got[3];
	rgba8_pixel(copy, 64, 10, 10, got);
	free(copy);
	int ok = got[0] == 200 && got[1] == 100 && got[2] == 50;
	log_printf("EFB readback control: want (200,100,50) got (%d,%d,%d): %s\n", got[0], got[1], got[2],
		ok ? "PASS" : "FAIL");
	return ok;
}

/* Draws the frame at 2x (nearest sampling) into the top-left of the EFB, copies it back,
 * and compares 64 sample points with ref_rgb(). */
static void verify_tev(AVFrame *f)
{
	int w = f->width > 320 ? 320 : f->width, h = f->height > 240 ? 240 : f->height;
	struct yuv_tex t;
	if (yuv_tex_init(&t, f->width, f->height, 1) != 0)
		return;
	yuv_tex_upload(&t, f->data, f->linesize);
	yuv_begin(&t);
	yuv_draw(&t, 0, 0, (float)f->width * 2, (float)f->height * 2);
	GX_DrawDone();

	int cw = w * 2, ch = h * 2;
	uint8_t *copy = efb_readback(cw, ch);

	int max_err = 0;
	for (int i = 0; i < 64; i++) {
		int sx = 4 + (i % 8) * (w - 8) / 8, sy = 4 + (i / 8) * (h - 8) / 8;
		int yv = f->data[0][sy * f->linesize[0] + sx];
		int uv = f->data[1][(sy / 2) * f->linesize[1] + sx / 2];
		int vv = f->data[2][(sy / 2) * f->linesize[2] + sx / 2];
		int want[3], got[3];
		ref_rgb(yv, uv, vv, want);
		rgba8_pixel(copy, cw, sx * 2, sy * 2, got);
		for (int c = 0; c < 3; c++) {
			int e = abs(want[c] - got[c]);
			if (e > max_err)
				max_err = e;
		}
		if (i < 3)
			log_printf("  sample yuv(%d,%d,%d) want rgb(%d,%d,%d) got (%d,%d,%d)\n", yv, uv, vv,
				want[0], want[1], want[2], got[0], got[1], got[2]);
	}
	log_printf("TEV YUV->RGB check: max channel error %d over 64 samples: %s\n", max_err,
		max_err <= 6 ? "PASS" : "FAIL");
	free(copy);
	yuv_tex_free(&t);
	g_verify_done = 1;
}

static struct yuv_tex g_tex;
static int g_tex_ready;

static void draw_frame(AVFrame *f, struct decode_stats *s)
{
	if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P)
		return;
	if (!g_verify_done) {
		readback_selftest();
		verify_tev(f);
	}
	if (!g_tex_ready) {
		if (yuv_tex_init(&g_tex, f->width, f->height, 0) != 0)
			return;
		g_tex_ready = 1;
	}
	uint64_t t = plat_us();
	yuv_tex_upload(&g_tex, f->data, f->linesize);
	s->upload_us += plat_us() - t;

	t = plat_us();
	/* Fit to 640x480, keeping the aspect ratio. */
	float sw = 640.0f / f->width, sh = 480.0f / f->height, sc = sw < sh ? sw : sh;
	float w = f->width * sc, h = f->height * sc;
	yuv_begin(&g_tex);
	yuv_draw(&g_tex, (640 - w) / 2, (480 - h) / 2, w, h);
	gx_present(&g_gx, 0);
	s->draw_us += plat_us() - t;
}

struct result {
	char name[48];
	int w, h;
	double fps, load_decode, load_fast, load_full, draw_fps;
};

static double load_pct(const struct decode_stats *s, double fps, int include_draw)
{
	if (!s->frames)
		return 0;
	double media_s = s->frames / fps;
	uint64_t us = s->demux_us + s->video_us + s->audio_us;
	if (include_draw)
		us += s->upload_us + s->draw_us;
	return (double)us / 1e6 / media_s * 100.0;
}

static void bench_file(const char *file, struct result *r)
{
	char path[256];
	struct media m;
	struct decode_stats s;
	snprintf(path, sizeof path, MEDIA_DIR "/%s", file);
	snprintf(r->name, sizeof r->name, "%s", file);

	log_printf("\n== %s\n", file);
	if (media_open(&m, path, 0) != 0) {
		log_printf("  missing or unreadable, skipped\n");
		return;
	}
	r->w = m.vdec->width;
	r->h = m.vdec->height;
	r->fps = m.fps;
	const char *vname = m.vdec->codec->name;
	log_printf("  video %s %dx%d @ %.2f fps, audio %s\n", vname, r->w, r->h, r->fps,
		m.adec ? m.adec->codec->name : "none");
	int is_h264 = strcmp(vname, "h264") == 0;
	int max_frames = (int)(DECODE_SECONDS * m.fps);

	log_heap("opened");
	decode_loop(&m, max_frames, &s, NULL);
	log_heap("after decode-only");
	r->load_decode = load_pct(&s, m.fps, 0);
	log_printf("  decode-only: %d frames, video %.1f ms/frame (max %.1f fps), audio %.1f ms per s, "
		"demux %.1f ms/frame -> %.0f%% CPU at real time\n", s.frames,
		s.frames ? s.video_us / 1000.0 / s.frames : 0,
		s.video_us ? s.frames / (s.video_us / 1e6) : 0,
		s.audio_seconds > 0 ? s.audio_us / 1000.0 / s.audio_seconds : 0,
		s.frames ? s.demux_us / 1000.0 / s.frames : 0, r->load_decode);
	media_close(&m);

	log_heap("closed");
	if (is_h264 && media_open(&m, path, 1) == 0) {
		decode_loop(&m, max_frames, &s, NULL);
		log_heap("after fast");
		r->load_fast = load_pct(&s, m.fps, 0);
		log_printf("  fast (no loop filter): video %.1f ms/frame (max %.1f fps) -> %.0f%% CPU\n",
			s.frames ? s.video_us / 1000.0 / s.frames : 0,
			s.video_us ? s.frames / (s.video_us / 1e6) : 0, r->load_fast);
		media_close(&m);
	}

	if (media_open(&m, path, is_h264) == 0) {
		uint64_t t0 = plat_us();
		decode_loop(&m, DRAW_FRAMES, &s, draw_frame);
		uint64_t wall = plat_us() - t0;
		r->load_full = load_pct(&s, m.fps, 1);
		r->draw_fps = wall ? s.frames / (wall / 1e6) : 0;
		log_printf("  decode+draw%s: %d frames, %.1f fps unthrottled; re-tile %.2f ms/frame, "
			"GX %.2f ms/frame -> %.0f%% CPU at real time\n", is_h264 ? " (fast)" : "", s.frames,
			r->draw_fps, s.frames ? s.upload_us / 1000.0 / s.frames : 0,
			s.frames ? s.draw_us / 1000.0 / s.frames : 0, r->load_full);
		if (g_tex_ready) {
			yuv_tex_free(&g_tex);
			g_tex_ready = 0;
		}
		media_close(&m);
	}
}

int main(void)
{
	gx_init(&g_gx);
	WPAD_Init();
	int have_sd = fatInitDefault();
	log_init(have_sd ? APP_DIR "/spike_video.log" : NULL);
	log_printf("IAChannel spike 2: video decode (FFmpeg %s)\n", av_version_info());
	struct mallinfo mi = mallinfo();
	log_printf("heap at start: %d KB in use\n", mi.uordblks / 1024);

	static const char *files[] = {
		"122Eyes1950_512kb.mp4",
		"122Eyes1950_h264_640x480.mp4",
		"SS14E04_h264_854x480.mp4",
	};
	struct result res[3];
	memset(res, 0, sizeof res);
	for (int i = 0; i < 3; i++)
		bench_file(files[i], &res[i]);

	mi = mallinfo();
	log_printf("\nheap at end: %d KB in use, %d KB arena\n", mi.uordblks / 1024, mi.arena / 1024);
	log_printf("\n=== Spike 2 results (CPU %% needed for real-time playback incl. audio) ===\n");
	for (int i = 0; i < 3; i++) {
		if (!res[i].w)
			continue;
		log_printf("%-30s %dx%d %5.2ffps  decode %3.0f%%  fast %3.0f%%  +draw %3.0f%%\n", res[i].name,
			res[i].w, res[i].h, res[i].fps, res[i].load_decode, res[i].load_fast, res[i].load_full);
	}
	if (res[0].w)
		log_printf("bar: 320x240 MPEG-4 load < 60%%: %s\n", res[0].load_full < 60 ? "PASS" : "FAIL");
	log_printf("\nPress HOME to exit.\n");
	log_close();

	console_init(gx_front_xfb(&g_gx), 20, 20, g_gx.mode->fbWidth, g_gx.mode->xfbHeight,
		g_gx.mode->fbWidth * VI_DISPLAY_PIX_SZ);
	printf("\x1b[2;0HSpike 2 done. Results in %s/spike_video.log\nPress HOME to exit.\n", APP_DIR);
	for (;;) {
		WPAD_ScanPads();
		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
			break;
		VIDEO_WaitVSync();
	}
	return 0;
}
