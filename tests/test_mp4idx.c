/* Host unit test for the compact MP4 index: a small hand-built file with known tables, so the
 * expected sample order, offsets and times are exact. With a path argument it instead indexes
 * that file and dumps its samples (for comparing against ffprobe -show_packets). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media/mp4idx.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* ---- a growable byte buffer with box helpers -------------------------------------------- */

struct buf {
	uint8_t *d;
	size_t n, cap;
};

static void put(struct buf *b, const void *p, size_t n)
{
	if (b->n + n > b->cap) {
		b->cap = (b->n + n) * 2;
		b->d = realloc(b->d, b->cap);
	}
	memcpy(b->d + b->n, p, n);
	b->n += n;
}
static void u8(struct buf *b, unsigned v) { uint8_t x = (uint8_t)v; put(b, &x, 1); }
static void u16(struct buf *b, unsigned v) { u8(b, v >> 8); u8(b, v); }
static void u32(struct buf *b, uint32_t v) { u16(b, v >> 16); u16(b, v & 0xffff); }
static void u64(struct buf *b, uint64_t v) { u32(b, (uint32_t)(v >> 32)); u32(b, (uint32_t)v); }
static void zeros(struct buf *b, size_t n) { while (n--) u8(b, 0); }
static size_t box(struct buf *b, const char *type)
{
	size_t at = b->n;
	u32(b, 0);
	put(b, type, 4);
	return at;
}
static void end(struct buf *b, size_t at)
{
	uint32_t v = (uint32_t)(b->n - at);
	b->d[at] = v >> 24, b->d[at + 1] = v >> 16, b->d[at + 2] = v >> 8, b->d[at + 3] = v;
}
static size_t fullbox(struct buf *b, const char *type, unsigned version)
{
	size_t at = box(b, type);
	u8(b, version);
	zeros(b, 3);
	return at;
}

struct trak_spec {
	const char *handler; /* vide / soun */
	uint32_t timescale, duration;
	uint32_t sample_count, uniform;
	const uint32_t *sizes;
	int stts_n;
	const uint32_t (*stts)[2];
	int ctts_n;
	const uint32_t (*ctts)[2];
	int stss_n;
	const uint32_t *stss;
	int stsc_n;
	const uint32_t (*stsc)[3];
	int co_n;
	const uint64_t *co;
	int wide;
	int media_time; /* -1: no edit list */
	int empty_edit; /* movie ticks of empty edit before the media, 0 for none */
};

static void build_trak(struct buf *b, const struct trak_spec *t)
{
	int video = !strcmp(t->handler, "vide");
	size_t trak = box(b, "trak");
	if (t->media_time >= 0) {
		size_t edts = box(b, "edts"), elst = fullbox(b, "elst", 0);
		u32(b, t->empty_edit ? 2 : 1);
		if (t->empty_edit) {
			u32(b, (uint32_t)t->empty_edit);
			u32(b, 0xffffffffu); /* media_time -1: an empty edit */
			u32(b, 0x10000);
		}
		u32(b, 1000);
		u32(b, (uint32_t)t->media_time);
		u32(b, 0x10000);
		end(b, elst);
		end(b, edts);
	}
	size_t mdia = box(b, "mdia"), mdhd = fullbox(b, "mdhd", 0);
	zeros(b, 8);
	u32(b, t->timescale);
	u32(b, t->duration);
	zeros(b, 4);
	end(b, mdhd);
	size_t hdlr = fullbox(b, "hdlr", 0);
	u32(b, 0);
	put(b, t->handler, 4);
	zeros(b, 13);
	end(b, hdlr);
	size_t minf = box(b, "minf"), stbl = box(b, "stbl"), stsd = fullbox(b, "stsd", 0);
	u32(b, 1);
	if (video) {
		size_t e = box(b, "avc1");
		zeros(b, 6);
		u16(b, 1);
		zeros(b, 16);
		u16(b, 640);
		u16(b, 480);
		zeros(b, 50); /* resolutions .. pre_defined */
		size_t avcc = box(b, "avcC");
		const uint8_t rec[] = {1, 0x64, 0, 0x1e, 0xff, 0xe1, 0, 0};
		put(b, rec, sizeof rec);
		end(b, avcc);
		end(b, e);
	} else {
		size_t e = box(b, "mp4a");
		zeros(b, 6);
		u16(b, 1);
		zeros(b, 8);
		u16(b, 2);
		u16(b, 16);
		zeros(b, 4);
		u16(b, 48000);
		u16(b, 0);
		size_t esds = fullbox(b, "esds", 0);
		u8(b, 0x03); /* ES_Descriptor: ES_ID, flags, DecoderConfig (13 bytes + the config descriptor) */
		u8(b, 3 + 2 + 13 + 2 + 2);
		u16(b, 1);
		u8(b, 0);
		u8(b, 0x04);
		u8(b, 13 + 2 + 2);
		u8(b, 0x40); /* AAC */
		u8(b, 0x15);
		zeros(b, 3 + 4 + 4);
		u8(b, 0x05); /* AudioSpecificConfig */
		u8(b, 2);
		u8(b, 0x11);
		u8(b, 0x90);
		end(b, esds);
		end(b, e);
	}
	end(b, stsd);
	size_t x = fullbox(b, "stts", 0);
	u32(b, (uint32_t)t->stts_n);
	for (int i = 0; i < t->stts_n; i++)
		u32(b, t->stts[i][0]), u32(b, t->stts[i][1]);
	end(b, x);
	if (t->ctts_n) {
		x = fullbox(b, "ctts", 0);
		u32(b, (uint32_t)t->ctts_n);
		for (int i = 0; i < t->ctts_n; i++)
			u32(b, t->ctts[i][0]), u32(b, t->ctts[i][1]);
		end(b, x);
	}
	if (t->stss_n) {
		x = fullbox(b, "stss", 0);
		u32(b, (uint32_t)t->stss_n);
		for (int i = 0; i < t->stss_n; i++)
			u32(b, t->stss[i]);
		end(b, x);
	}
	x = fullbox(b, "stsc", 0);
	u32(b, (uint32_t)t->stsc_n);
	for (int i = 0; i < t->stsc_n; i++)
		u32(b, t->stsc[i][0]), u32(b, t->stsc[i][1]), u32(b, t->stsc[i][2]);
	end(b, x);
	x = fullbox(b, "stsz", 0);
	u32(b, t->uniform);
	u32(b, t->sample_count);
	if (!t->uniform)
		for (uint32_t i = 0; i < t->sample_count; i++)
			u32(b, t->sizes[i]);
	end(b, x);
	x = fullbox(b, t->wide ? "co64" : "stco", 0);
	u32(b, (uint32_t)t->co_n);
	for (int i = 0; i < t->co_n; i++)
		t->wide ? u64(b, t->co[i]) : u32(b, (uint32_t)t->co[i]);
	end(b, x);
	end(b, stbl);
	end(b, minf);
	end(b, mdia);
	end(b, trak);
}

/* ---- file-backed byte source ------------------------------------------------------------- */

struct mem {
	const uint8_t *d;
	size_t n, pos;
	FILE *f;
};
static int mem_read(void *c, uint8_t *buf, size_t n)
{
	struct mem *m = c;
	if (m->f)
		return (int)fread(buf, 1, n, m->f);
	if (m->pos >= m->n)
		return 0;
	if (n > m->n - m->pos)
		n = m->n - m->pos;
	memcpy(buf, m->d + m->pos, n);
	m->pos += n;
	return (int)n;
}
static int mem_seek(void *c, int64_t pos)
{
	struct mem *m = c;
	if (m->f)
		return fseeko(m->f, pos, SEEK_SET);
	m->pos = pos < 0 ? 0 : (size_t)pos > m->n ? m->n : (size_t)pos;
	return 0;
}

static int dump(const char *path, int limit)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return 1;
	}
	fseeko(f, 0, SEEK_END);
	int64_t size = ftello(f);
	struct mem m = {.f = f};
	struct mp4_io io = {&m, mem_read, mem_seek};
	int64_t off, len;
	if (mp4_find_moov(&io, size, &off, &len)) {
		printf("no moov\n");
		return 1;
	}
	char err[128];
	mp4idx *x = mp4idx_open(&io, off, len, err, sizeof err);
	if (!x) {
		printf("open failed: %s\n", err);
		return 1;
	}
	for (int k = 0; k < MP4_KINDS; k++) {
		const struct mp4_track_info *t = mp4idx_track(x, (enum mp4_kind)k);
		if (t)
			printf("track %d codec %d ts %u samples %u %dx%d ch %d rate %d extra %d\n", k, t->codec, t->timescale,
				t->sample_count, t->width, t->height, t->channels, t->sample_rate, t->extradata_size);
	}
	printf("moov %lld bytes at %lld, duration %.3f s, index %zu KB\n", (long long)len, (long long)off,
		mp4idx_duration(x), mp4idx_bytes(x) / 1024);
	struct mp4_sample s;
	long n = 0, per_kind[MP4_KINDS] = {0};
	while (mp4idx_next(x, &s)) {
		per_kind[s.kind]++;
		if (n++ < limit)
			printf("S %d %lld %u %lld %lld %d\n", s.kind, (long long)s.pos, s.size, (long long)s.dts, (long long)s.pts,
				s.key);
	}
	printf("total %ld samples (video %ld, audio %ld)\n", n, per_kind[0], per_kind[1]);
	mp4idx_close(x);
	fclose(f);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1)
		return dump(argv[1], argc > 2 ? atoi(argv[2]) : 20);

	/* Video: 6 samples, two chunks (4 + 2), keyframes at samples 1 and 4, B-frame style offsets,
	 * and an edit list starting 40 ticks in. Audio: 5 uniform samples in chunks of 3 + 2. */
	static const uint32_t vsizes[] = {100, 50, 60, 70, 80, 90}, vkeys[] = {1, 4};
	static const uint32_t vstts[][2] = {{6, 20}}, vctts[][2] = {{2, 40}, {4, 0}};
	static const uint32_t vstsc[][3] = {{1, 4, 1}, {2, 2, 1}};
	static const uint64_t vco[] = {1000, 2000};
	static const uint32_t astts[][2] = {{5, 1024}}, astsc[][3] = {{1, 3, 1}};
	static const uint64_t aco[] = {1500, 1800};
	struct trak_spec v = {"vide", 600, 120, 6, 0, vsizes, 1, vstts, 2, vctts, 2, vkeys, 2, vstsc, 2, vco, 0, 40, 0};
	/* The audio starts 100 ms late: an empty edit of 100 movie ticks (timescale 1000) = 4800 ticks. */
	struct trak_spec a = {"soun", 48000, 5120, 5, 10, NULL, 1, astts, 0, NULL, 0, NULL, 1, astsc, 2, aco, 1, 0, 100};

	struct buf moov = {0};
	size_t mvhd = fullbox(&moov, "mvhd", 0);
	zeros(&moov, 8);
	u32(&moov, 1000);
	zeros(&moov, 80);
	end(&moov, mvhd);
	build_trak(&moov, &v);
	build_trak(&moov, &a);
	struct buf file = {0};
	size_t ftyp = box(&file, "ftyp");
	put(&file, "isom", 4);
	zeros(&file, 4);
	end(&file, ftyp);
	size_t mdat = box(&file, "mdat");
	zeros(&file, 500);
	end(&file, mdat);
	size_t moov_at = box(&file, "moov");
	put(&file, moov.d, moov.n);
	end(&file, moov_at);

	struct mem m = {.d = file.d, .n = file.n};
	struct mp4_io io = {&m, mem_read, mem_seek};
	int64_t off, len;
	CHECK(mp4_find_moov(&io, (int64_t)file.n, &off, &len) == 0);
	CHECK(off == (int64_t)moov_at + 8 && len == (int64_t)moov.n);
	char err[128];
	mp4idx *x = mp4idx_open(&io, off, len, err, sizeof err);
	CHECK(x != NULL);
	if (!x) {
		printf("%s\n", err);
		return 1;
	}
	const struct mp4_track_info *vi = mp4idx_track(x, MP4_VIDEO), *ai = mp4idx_track(x, MP4_AUDIO);
	CHECK(vi && vi->codec == MP4_CODEC_H264 && vi->width == 640 && vi->height == 480 && vi->extradata_size == 8);
	CHECK(vi && vi->sample_count == 6 && vi->timescale == 600 && vi->extradata[0] == 1);
	CHECK(ai && ai->codec == MP4_CODEC_AAC && ai->channels == 2 && ai->sample_rate == 48000);
	CHECK(ai && ai->extradata_size == 2 && ai->extradata[0] == 0x11 && ai->extradata[1] == 0x90);
	CHECK(mp4idx_duration(x) > 0.19 && mp4idx_duration(x) < 0.21 + 0.1);

	/* Samples come in file order, interleaved across the tracks. */
	static const struct {
		int kind;
		int64_t pos;
		uint32_t size;
		int64_t dts, pts;
		int key;
	} want[] = {
		{0, 1000, 100, 0, 0, 1}, {0, 1100, 50, 20, 20, 0}, {0, 1150, 60, 40, 0, 0}, {0, 1210, 70, 60, 20, 1},
		{1, 1500, 10, 0, 0, 1}, {1, 1510, 10, 1024, 1024, 1}, {1, 1520, 10, 2048, 2048, 1},
		{1, 1800, 10, 3072, 3072, 1}, {1, 1810, 10, 4096, 4096, 1},
		{0, 2000, 80, 80, 40, 0}, {0, 2080, 90, 100, 60, 0},
	};
	struct mp4_sample s;
	for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
		int ok = mp4idx_next(x, &s) && s.kind == want[i].kind && s.pos == want[i].pos && s.size == want[i].size &&
			s.dts == want[i].dts && s.key == want[i].key;
		/* pts of the first video sample: dts 0 + ctts 40 - media_time 40 = 0; the rest follow. */
		ok = ok && s.pts == (want[i].kind ? want[i].dts + 4800 : want[i].pts); /* audio: the empty edit's delay */
		if (!ok)
			printf("sample %zu: kind %d pos %lld size %u dts %lld pts %lld key %d\n", i, s.kind, (long long)s.pos, s.size,
				(long long)s.dts, (long long)s.pts, s.key);
		CHECK(ok);
	}
	CHECK(mp4idx_next(x, &s) == 0);

	/* Seeking: video lands on the keyframe at or before, audio on the sample at the time. */
	mp4idx_seek(x, 0.2); /* video: 120 + 40 ticks, past the end, so the last keyframe (sample 4, index 3) */
	CHECK(mp4idx_next(x, &s) && s.kind == 0 && s.pos == 1210 && s.key == 1);
	CHECK(mp4idx_next(x, &s) && s.kind == 1 && s.pos == 1810 && s.dts == 4096 && s.pts == 4096 + 4800);
	/* audio: 9600 ticks - the 4800 delay = 4800 -> sample 4 (dts 4096) */
	CHECK(mp4idx_next(x, &s) && s.kind == 0 && s.pos == 2000);
	mp4idx_seek(x, 0);
	CHECK(mp4idx_next(x, &s) && s.pos == 1000);

	/* One track on its own. */
	mp4idx_enable(x, MP4_VIDEO, 0);
	mp4idx_seek(x, 0);
	int n = 0;
	while (mp4idx_next(x, &s)) {
		CHECK(s.kind == 1);
		n++;
	}
	CHECK(n == 5);
	mp4idx_close(x);

	/* Not an MP4, and a truncated moov. */
	static const uint8_t junk[] = "OggS\0\x02\0\0\0\0\0\0\0\0\0\0";
	struct mem jm = {.d = junk, .n = sizeof junk};
	struct mp4_io jio = {&jm, mem_read, mem_seek};
	CHECK(mp4_find_moov(&jio, sizeof junk, &off, &len) != 0);
	struct mem tm = {.d = file.d, .n = moov_at + 8 + moov.n / 2};
	struct mp4_io tio = {&tm, mem_read, mem_seek};
	CHECK(mp4idx_open(&tio, (int64_t)moov_at + 8, (int64_t)moov.n, err, sizeof err) == NULL && err[0]);

	free(moov.d);
	free(file.d);
	printf("%s\n", failures ? "FAILED" : "ok");
	return failures != 0;
}
