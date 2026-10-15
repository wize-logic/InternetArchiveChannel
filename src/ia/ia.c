#include "ia/ia.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "net/url.h"

#define IA_HOST "https://archive.org"

/* ---- small helpers ---------------------------------------------------------------------- */

static void copy_str(char *out, size_t cap, const char *s)
{
	snprintf(out, cap, "%s", s ? s : "");
}

/* String, number, or the first element of an array of strings (IA mixes all three). */
static const char *json_str(const cJSON *obj, const char *key)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (cJSON_IsArray(v))
		v = cJSON_GetArrayItem(v, 0);
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double json_num(const cJSON *obj, const char *key, double def)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (cJSON_IsNumber(v))
		return v->valuedouble;
	if (cJSON_IsString(v) && v->valuestring[0])
		return strtod(v->valuestring, NULL);
	return def;
}

/* "length" is seconds ("187.2") or a clock ("3:07", "1:02:03"). */
static float parse_length(const cJSON *obj)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, "length");
	if (cJSON_IsNumber(v))
		return (float)v->valuedouble;
	if (!cJSON_IsString(v))
		return 0;
	double total = 0;
	const char *p = v->valuestring;
	for (;;) {
		char *end;
		double part = strtod(p, &end);
		if (end == p)
			break;
		total = total * 60 + part; /* each ':' shifts the earlier parts up a unit */
		if (*end != ':')
			break;
		p = end + 1;
	}
	return (float)total;
}

enum ia_media ia_media_from_string(const char *s)
{
	if (!s)
		return IA_MEDIA_OTHER;
	if (strcmp(s, "movies") == 0)
		return IA_MEDIA_MOVIES;
	if (strcmp(s, "audio") == 0 || strcmp(s, "etree") == 0)
		return IA_MEDIA_AUDIO;
	if (strcmp(s, "texts") == 0)
		return IA_MEDIA_TEXTS;
	if (strcmp(s, "image") == 0)
		return IA_MEDIA_IMAGE;
	if (strcmp(s, "collection") == 0)
		return IA_MEDIA_COLLECTION;
	if (strcmp(s, "web") == 0)
		return IA_MEDIA_WEB;
	return IA_MEDIA_OTHER;
}

/* ---- search ----------------------------------------------------------------------------- */

int ia_search_url(char *out, size_t cap, const char *query, enum ia_sort sort, int rows, int page)
{
	/* The lending library and TV News (stream-only) collections, and the collections inside
	 * them, hold nothing the channel can open, so they would only ever list as empty. */
	char q[1024], enc[3072];
	snprintf(q, sizeof q,
		"(%s) AND NOT access-restricted-item:true AND NOT collection:(" IA_CLOSED_COLLECTIONS ")"
		" AND NOT identifier:(" IA_CLOSED_COLLECTIONS ")", query);
	if (url_encode(q, enc, sizeof enc) < 0)
		return -1;
	static const char *const sorts[] = {"downloads+desc", "publicdate+desc", "titleSorter+asc"};
	int n = snprintf(out, cap,
		IA_HOST "/advancedsearch.php?q=%s"
		"&fl%%5B%%5D=identifier&fl%%5B%%5D=title&fl%%5B%%5D=creator&fl%%5B%%5D=date"
		"&fl%%5B%%5D=mediatype&fl%%5B%%5D=downloads&sort%%5B%%5D=%s&rows=%d&page=%d&output=json",
		enc, sorts[sort], rows, page);
	return n > 0 && (size_t)n < cap ? 0 : -1;
}

void ia_user_query(const char *text, const char *mediatype, int titles, char *out, size_t cap)
{
	/* Terms: words or "quoted phrases", all required. Characters Lucene treats specially become
	 * spaces, and AND/OR/NOT typed as words are searched for rather than obeyed. */
	char terms[640];
	size_t n = 0;
	int count = 0;
	const char *p = text;
	for (;;) {
		while (*p && (*p == ' ' || strchr("\\+!(){}[]^~*?:/-", *p)))
			p++;
		if (!*p)
			break;
		char term[256];
		size_t t = 0;
		if (*p == '"') {
			term[t++] = *p++;
			for (; *p && *p != '"' && t < sizeof term - 2; p++)
				term[t++] = strchr("\\+!(){}[]^~*?:/", *p) ? ' ' : *p;
			if (*p == '"')
				p++;
			if (t == 1)
				continue; /* "" */
			term[t++] = '"';
		} else {
			while (*p && *p != ' ' && *p != '"' && !strchr("\\+!(){}[]^~*?:/", *p) && t < sizeof term - 1)
				term[t++] = *p++;
		}
		term[t] = '\0';
		if (strcmp(term, "AND") == 0 || strcmp(term, "OR") == 0 || strcmp(term, "NOT") == 0)
			for (char *c = term; *c; c++)
				*c = (char)(*c + ('a' - 'A'));
		int w = snprintf(terms + n, sizeof terms - n, "%s%s", count ? " AND " : "", term);
		if (w < 0 || (size_t)w >= sizeof terms - n)
			break;
		n += (size_t)w;
		count++;
	}
	terms[n] = '\0';
	if (!count) {
		snprintf(out, cap, "%s", "");
		return;
	}
	/* Only what the channel opens: an "audio" search takes in live music (etree) too. Users'
	 * favourites lists (fav-*) are collections that match everything. */
	const char *types = !mediatype || !*mediatype ? "(movies OR audio OR etree OR texts OR image OR collection)"
		: strcmp(mediatype, "audio") == 0 ? "(audio OR etree)" : mediatype;
	if (titles)
		snprintf(out, cap, "(title:(%s) OR creator:(%s)) AND mediatype:%s AND NOT identifier:fav-*", terms, terms, types);
	else
		snprintf(out, cap, "(%s) AND mediatype:%s AND NOT identifier:fav-*", terms, types);
}

int ia_parse_search(const char *json, struct ia_search *out)
{
	memset(out, 0, sizeof *out);
	cJSON *root = cJSON_Parse(json);
	if (!root)
		return -1;
	const cJSON *resp = cJSON_GetObjectItemCaseSensitive(root, "response");
	const cJSON *docs = cJSON_GetObjectItemCaseSensitive(resp, "docs");
	if (!cJSON_IsArray(docs)) {
		cJSON_Delete(root);
		return -1;
	}
	out->total = (int)json_num(resp, "numFound", 0);
	int n = cJSON_GetArraySize(docs);
	out->docs = n ? calloc((size_t)n, sizeof *out->docs) : NULL;
	if (n && !out->docs) {
		cJSON_Delete(root);
		return -1;
	}
	const cJSON *d;
	cJSON_ArrayForEach(d, docs)
	{
		const char *id = json_str(d, "identifier");
		if (!id)
			continue;
		struct ia_doc *doc = &out->docs[out->count++];
		copy_str(doc->id, sizeof doc->id, id);
		copy_str(doc->title, sizeof doc->title, json_str(d, "title") ? json_str(d, "title") : id);
		copy_str(doc->creator, sizeof doc->creator, json_str(d, "creator"));
		copy_str(doc->mediatype, sizeof doc->mediatype, json_str(d, "mediatype"));
		const char *date = json_str(d, "date");
		if (date && strlen(date) >= 4 && isdigit((unsigned char)date[0]))
			snprintf(doc->year, sizeof doc->year, "%.4s", date);
		doc->downloads = (int)json_num(d, "downloads", 0);
	}
	cJSON_Delete(root);
	return 0;
}

void ia_search_free(struct ia_search *s)
{
	free(s->docs);
	memset(s, 0, sizeof *s);
}

int ia_thumb_url(char *out, size_t cap, const char *id)
{
	char enc[IA_ID_MAX * 3];
	if (url_encode(id, enc, sizeof enc) < 0)
		return -1;
	int n = snprintf(out, cap, IA_HOST "/services/img/%s", enc);
	return n > 0 && (size_t)n < cap ? 0 : -1;
}

/* ---- items ------------------------------------------------------------------------------ */

int ia_metadata_url(char *out, size_t cap, const char *id)
{
	char enc[IA_ID_MAX * 3];
	if (url_encode(id, enc, sizeof enc) < 0)
		return -1;
	int n = snprintf(out, cap, IA_HOST "/metadata/%s", enc);
	return n > 0 && (size_t)n < cap ? 0 : -1;
}

int ia_file_url(char *out, size_t cap, const char *id, const char *file_name)
{
	char eid[IA_ID_MAX * 3], ename[1024];
	if (url_encode(id, eid, sizeof eid) < 0)
		return -1;
	/* Encode each path segment, keeping the slashes of files in subdirectories. */
	size_t o = 0;
	for (const char *p = file_name; *p;) {
		const char *slash = strchr(p, '/');
		size_t len = slash ? (size_t)(slash - p) : strlen(p);
		char seg[512], eseg[1024];
		if (len >= sizeof seg)
			return -1;
		memcpy(seg, p, len);
		seg[len] = '\0';
		int e = url_encode(seg, eseg, sizeof eseg);
		if (e < 0 || o + (size_t)e + 2 > sizeof ename)
			return -1;
		memcpy(ename + o, eseg, (size_t)e);
		o += (size_t)e;
		if (slash)
			ename[o++] = '/';
		p = slash ? slash + 1 : p + len;
	}
	ename[o] = '\0';
	int n = snprintf(out, cap, IA_HOST "/download/%s/%s", eid, ename);
	return n > 0 && (size_t)n < cap ? 0 : -1;
}

void ia_strip_html(char *s)
{
	static const struct {
		const char *ent;
		const char *rep;
	} ents[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"},
		{"&apos;", "'"}, {"&nbsp;", " "}, {"&mdash;", "-"}, {"&ndash;", "-"}, {"&hellip;", "..."}};
	char *w = s;
	int last_nl = 1, last_space = 1;
	for (char *r = s; *r;) {
		if (*r == '<') {
			char *end = strchr(r, '>');
			if (!end)
				break;
			/* Block-level tags become line breaks. */
			if ((strncasecmp(r, "<br", 3) == 0 || strncasecmp(r, "<p", 2) == 0 ||
				    strncasecmp(r, "</p", 3) == 0 || strncasecmp(r, "<div", 4) == 0 ||
				    strncasecmp(r, "<li", 3) == 0) &&
				!last_nl) {
				*w++ = '\n';
				last_nl = last_space = 1;
			}
			r = end + 1;
			continue;
		}
		if (*r == '&') {
			int matched = 0;
			for (size_t i = 0; i < sizeof ents / sizeof ents[0]; i++) {
				size_t el = strlen(ents[i].ent);
				if (strncmp(r, ents[i].ent, el) == 0) {
					size_t rl = strlen(ents[i].rep);
					memcpy(w, ents[i].rep, rl); /* replacement is never longer */
					w += rl;
					r += el;
					matched = 1;
					last_nl = last_space = 0;
					break;
				}
			}
			if (matched)
				continue;
		}
		char c = *r++;
		if (c == '\r')
			continue;
		if (c == '\n' || c == '\t' || c == ' ') {
			if (c == '\n' && !last_nl) {
				*w++ = '\n';
				last_nl = last_space = 1;
			} else if (!last_space) {
				*w++ = ' ';
				last_space = 1;
			}
			continue;
		}
		*w++ = c;
		last_nl = last_space = 0;
	}
	while (w > s && (w[-1] == ' ' || w[-1] == '\n'))
		w--;
	*w = '\0';
}

int ia_parse_item(const char *json, struct ia_item *out)
{
	memset(out, 0, sizeof *out);
	cJSON *root = cJSON_Parse(json);
	if (!root)
		return -1;
	const cJSON *md = cJSON_GetObjectItemCaseSensitive(root, "metadata");
	if (!cJSON_IsObject(md)) {
		cJSON_Delete(root);
		return -1; /* {} for unknown identifiers */
	}
	copy_str(out->id, sizeof out->id, json_str(md, "identifier"));
	copy_str(out->title, sizeof out->title, json_str(md, "title") ? json_str(md, "title") : out->id);
	copy_str(out->creator, sizeof out->creator, json_str(md, "creator"));
	copy_str(out->date, sizeof out->date, json_str(md, "date"));
	out->media = ia_media_from_string(json_str(md, "mediatype"));
	out->imagecount = (int)json_num(md, "imagecount", 0);
	const char *restricted = json_str(md, "access-restricted-item");
	out->restricted = restricted && strcasecmp(restricted, "true") == 0;
	const char *desc = json_str(md, "description");
	if (desc) {
		out->description = strdup(desc);
		if (out->description)
			ia_strip_html(out->description);
	}
	copy_str(out->server, sizeof out->server, json_str(root, "d1"));
	copy_str(out->dir, sizeof out->dir, json_str(root, "dir"));

	const cJSON *files = cJSON_GetObjectItemCaseSensitive(root, "files");
	int n = cJSON_IsArray(files) ? cJSON_GetArraySize(files) : 0;
	out->files = n ? calloc((size_t)n, sizeof *out->files) : NULL;
	const cJSON *f;
	if (out->files) {
		cJSON_ArrayForEach(f, files)
		{
			const char *name = json_str(f, "name");
			if (!name)
				continue;
			struct ia_file *x = &out->files[out->file_count++];
			copy_str(x->name, sizeof x->name, name);
			copy_str(x->format, sizeof x->format, json_str(f, "format"));
			copy_str(x->title, sizeof x->title, json_str(f, "title"));
			copy_str(x->track, sizeof x->track, json_str(f, "track"));
			x->size = (int64_t)json_num(f, "size", 0);
			x->length = parse_length(f);
			x->width = (int)json_num(f, "width", 0);
			x->height = (int)json_num(f, "height", 0);
			const char *src = json_str(f, "source");
			x->original = src && strcmp(src, "original") == 0;
		}
	}
	cJSON_Delete(root);
	return 0;
}

void ia_item_free(struct ia_item *it)
{
	free(it->description);
	free(it->files);
	memset(it, 0, sizeof *it);
}

/* ---- file selection --------------------------------------------------------------------- */

/* Rank of a file as a Wii-playable video (0: not one). */
static int video_rank(const struct ia_file *f)
{
	if (f->length > 0 && f->length < 1.0f)
		return 0;
	if (strcmp(f->format, "512Kb MPEG4") == 0)
		return 4;
	if (strcmp(f->format, "Ogg Video") == 0)
		return 3;
	if ((strcmp(f->format, "h.264") == 0 || strcmp(f->format, "h.264 IA") == 0 || strcmp(f->format, "MPEG4") == 0) &&
		(f->height == 0 || f->height <= 480))
		return f->height && f->height <= 360 ? 2 : 1;
	return 0;
}

int ia_file_kbps(const struct ia_file *f)
{
	return f->size > 0 && f->length >= 1.0f ? (int)((double)f->size * 8 / 1000 / (double)f->length) : 0;
}

int ia_pick_video(const struct ia_item *it, int *fast_hint)
{
	/* Preference: the 320x240 "512Kb" derivative,
	 * then Ogg Theora, then H.264 up to 480 lines. Very short "videos" are stills made from
	 * image uploads. Among equally ranked files the lower bitrate wins: the Wii's link tops out
	 * around 350-400 KB/s. (How many samples FFmpeg can index is checked by the player on open.) */
	int best = -1, best_rank = 0;
	for (int i = 0; i < it->file_count; i++) {
		const struct ia_file *f = &it->files[i];
		int rank = video_rank(f);
		int kbps = ia_file_kbps(f);
		if (rank > best_rank || (rank && rank == best_rank && kbps && ia_file_kbps(&it->files[best]) > kbps)) {
			best_rank = rank;
			best = i;
		}
	}
	if (fast_hint)
		*fast_hint = best >= 0 && best_rank <= 2 && it->files[best].width > 320;
	return best;
}

int ia_list_audio(const struct ia_item *it, int *out, int max)
{
	static const char *const prefs[] = {"VBR MP3", "Ogg Vorbis", "64Kbps MP3", "128Kbps MP3", "MP3"};
	/* Use the most preferred format the item actually has, so every track comes out alike. */
	for (size_t p = 0; p < sizeof prefs / sizeof prefs[0]; p++) {
		int n = 0;
		for (int i = 0; i < it->file_count && n < max; i++)
			if (strcmp(it->files[i].format, prefs[p]) == 0 &&
				!(it->files[i].length > 0 && it->files[i].length < 1.0f))
				out[n++] = i;
		if (n)
			return n;
	}
	return 0;
}

/* ---- books and images ------------------------------------------------------------------- */

int ia_book_page_url(char *out, size_t cap, const char *id, int page, int width)
{
	char enc[IA_ID_MAX * 3];
	if (url_encode(id, enc, sizeof enc) < 0)
		return -1;
	int n = snprintf(out, cap, IA_HOST "/download/%s/page/n%d_w%d.jpg", enc, page, width);
	return n > 0 && (size_t)n < cap ? 0 : -1;
}

int ia_iiif_url(char *out, size_t cap, const char *id, const char *file_name, int max_w, int max_h)
{
	/* The identifier "<id>/<file>" is one path segment, so the slash is encoded too. */
	char full[512], enc[1536];
	snprintf(full, sizeof full, "%s/%s", id, file_name);
	if (url_encode(full, enc, sizeof enc) < 0)
		return -1;
	int n = max_w > 0 ? snprintf(out, cap, "https://iiif.archive.org/image/iiif/3/%s/full/!%d,%d/0/default.jpg", enc,
						max_w, max_h)
			  : snprintf(out, cap, "https://iiif.archive.org/image/iiif/3/%s/full/max/0/default.jpg", enc);
	return n > 0 && (size_t)n < cap ? 0 : -1;
}

int ia_list_images(const struct ia_item *it, int *out, int max)
{
	static const char *const formats[] = {"JPEG", "PNG", "GIF", "TIFF", "JPEG 2000", "Animated GIF"};
	int n = 0;
	for (int i = 0; i < it->file_count && n < max; i++) {
		if (!it->files[i].original)
			continue;
		for (size_t f = 0; f < sizeof formats / sizeof formats[0]; f++)
			if (strcmp(it->files[i].format, formats[f]) == 0) {
				out[n++] = i;
				break;
			}
	}
	return n;
}

static int ends_with(const char *s, const char *suffix)
{
	size_t l = strlen(s), n = strlen(suffix);
	return l >= n && strcmp(s + l - n, suffix) == 0;
}

int ia_find_text(const struct ia_item *it)
{
	/* OCR text first, then a plain-text edition (Project Gutenberg: "-0.txt" is UTF-8, "-8.txt"
	 * Latin-1, ".txt" ASCII; any of them reads fine after ia_reflow_text). */
	int best = -1, best_rank = 0;
	for (int i = 0; i < it->file_count; i++) {
		const struct ia_file *f = &it->files[i];
		int text = strcmp(f->format, "Text") == 0 && ends_with(f->name, ".txt");
		int rank = ends_with(f->name, "_djvu.txt")    ? 5
			: text && ends_with(f->name, "-0.txt") ? 4
			: text && !ends_with(f->name, "-8.txt") ? 3
			: text                                  ? 2
								: 0;
		if (rank > best_rank) {
			best = i;
			best_rank = rank;
		}
	}
	return best;
}

int ia_has_book_pages(const struct ia_item *it)
{
	if (it->imagecount > 0)
		return 1;
	for (int i = 0; i < it->file_count; i++) {
		const char *n = it->files[i].name;
		if (ends_with(n, "_jp2.zip") || ends_with(n, "_jp2.tar") || ends_with(n, "_tif.zip"))
			return 1;
	}
	return 0;
}

int ia_find_page_count_file(const struct ia_item *it)
{
	int scandata = -1;
	for (int i = 0; i < it->file_count; i++) {
		if (ends_with(it->files[i].name, "_page_numbers.json"))
			return i;
		if (ends_with(it->files[i].name, "_scandata.xml"))
			scandata = i;
	}
	return scandata;
}

int ia_parse_page_count(const char *body)
{
	while (*body == ' ' || *body == '\n' || *body == '\r' || *body == '\t')
		body++;
	if (*body == '{') {
		cJSON *root = cJSON_Parse(body);
		const cJSON *pages = root ? cJSON_GetObjectItemCaseSensitive(root, "pages") : NULL;
		int n = cJSON_IsArray(pages) ? cJSON_GetArraySize(pages) : -1;
		cJSON_Delete(root);
		return n > 0 ? n : -1;
	}
	/* scandata.xml: one <page leafNum="..."> per leaf; leaves left out of the access formats
	 * (colour cards, blanks) aren't in the reader. */
	int n = 0;
	for (const char *p = strstr(body, "<page "); p; p = strstr(p + 6, "<page ")) {
		const char *end = strstr(p, "</page>");
		const char *skip = strstr(p, "<addToAccessFormats>false");
		if (!(skip && (!end || skip < end)))
			n++;
	}
	return n > 0 ? n : -1;
}

/* Bytes of the UTF-8 sequence starting at s, or 0 if it isn't valid UTF-8. */
static int utf8_len(const unsigned char *s, const unsigned char *end)
{
	int n = s[0] < 0x80 ? 1 : (s[0] & 0xE0) == 0xC0 ? 2 : (s[0] & 0xF0) == 0xE0 ? 3 : (s[0] & 0xF8) == 0xF0 ? 4 : 0;
	if (n == 0 || s + n > end || (n == 2 && s[0] < 0xC2))
		return 0;
	for (int i = 1; i < n; i++)
		if ((s[i] & 0xC0) != 0x80)
			return 0;
	return n;
}

char *ia_reflow_text(const char *text, size_t len)
{
	const unsigned char *in = (const unsigned char *)text, *end = in + len;
	/* Latin-1 unless the whole text is valid UTF-8 (a UTF-8 BOM is dropped). */
	int utf8 = 1;
	for (const unsigned char *p = in; p < end && utf8;) {
		int n = utf8_len(p, end);
		utf8 = n > 0;
		p += n;
	}
	if (utf8 && len >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF)
		in += 3;

	/* Typical line length: the length that 70% of non-empty lines stay within. In wrapped prose
	 * most lines are close to the wrap width, so a much shorter line ends a paragraph (or is a
	 * heading, or a line of verse) and keeps its break. */
	int hist[201] = {0}, lines = 0;
	for (const unsigned char *p = in; p < end;) {
		const unsigned char *nl = memchr(p, '\n', (size_t)(end - p));
		const unsigned char *e = nl ? nl : end;
		int l = 0;
		for (const unsigned char *q = p; q < e; q++)
			l += (*q & 0xC0) != 0x80 && *q > ' ';
		if (l > 0) {
			hist[l > 200 ? 200 : l]++;
			lines++;
		}
		p = nl ? nl + 1 : end;
	}
	int wrap = 0, seen = 0;
	while (wrap < 200 && (seen += hist[wrap]) * 10 < lines * 7)
		wrap++;

	char *out = malloc(len * 2 + 2), *o = out;
	if (!out)
		return NULL;
	int para = 0; /* inside a paragraph: the next line joins it */
	const unsigned char *p = in;
	while (p < end) {
		/* One line, trimmed. */
		const unsigned char *nl = memchr(p, '\n', (size_t)(end - p));
		const unsigned char *e = nl ? nl : end, *s = p;
		p = nl ? nl + 1 : end;
		int form_feed = 0;
		while (s < e && (*s <= ' ' || *s == 0x7F))
			form_feed |= *s++ == '\f';
		while (e > s && (e[-1] <= ' ' || e[-1] == 0x7F))
			e--;
		if ((s == e || form_feed) && o > out) {
			/* A blank line (or a page break) separates paragraphs. */
			if (o[-1] != '\n')
				*o++ = '\n';
			if (o - out < 2 || o[-2] != '\n')
				*o++ = '\n';
			para = 0;
		}
		if (s == e)
			continue;
		if (para) {
			/* "for-" + "tune" -> "fortune"; anything else joins with a space. */
			if (o - out >= 2 && o[-1] == '-' && isalpha((unsigned char)o[-2]) && *s >= 'a' && *s <= 'z')
				o--;
			else
				*o++ = ' ';
		}
		int count = 0, space = 0;
		while (s < e) {
			if (*s <= ' ' || *s == 0x7F) {
				space = 1;
				s++;
				continue;
			}
			if (space && o > out && o[-1] != ' ' && o[-1] != '\n')
				*o++ = ' ';
			space = 0;
			if (*s < 0x80) {
				*o++ = (char)*s++;
			} else if (utf8) {
				int n = utf8_len(s, e);
				memcpy(o, s, (size_t)n);
				o += n, s += n;
			} else { /* Latin-1 -> UTF-8 */
				*o++ = (char)(0xC0 | *s >> 6);
				*o++ = (char)(0x80 | (*s & 0x3F));
				s++;
			}
			count++;
		}
		/* A line much shorter than the wrap width keeps its break: the end of a paragraph, a
		 * heading, a line of verse. */
		para = count * 10 >= wrap * 6;
		if (!para)
			*o++ = '\n';
	}
	while (o > out && o[-1] == '\n')
		o--;
	*o = '\0';
	return out;
}

/* ---- Wayback Machine -------------------------------------------------------------------- */

int ia_wayback_available_url(char *out, size_t cap, const char *site, const char *timestamp)
{
	char enc[1024];
	if (url_encode(site, enc, sizeof enc) < 0)
		return -1;
	int n = snprintf(out, cap, IA_HOST "/wayback/available?url=%s&timestamp=%s", enc, timestamp);
	return n > 0 && (size_t)n < cap ? 0 : -1;
}

int ia_wayback_cdx_url(char *out, size_t cap, const char *site, int year)
{
	char enc[1024];
	if (url_encode(site, enc, sizeof enc) < 0)
		return -1;
	int n = snprintf(out, cap,
		"https://web.archive.org/cdx/search/cdx?url=%s&from=%d&to=%d&output=json&fl=timestamp"
		"&filter=statuscode:200&collapse=timestamp:8&limit=400",
		enc, year, year);
	return n > 0 && (size_t)n < cap ? 0 : -1;
}

/* Keeps 14-digit timestamps; anything else (the header row, blanks) is skipped. */
static int take_ts(const char *s, size_t len, char (*out)[IA_TS_LEN], int n, int max)
{
	if (n >= max || len < 8 || len > 14)
		return n;
	for (size_t i = 0; i < len; i++)
		if (s[i] < '0' || s[i] > '9')
			return n;
	memcpy(out[n], s, len);
	out[n][len] = '\0';
	return n + 1;
}

int ia_parse_cdx(const char *body, char (*out)[IA_TS_LEN], int max)
{
	int n = 0;
	const char *p0 = body;
	while (*p0 == ' ' || *p0 == '\n' || *p0 == '\r' || *p0 == '\t')
		p0++;
	cJSON *root = (*p0 == '[' || *p0 == '{') ? cJSON_Parse(p0) : NULL;
	if (*p0 == '[' || *p0 == '{') {
		if (!root)
			return -1;
		if (!cJSON_IsArray(root)) {
			cJSON_Delete(root);
			return -1;
		}
		const cJSON *row;
		cJSON_ArrayForEach(row, root)
		{
			const cJSON *first = cJSON_IsArray(row) ? cJSON_GetArrayItem(row, 0) : row;
			if (cJSON_IsString(first))
				n = take_ts(first->valuestring, strlen(first->valuestring), out, n, max);
		}
		cJSON_Delete(root);
		return n;
	}
	/* Plain output: one capture per line, timestamp first. */
	for (const char *p = body; *p;) {
		const char *e = p;
		while (*e && *e != '\n' && *e != ' ')
			e++;
		n = take_ts(p, (size_t)(e - p), out, n, max);
		while (*e && *e != '\n')
			e++;
		p = *e ? e + 1 : e;
	}
	return n;
}

int ia_parse_wayback_available(const char *json, char *ts_out, size_t cap)
{
	cJSON *root = cJSON_Parse(json);
	if (!root)
		return -1;
	const cJSON *snaps = cJSON_GetObjectItemCaseSensitive(root, "archived_snapshots");
	const cJSON *closest = cJSON_GetObjectItemCaseSensitive(snaps, "closest");
	const char *ts = json_str(closest, "timestamp");
	const cJSON *avail = cJSON_GetObjectItemCaseSensitive(closest, "available");
	int ok = ts && (!avail || cJSON_IsTrue(avail));
	if (ok)
		snprintf(ts_out, cap, "%s", ts);
	cJSON_Delete(root);
	return ok ? 0 : -1;
}
