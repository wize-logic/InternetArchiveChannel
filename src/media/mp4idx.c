#include "media/mp4idx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Index memory we will hold at most (sizes + chunk offsets + timing runs of every kept track). */
#define MAX_INDEX_BYTES (40u * 1024 * 1024)
#define MAX_STSD (64 * 1024)

#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

struct run {
	uint32_t count;
	int32_t val;
};
struct stsc_run {
	uint32_t first, spc;
};

struct cursor {
	uint32_t i;           /* next sample */
	uint32_t chunk, j;    /* its chunk, and its place in that chunk */
	uint32_t srun;        /* stsc run holding the chunk */
	int64_t off;          /* its byte offset */
	uint32_t trun, tleft; /* stts run, and samples left in it */
	int64_t dts;
	uint32_t crun, cleft; /* the same for ctts */
	uint32_t ks;          /* first stss entry at or after the sample */
	int done;
};

struct trk {
	int kind; /* enum mp4_kind, -1 until hdlr */
	int claimed, skip;
	struct mp4_track_info info;
	uint32_t timescale;
	uint64_t mdhd_duration;
	int64_t media_time;  /* start of the media in the edit list, track ticks */
	int64_t delay_movie; /* leading empty edit, movie ticks */
	int64_t delay;       /* the same in track ticks */
	uint8_t *extra;
	uint32_t nsamples, uniform;
	uint32_t *sizes;
	struct run *stts, *ctts;
	uint32_t stts_n, ctts_n;
	uint32_t *stss;
	uint32_t stss_n;
	int has_stss;
	struct stsc_run *stsc;
	uint32_t stsc_n;
	uint64_t *co;
	uint32_t co_n;
	int enabled;
	struct cursor cur;
};

struct mp4idx {
	struct trk *trk[MP4_KINDS];
	size_t bytes;
	uint32_t movie_ts;
};

/* ---- reading ---------------------------------------------------------------------------- */

struct parser {
	const struct mp4_io *io;
	int64_t pos;
	mp4idx *x;
	struct trk *t; /* the trak being read */
	char *err;
	size_t errcap;
	int failed;
};

static void fail(struct parser *p, const char *msg)
{
	if (!p->failed && p->err && p->errcap)
		snprintf(p->err, p->errcap, "%s", msg);
	p->failed = 1;
}

static int rd(struct parser *p, void *buf, size_t n)
{
	uint8_t *b = buf;
	while (n) {
		int got = p->io->read(p->io->ctx, b, n);
		if (got <= 0) {
			fail(p, "Couldn't read the video index.");
			return -1;
		}
		b += got;
		n -= (size_t)got;
		p->pos += got;
	}
	return 0;
}

static int goto_pos(struct parser *p, int64_t pos)
{
	if (pos == p->pos)
		return 0;
	if (p->io->seek(p->io->ctx, pos) != 0) {
		fail(p, "Couldn't read the video index.");
		return -1;
	}
	p->pos = pos;
	return 0;
}

static uint32_t be32(const uint8_t *b)
{
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static uint64_t be64(const uint8_t *b)
{
	return (uint64_t)be32(b) << 32 | be32(b + 4);
}

static int rd32(struct parser *p, uint32_t *v)
{
	uint8_t b[4];
	if (rd(p, b, 4))
		return -1;
	*v = be32(b);
	return 0;
}

/* Memory for a table of n entries; counted against the index budget. */
static void *table(struct parser *p, uint32_t n, size_t each)
{
	size_t bytes = (size_t)n * each;
	if (p->x->bytes + bytes > MAX_INDEX_BYTES) {
		fail(p, "This video's index is too big for the Wii's memory.");
		return NULL;
	}
	void *m = malloc(bytes ? bytes : 1);
	if (!m) {
		fail(p, "Out of memory reading the video index.");
		return NULL;
	}
	p->x->bytes += bytes;
	return m;
}

/* ---- box walking ------------------------------------------------------------------------ */

struct box {
	int64_t start, payload, end;
	uint32_t type;
};

/* 1 with the next box, 0 at the end of the parent. */
static int next_box(struct parser *p, int64_t limit, struct box *b)
{
	if (p->failed || p->pos + 8 > limit)
		return 0;
	uint8_t h[16];
	b->start = p->pos;
	if (rd(p, h, 8))
		return 0;
	uint64_t size = be32(h);
	b->type = be32(h + 4);
	b->payload = b->start + 8;
	if (size == 1) {
		if (rd(p, h + 8, 8))
			return 0;
		size = be64(h + 8);
		b->payload = b->start + 16;
	} else if (size == 0) {
		size = (uint64_t)(limit - b->start);
	}
	if (size < (uint64_t)(b->payload - b->start) || b->start + (int64_t)size > limit) {
		fail(p, "The video index is damaged.");
		return 0;
	}
	b->end = b->start + (int64_t)size;
	return 1;
}

static void free_trk(struct trk *t)
{
	if (!t)
		return;
	free(t->extra);
	free(t->sizes);
	free(t->stts);
	free(t->ctts);
	free(t->stss);
	free(t->stsc);
	free(t->co);
	free(t);
}

/* Finds a box among the children of buf[start, end), looking inside `wave` too. */
static const uint8_t *find_child(const uint8_t *buf, size_t start, size_t end, uint32_t type, size_t *len, int depth)
{
	while (start + 8 <= end) {
		uint32_t size = be32(buf + start), t = be32(buf + start + 4);
		if (size < 8 || start + size > end)
			break;
		if (t == type) {
			*len = size - 8;
			return buf + start + 8;
		}
		if (t == FOURCC('w', 'a', 'v', 'e') && depth < 2) {
			const uint8_t *r = find_child(buf, start + 8, start + size, type, len, depth + 1);
			if (r)
				return r;
		}
		start += size;
	}
	return NULL;
}

/* AAC's AudioSpecificConfig out of an esds payload (after the 4 version/flag bytes), and whether
 * the object type is AAC. */
static const uint8_t *esds_config(const uint8_t *b, size_t n, size_t *len, int *is_aac)
{
	size_t pos = 0;
	*is_aac = 0;
	while (pos < n) {
		uint8_t tag = b[pos++];
		size_t l = 0;
		for (int k = 0; k < 4 && pos < n; k++) {
			uint8_t c = b[pos++];
			l = (l << 7) | (c & 0x7f);
			if (!(c & 0x80))
				break;
		}
		if (tag == 0x03) { /* ES_Descriptor: ES_ID, flags, then optional fields, then nested */
			if (pos + 3 > n)
				return NULL;
			uint8_t flags = b[pos + 2];
			pos += 3;
			if (flags & 0x80)
				pos += 2;
			if (flags & 0x40 && pos < n)
				pos += 1 + b[pos];
			if (flags & 0x20)
				pos += 2;
		} else if (tag == 0x04) { /* DecoderConfigDescriptor: 13 fixed bytes, then nested */
			if (pos + 13 > n)
				return NULL;
			uint8_t ot = b[pos];
			*is_aac = ot == 0x40 || ot == 0x66 || ot == 0x67 || ot == 0x68;
			pos += 13;
		} else if (tag == 0x05) {
			if (pos + l > n)
				return NULL;
			*len = l;
			return b + pos;
		} else {
			pos += l;
		}
	}
	return NULL;
}

static void read_stsd(struct parser *p, const struct box *b)
{
	struct trk *t = p->t;
	int64_t n = b->end - b->payload;
	if (!t || t->kind < 0 || t->claimed || n < 16 + 8 || n > MAX_STSD)
		return;
	uint8_t *buf = malloc((size_t)n);
	if (!buf) {
		fail(p, "Out of memory reading the video index.");
		return;
	}
	if (rd(p, buf, (size_t)n)) {
		free(buf);
		return;
	}
	uint32_t esize = be32(buf + 8), type = be32(buf + 12);
	if (esize < 16 || (int64_t)esize + 8 > n)
		goto out;
	const uint8_t *e = buf + 8; /* the first sample entry */
	size_t elen = esize;
	enum mp4_codec codec = MP4_CODEC_NONE;
	const uint8_t *extra = NULL;
	size_t extra_n = 0;
	if (t->kind == MP4_VIDEO && (type == FOURCC('a', 'v', 'c', '1') || type == FOURCC('a', 'v', 'c', '3')) &&
		elen >= 86) {
		extra = find_child(e, 86, elen, FOURCC('a', 'v', 'c', 'C'), &extra_n, 0);
		if (extra && extra_n >= 7) {
			codec = MP4_CODEC_H264;
			t->info.width = e[32] << 8 | e[33];
			t->info.height = e[34] << 8 | e[35];
		}
	} else if (t->kind == MP4_AUDIO && type == FOURCC('m', 'p', '4', 'a') && elen >= 36) {
		unsigned version = e[16] << 8 | e[17];
		size_t kids = version == 1 ? 52 : version == 2 ? 72 : 36;
		size_t el = 0;
		const uint8_t *esds = kids < elen ? find_child(e, kids, elen, FOURCC('e', 's', 'd', 's'), &el, 0) : NULL;
		int is_aac = 0;
		if (esds && el > 4) {
			extra = esds_config(esds + 4, el - 4, &extra_n, &is_aac);
			if (extra && is_aac && extra_n >= 2) {
				codec = MP4_CODEC_AAC;
				t->info.channels = e[24] << 8 | e[25];
				t->info.sample_rate = e[32] << 8 | e[33];
			}
		}
	}
	if (codec != MP4_CODEC_NONE && !p->x->trk[t->kind]) {
		t->extra = malloc(extra_n ? extra_n : 1);
		if (t->extra) {
			memcpy(t->extra, extra, extra_n);
			t->info.extradata = t->extra;
			t->info.extradata_size = (int)extra_n;
			t->info.codec = codec;
			t->claimed = 1;
			p->x->trk[t->kind] = t;
		}
	}
out:
	free(buf);
}

/* Reads `count` big-endian u32 values into dst. */
static int read_u32s(struct parser *p, uint32_t *dst, uint32_t count)
{
	uint8_t buf[4096];
	while (count) {
		uint32_t n = count < sizeof buf / 4 ? count : (uint32_t)(sizeof buf / 4);
		if (rd(p, buf, n * 4))
			return -1;
		for (uint32_t i = 0; i < n; i++)
			dst[i] = be32(buf + i * 4);
		dst += n;
		count -= n;
	}
	return 0;
}

/* Header of a full box holding a table: version/flags and the entry count, checked against the
 * box size (each entry `each` bytes after `extra` more header bytes). */
static int table_header(struct parser *p, const struct box *b, size_t extra, size_t each, uint32_t *count)
{
	uint32_t vf;
	int64_t room = b->end - b->payload;
	if (room < 8 || rd32(p, &vf) || rd32(p, count))
		return -1;
	if ((uint64_t)*count * each + extra + 8 > (uint64_t)room) {
		fail(p, "The video index is damaged.");
		return -1;
	}
	return 0;
}

static void read_runs(struct parser *p, const struct box *b, struct run **out, uint32_t *n)
{
	uint32_t count;
	if (table_header(p, b, 0, 8, &count))
		return;
	struct run *r = table(p, count, sizeof *r);
	if (!r)
		return;
	if (read_u32s(p, (uint32_t *)(void *)r, count * 2)) {
		free(r);
		p->x->bytes -= (size_t)count * sizeof *r;
		return;
	}
	*out = r;
	*n = count;
}

static void read_stsz(struct parser *p, const struct box *b)
{
	struct trk *t = p->t;
	uint32_t vf, size, count;
	if (b->end - b->payload < 12 || rd32(p, &vf) || rd32(p, &size) || rd32(p, &count))
		return;
	if (!size && (uint64_t)count * 4 + 12 > (uint64_t)(b->end - b->payload)) {
		fail(p, "The video index is damaged.");
		return;
	}
	t->nsamples = count;
	t->uniform = size;
	if (!size) {
		t->sizes = table(p, count, 4);
		if (t->sizes && read_u32s(p, t->sizes, count)) {
			free(t->sizes);
			t->sizes = NULL;
		}
	}
}

static void read_stsc(struct parser *p, const struct box *b)
{
	uint32_t count;
	if (table_header(p, b, 0, 12, &count))
		return;
	struct stsc_run *r = table(p, count, sizeof *r);
	if (!r)
		return;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t v[3];
		if (read_u32s(p, v, 3)) {
			free(r);
			return;
		}
		r[i].first = v[0];
		r[i].spc = v[1];
	}
	p->t->stsc = r;
	p->t->stsc_n = count;
}

static void read_chunk_offsets(struct parser *p, const struct box *b, int wide)
{
	uint32_t count;
	if (table_header(p, b, 0, wide ? 8 : 4, &count))
		return;
	uint64_t *co = table(p, count, sizeof *co);
	if (!co)
		return;
	uint8_t buf[4096];
	size_t each = wide ? 8 : 4;
	for (uint32_t done = 0; done < count;) {
		uint32_t n = count - done < sizeof buf / each ? count - done : (uint32_t)(sizeof buf / each);
		if (rd(p, buf, n * each)) {
			free(co);
			return;
		}
		for (uint32_t i = 0; i < n; i++)
			co[done + i] = wide ? be64(buf + i * 8) : be32(buf + i * 4);
		done += n;
	}
	p->t->co = co;
	p->t->co_n = count;
}

static void read_elst(struct parser *p, const struct box *b)
{
	uint8_t h[8];
	if (b->end - b->payload < 8 || rd(p, h, 8))
		return;
	unsigned version = h[0];
	uint32_t count = be32(h + 4);
	size_t each = version == 1 ? 20 : 12;
	for (uint32_t i = 0; i < count && i < 8 && p->pos + (int64_t)each <= b->end; i++) {
		uint8_t e[20];
		if (rd(p, e, each))
			return;
		int64_t dur = version == 1 ? (int64_t)be64(e) : be32(e);
		int64_t mt = version == 1 ? (int64_t)be64(e + 8) : (int32_t)be32(e + 4);
		if (mt >= 0) { /* the first real edit sets where the media starts */
			p->t->media_time = mt;
			return;
		}
		p->t->delay_movie += dur; /* an empty edit before it delays the track */
	}
}

static void parse_boxes(struct parser *p, int64_t end, int depth);

static void finish_trak(struct parser *p, struct trk *t)
{
	if (!t->claimed) {
		free_trk(t);
		return;
	}
	if (p->failed || !t->nsamples || !t->stts_n || !t->stsc_n || !t->co_n || !t->timescale ||
		(!t->uniform && !t->sizes)) {
		p->x->trk[t->kind] = NULL; /* an incomplete track is no track */
		free_trk(t);
		return;
	}
	if (p->x->movie_ts)
		t->delay = t->delay_movie * t->timescale / p->x->movie_ts;
	t->enabled = 1;
	t->info.present = 1;
	t->info.timescale = t->timescale;
	t->info.sample_count = t->nsamples;
}

static void parse_boxes(struct parser *p, int64_t end, int depth)
{
	struct box b;
	while (next_box(p, end, &b)) {
		struct trk *t = p->t;
		int tables = t && t->claimed; /* only the kept tracks' tables are read */
		switch (b.type) {
		case FOURCC('m', 'o', 'o', 'v'):
		case FOURCC('m', 'd', 'i', 'a'):
		case FOURCC('m', 'i', 'n', 'f'):
		case FOURCC('s', 't', 'b', 'l'):
		case FOURCC('e', 'd', 't', 's'):
			if (depth < 8 && (b.type == FOURCC('m', 'o', 'o', 'v') || t))
				parse_boxes(p, b.end, depth + 1);
			break;
		case FOURCC('m', 'v', 'h', 'd'):
			if (!t && b.end - b.payload >= 24) {
				uint8_t h[24];
				if (!rd(p, h, 24))
					p->x->movie_ts = h[0] == 1 ? be32(h + 20) : be32(h + 12);
			}
			break;
		case FOURCC('t', 'r', 'a', 'k'):
			if (depth < 8 && !t) {
				p->t = calloc(1, sizeof *p->t);
				if (!p->t) {
					fail(p, "Out of memory reading the video index.");
					break;
				}
				p->t->kind = -1;
				parse_boxes(p, b.end, depth + 1);
				finish_trak(p, p->t);
				p->t = NULL;
			}
			break;
		case FOURCC('m', 'd', 'h', 'd'):
			if (t) {
				uint8_t h[32];
				int64_t room = b.end - b.payload;
				if (room >= 24 && !rd(p, h, 24)) {
					if (h[0] == 1 && room >= 32 && !rd(p, h + 24, 8)) {
						t->timescale = be32(h + 20);
						t->mdhd_duration = be64(h + 24);
					} else if (h[0] == 0) {
						t->timescale = be32(h + 12);
						t->mdhd_duration = be32(h + 16);
					}
				}
			}
			break;
		case FOURCC('h', 'd', 'l', 'r'):
			/* The media handler. (QuickTime files carry a second one under minf, of type "url ".) */
			if (t && t->kind < 0 && b.end - b.payload >= 12) {
				uint8_t h[12];
				if (!rd(p, h, 12)) {
					uint32_t type = be32(h + 8);
					if (type == FOURCC('v', 'i', 'd', 'e'))
						t->kind = MP4_VIDEO;
					else if (type == FOURCC('s', 'o', 'u', 'n'))
						t->kind = MP4_AUDIO;
				}
			}
			break;
		case FOURCC('e', 'l', 's', 't'):
			if (t)
				read_elst(p, &b);
			break;
		case FOURCC('s', 't', 's', 'd'):
			read_stsd(p, &b);
			break;
		case FOURCC('s', 't', 't', 's'):
			if (tables)
				read_runs(p, &b, &t->stts, &t->stts_n);
			break;
		case FOURCC('c', 't', 't', 's'):
			if (tables)
				read_runs(p, &b, &t->ctts, &t->ctts_n);
			break;
		case FOURCC('s', 't', 's', 's'):
			if (tables) {
				uint32_t count;
				if (!table_header(p, &b, 0, 4, &count)) {
					t->stss = table(p, count, 4);
					if (t->stss && read_u32s(p, t->stss, count)) {
						free(t->stss);
						t->stss = NULL;
					} else if (t->stss) {
						t->stss_n = count;
						t->has_stss = 1;
					}
				}
			}
			break;
		case FOURCC('s', 't', 's', 'c'):
			if (tables)
				read_stsc(p, &b);
			break;
		case FOURCC('s', 't', 's', 'z'):
			if (tables)
				read_stsz(p, &b);
			break;
		case FOURCC('s', 't', 'c', 'o'):
			if (tables)
				read_chunk_offsets(p, &b, 0);
			break;
		case FOURCC('c', 'o', '6', '4'):
			if (tables)
				read_chunk_offsets(p, &b, 1);
			break;
		default:
			break;
		}
		if (p->failed || goto_pos(p, b.end))
			return;
	}
}

/* ---- the index -------------------------------------------------------------------------- */

static uint32_t sample_size(const struct trk *t, uint32_t i)
{
	return t->sizes ? t->sizes[i] : t->uniform;
}

static void cursor_set(const struct trk *t, struct cursor *c, uint32_t s)
{
	memset(c, 0, sizeof *c);
	c->i = s;
	if (s >= t->nsamples) {
		c->done = 1;
		return;
	}
	/* chunk: walk the stsc runs */
	uint64_t acc = 0;
	uint32_t r;
	for (r = 0; r < t->stsc_n; r++) {
		uint32_t first = t->stsc[r].first, spc = t->stsc[r].spc;
		uint32_t next = r + 1 < t->stsc_n ? t->stsc[r + 1].first : t->co_n + 1;
		if (!spc || next <= first || !first)
			break;
		uint64_t cnt = (uint64_t)(next - first) * spc;
		if (s < acc + cnt) {
			uint64_t in = s - acc;
			c->chunk = first - 1 + (uint32_t)(in / spc);
			c->j = (uint32_t)(in % spc);
			c->srun = r;
			break;
		}
		acc += cnt;
	}
	if (r >= t->stsc_n || c->chunk >= t->co_n) {
		c->done = 1;
		return;
	}
	c->off = (int64_t)t->co[c->chunk];
	for (uint32_t k = s - c->j; k < s; k++)
		c->off += sample_size(t, k);
	/* timing */
	uint32_t seen = 0;
	c->tleft = 0xFFFFFFFFu;
	for (uint32_t k = 0; k < t->stts_n; k++) {
		if (s < seen + t->stts[k].count) {
			c->trun = k;
			c->tleft = t->stts[k].count - (s - seen);
			c->dts += (int64_t)(s - seen) * t->stts[k].val;
			break;
		}
		seen += t->stts[k].count;
		c->dts += (int64_t)t->stts[k].count * t->stts[k].val;
		c->trun = k;
	}
	seen = 0;
	for (uint32_t k = 0; k < t->ctts_n; k++) {
		if (s < seen + t->ctts[k].count) {
			c->crun = k;
			c->cleft = t->ctts[k].count - (s - seen);
			break;
		}
		seen += t->ctts[k].count;
		c->crun = k + 1;
	}
	uint32_t lo = 0, hi = t->stss_n; /* first stss entry >= s + 1 */
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (t->stss[mid] < s + 1)
			lo = mid + 1;
		else
			hi = mid;
	}
	c->ks = lo;
}

static void cursor_advance(const struct trk *t, struct cursor *c)
{
	c->off += sample_size(t, c->i);
	c->i++;
	c->j++;
	if (c->j >= t->stsc[c->srun].spc) {
		c->j = 0;
		c->chunk++;
		if (c->chunk >= t->co_n) {
			c->done = 1;
			return;
		}
		while (c->srun + 1 < t->stsc_n && c->chunk + 1 >= t->stsc[c->srun + 1].first)
			c->srun++;
		c->off = (int64_t)t->co[c->chunk];
	}
	if (c->i >= t->nsamples) {
		c->done = 1;
		return;
	}
	c->dts += t->stts[c->trun].val;
	if (c->tleft != 0xFFFFFFFFu && --c->tleft == 0) {
		do
			c->trun++;
		while (c->trun < t->stts_n && !t->stts[c->trun].count);
		if (c->trun >= t->stts_n) {
			c->trun = t->stts_n - 1; /* past the table: keep the last delta */
			c->tleft = 0xFFFFFFFFu;
		} else {
			c->tleft = t->stts[c->trun].count;
		}
	}
	if (c->cleft && --c->cleft == 0) {
		do
			c->crun++;
		while (c->crun < t->ctts_n && !t->ctts[c->crun].count);
		c->cleft = c->crun < t->ctts_n ? t->ctts[c->crun].count : 0;
	}
	while (c->ks < t->stss_n && t->stss[c->ks] < c->i + 1)
		c->ks++;
}

static void peek(const struct trk *t, int kind, struct mp4_sample *s)
{
	const struct cursor *c = &t->cur;
	s->kind = kind;
	s->pos = c->off;
	s->size = sample_size(t, c->i);
	s->dts = c->dts;
	s->pts = c->dts + (c->cleft && c->crun < t->ctts_n ? t->ctts[c->crun].val : 0) - t->media_time + t->delay;
	s->key = !t->has_stss || (c->ks < t->stss_n && t->stss[c->ks] == c->i + 1);
}

/* The sample whose decode time is the last at or before `ticks`. */
static uint32_t sample_at(const struct trk *t, int64_t ticks)
{
	int64_t d = 0;
	uint32_t s = 0;
	if (ticks <= 0)
		return 0;
	for (uint32_t k = 0; k < t->stts_n; k++) {
		int64_t span = (int64_t)t->stts[k].count * t->stts[k].val;
		if (ticks < d + span)
			return s + (t->stts[k].val > 0 ? (uint32_t)((ticks - d) / t->stts[k].val) : 0);
		s += t->stts[k].count;
		d += span;
	}
	return t->nsamples ? t->nsamples - 1 : 0;
}

mp4idx *mp4idx_open(const struct mp4_io *io, int64_t moov_off, int64_t moov_size, char *err, size_t errcap)
{
	mp4idx *x = calloc(1, sizeof *x);
	struct parser p = {.io = io, .pos = -1, .x = x, .err = err, .errcap = errcap};
	if (err && errcap)
		err[0] = '\0';
	if (!x || goto_pos(&p, moov_off)) {
		free(x);
		return NULL;
	}
	parse_boxes(&p, moov_off + moov_size, 0);
	free_trk(p.t); /* a trak cut short by an error */
	if (!p.failed && !x->trk[MP4_VIDEO] && !x->trk[MP4_AUDIO])
		fail(&p, "No H.264 video or AAC audio to play.");
	if (p.failed) {
		mp4idx_close(x);
		return NULL;
	}
	for (int k = 0; k < MP4_KINDS; k++)
		if (x->trk[k])
			cursor_set(x->trk[k], &x->trk[k]->cur, 0);
	return x;
}

void mp4idx_close(mp4idx *x)
{
	if (!x)
		return;
	for (int k = 0; k < MP4_KINDS; k++)
		free_trk(x->trk[k]);
	free(x);
}

const struct mp4_track_info *mp4idx_track(const mp4idx *x, enum mp4_kind kind)
{
	return x->trk[kind] && x->trk[kind]->info.present ? &x->trk[kind]->info : NULL;
}

double mp4idx_duration(const mp4idx *x)
{
	double best = 0;
	for (int k = 0; k < MP4_KINDS; k++) {
		const struct trk *t = x->trk[k];
		if (!t)
			continue;
		double d = (double)t->mdhd_duration / t->timescale;
		if (!t->mdhd_duration) {
			int64_t sum = 0;
			for (uint32_t i = 0; i < t->stts_n; i++)
				sum += (int64_t)t->stts[i].count * t->stts[i].val;
			d = (double)sum / t->timescale;
		}
		if (d > best)
			best = d;
	}
	return best;
}

size_t mp4idx_bytes(const mp4idx *x)
{
	return x->bytes;
}

void mp4idx_enable(mp4idx *x, enum mp4_kind kind, int on)
{
	if (x->trk[kind])
		x->trk[kind]->enabled = on;
}

int mp4idx_next(mp4idx *x, struct mp4_sample *out)
{
	struct trk *best = NULL;
	int best_kind = 0;
	for (int k = 0; k < MP4_KINDS; k++) {
		struct trk *t = x->trk[k];
		if (t && t->enabled && !t->cur.done && (!best || t->cur.off < best->cur.off)) {
			best = t;
			best_kind = k;
		}
	}
	if (!best)
		return 0;
	peek(best, best_kind, out);
	cursor_advance(best, &best->cur);
	return 1;
}

void mp4idx_seek(mp4idx *x, double seconds)
{
	if (seconds < 0)
		seconds = 0;
	for (int k = 0; k < MP4_KINDS; k++) {
		struct trk *t = x->trk[k];
		if (!t)
			continue;
		/* The edit list moves the timeline: pts = dts - media_time + delay. */
		uint32_t s = sample_at(t, (int64_t)(seconds * t->timescale) + t->media_time - t->delay);
		if (k == MP4_VIDEO && t->has_stss) {
			uint32_t lo = 0, hi = t->stss_n; /* first stss entry > s + 1 */
			while (lo < hi) {
				uint32_t mid = lo + (hi - lo) / 2;
				if (t->stss[mid] <= s + 1)
					lo = mid + 1;
				else
					hi = mid;
			}
			s = lo ? t->stss[lo - 1] - 1 : 0;
		}
		cursor_set(t, &t->cur, s);
	}
}

/* ---- finding the moov ------------------------------------------------------------------- */

int mp4_find_moov(const struct mp4_io *io, int64_t file_size, int64_t *off, int64_t *size)
{
	int64_t pos = 0;
	for (int n = 0; n < 64 && (file_size < 0 || pos + 8 <= file_size); n++) {
		uint8_t h[16];
		int got = 0;
		if (io->seek(io->ctx, pos))
			return -1;
		while (got < 8) {
			int r = io->read(io->ctx, h + got, (size_t)(8 - got));
			if (r <= 0)
				return -1;
			got += r;
		}
		uint32_t type = be32(h + 4);
		if (n == 0 && type != FOURCC('f', 't', 'y', 'p') && type != FOURCC('m', 'o', 'o', 'v') &&
			type != FOURCC('m', 'd', 'a', 't') && type != FOURCC('w', 'i', 'd', 'e') &&
			type != FOURCC('f', 'r', 'e', 'e') && type != FOURCC('s', 'k', 'i', 'p') &&
			type != FOURCC('p', 'n', 'o', 't'))
			return -1;
		uint64_t len = be32(h);
		if (len == 1) {
			got = 0;
			while (got < 8) {
				int r = io->read(io->ctx, h + 8 + got, (size_t)(8 - got));
				if (r <= 0)
					return -1;
				got += r;
			}
			len = be64(h + 8);
		} else if (len == 0) {
			len = file_size > pos ? (uint64_t)(file_size - pos) : 0;
		}
		if (len < 8)
			return -1;
		if (type == FOURCC('m', 'o', 'o', 'v')) {
			if (file_size >= 0 && pos + (int64_t)len > file_size)
				return -1;
			*off = pos + 8;
			*size = (int64_t)len - 8;
			if (be32(h) == 1) { /* 64-bit size: payload starts 8 bytes later */
				*off += 8;
				*size -= 8;
			}
			return 0;
		}
		pos += (int64_t)len;
	}
	return -1;
}
