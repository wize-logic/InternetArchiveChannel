/* archive.org API: URL builders, JSON parsing and file selection (no I/O; host-tested). */
#ifndef IAC_IA_H
#define IAC_IA_H

#include <stddef.h>
#include <stdint.h>

#define IA_ID_MAX 128

/* ---- search ---- */

struct ia_doc {
	char id[IA_ID_MAX];
	char title[192];
	char creator[96];
	char year[8];
	char mediatype[16];
	int downloads;
};

struct ia_search {
	struct ia_doc *docs;
	int count;
	int total; /* numFound */
};

enum ia_sort { IA_SORT_POPULAR = 0, IA_SORT_NEWEST, IA_SORT_TITLE };

/* Builds an advancedsearch URL. `query` is Lucene syntax (encoded here). Items that can only
 * be borrowed (lending library) are excluded, since the channel can't read them. */
int ia_search_url(char *out, size_t cap, const char *query, enum ia_sort sort, int rows, int page);
/* Turns free text from the keyboard into a query (plain words are ANDed, quotes kept). */
void ia_user_query(const char *text, const char *mediatype, char *out, size_t cap);
int ia_parse_search(const char *json, struct ia_search *out);
void ia_search_free(struct ia_search *s);

int ia_thumb_url(char *out, size_t cap, const char *id);

/* ---- items ---- */

enum ia_media { IA_MEDIA_OTHER = 0, IA_MEDIA_MOVIES, IA_MEDIA_AUDIO, IA_MEDIA_TEXTS, IA_MEDIA_IMAGE,
	IA_MEDIA_COLLECTION, IA_MEDIA_WEB };

struct ia_file {
	char name[192];
	char format[40];
	int64_t size;
	float length; /* seconds, 0 if unknown */
	int width, height;
	int original; /* source == "original" */
	char title[128]; /* track title for audio, if given */
	char track[8];
};

struct ia_item {
	char id[IA_ID_MAX];
	char title[256];
	char creator[160];
	char date[32];
	enum ia_media media;
	char *description; /* plain text (HTML stripped), may be NULL */
	int restricted;    /* lending / access-restricted: not viewable */
	int imagecount;    /* books: pages */
	char server[64];   /* d1 storage host, for direct downloads */
	char dir[160];
	struct ia_file *files;
	int file_count;
};

int ia_metadata_url(char *out, size_t cap, const char *id);
int ia_parse_item(const char *json, struct ia_item *out);
void ia_item_free(struct ia_item *it);

/* Download URL for a file (archive.org/download/..., which redirects to a storage node). */
int ia_file_url(char *out, size_t cap, const char *id, const char *file_name);

/* Index of the best Wii-playable video file, or -1. *fast_hint is set for H.264 above 320 px
 * wide, which needs the skip-loop-filter decode path. */
int ia_pick_video(const struct ia_item *it, int *fast_hint);
/* Indices of playable audio tracks in item order (one format per track, VBR MP3 preferred).
 * Returns the count (<= max). */
int ia_list_audio(const struct ia_item *it, int *out, int max);

/* Book page n (0-based) as a JPEG about `width` pixels wide (BookReader, via /download/). */
int ia_book_page_url(char *out, size_t cap, const char *id, int page, int width);
/* An image file scaled server-side by IIIF to fit max_w x max_h (also converts TIFF/JPEG 2000).
 * The server refuses (400) to fit an image that is smaller than the box, as that would upscale
 * it; max_w <= 0 asks for the image at its own size instead. */
int ia_iiif_url(char *out, size_t cap, const char *id, const char *file_name, int max_w, int max_h);
/* Indices of original image files (JPEG, PNG, GIF, TIFF, JPEG 2000) in item order. */
int ia_list_images(const struct ia_item *it, int *out, int max);
/* Index of the book's text: the OCR text (<x>_djvu.txt), else a plain-text edition (Project
 * Gutenberg's .txt), or -1. */
int ia_find_text(const struct ia_item *it);
/* The item has BookReader page images (ia_book_page_url works): an imagecount, or a processed
 * JP2/TIFF archive (PDF uploads have the archive but often no imagecount). */
int ia_has_book_pages(const struct ia_item *it);
/* Index of the file that tells how many pages the reader has (<x>_page_numbers.json, else
 * <x>_scandata.xml), or -1. */
int ia_find_page_count_file(const struct ia_item *it);
/* Page count from a page_numbers.json or a scandata.xml body, or -1. */
int ia_parse_page_count(const char *body);
/* Book text for reading: Latin-1 converted to UTF-8 (unless it's already valid UTF-8), and the
 * hard-wrapped lines of OCR and plain-text editions joined into paragraphs (words hyphenated
 * across lines rejoined, runs of spaces collapsed). Blank lines stay as paragraph breaks, and
 * short lines (headings, verse, paragraph ends) keep their line break. Returns a malloc'd string, or NULL. */
char *ia_reflow_text(const char *text, size_t len);

/* Wayback availability lookup for `site` near `timestamp` (YYYY[MMDD...]). */
int ia_wayback_available_url(char *out, size_t cap, const char *site, const char *timestamp);
/* Parses the availability answer: 0 with the closest snapshot's timestamp, -1 if none. */
int ia_parse_wayback_available(const char *json, char *ts_out, size_t cap);

/* CDX listing of `site`'s captures during `year`, at most one per day (successful pages only). */
#define IA_TS_LEN 15
int ia_wayback_cdx_url(char *out, size_t cap, const char *site, int year);
/* Capture timestamps from a CDX answer (JSON rows, header skipped, or plain lines), in order.
 * Returns the count (<= max), or -1 if it isn't a CDX answer. */
int ia_parse_cdx(const char *body, char (*out)[IA_TS_LEN], int max);

/* Collapses HTML into plain text in place: tags dropped, <br>/<p> to newlines, entities decoded. */
void ia_strip_html(char *s);
enum ia_media ia_media_from_string(const char *s);

#endif
