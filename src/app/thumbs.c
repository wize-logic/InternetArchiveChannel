/* Item thumbnails: fetched on demand from archive.org/services/img/<id>, decoded on the UI
 * thread (a couple per frame), kept as textures in a small LRU cache. */
#include <stdio.h>
#include <string.h>

#include "app/app.h"
#include "ia/ia.h"
#include "net/http.h"

#define MAX_THUMBS 96
#define MAX_LOADING 6
#define DECODES_PER_FRAME 2

enum { T_EMPTY = 0, T_LOADING, T_READY, T_FAILED };

struct thumb {
	char id[IA_ID_MAX];
	int state;
	http_req *req;
	struct image img;
	uint32_t last_used;
};

static struct thumb g_thumbs[MAX_THUMBS];
static uint32_t g_frame;

static int loading_count(void)
{
	int n = 0;
	for (int i = 0; i < MAX_THUMBS; i++)
		n += g_thumbs[i].state == T_LOADING;
	return n;
}

static void evict(struct thumb *t)
{
	if (t->req)
		http_release(t->req);
	image_free(&t->img);
	memset(t, 0, sizeof *t);
}

const struct image *thumb_get(const char *id)
{
	struct thumb *free_slot = NULL, *oldest = NULL;
	for (int i = 0; i < MAX_THUMBS; i++) {
		struct thumb *t = &g_thumbs[i];
		if (t->state != T_EMPTY && strcmp(t->id, id) == 0) {
			t->last_used = g_frame;
			return t->state == T_READY ? &t->img : NULL;
		}
		if (t->state == T_EMPTY && !free_slot)
			free_slot = t;
		if (t->state != T_LOADING && t->state != T_EMPTY && t->last_used != g_frame &&
			(!oldest || t->last_used < oldest->last_used))
			oldest = t;
	}
	/* Not cached: start a fetch if the network is up and there's room. */
	if (net_state() != NET_READY || loading_count() >= MAX_LOADING)
		return NULL;
	struct thumb *t = free_slot ? free_slot : oldest;
	if (!t)
		return NULL;
	evict(t);
	char url[512];
	if (ia_thumb_url(url, sizeof url, id) != 0)
		return NULL;
	struct http_opts o;
	http_opts_default(&o);
	o.max_body = 512 * 1024;
	o.timeout_ms = 45000;
	snprintf(t->id, sizeof t->id, "%s", id);
	t->req = http_get(url, &o);
	t->state = T_LOADING;
	t->last_used = g_frame;
	return NULL;
}

void thumbs_frame(void)
{
	g_frame++;
	int decoded = 0;
	for (int i = 0; i < MAX_THUMBS && decoded < DECODES_PER_FRAME; i++) {
		struct thumb *t = &g_thumbs[i];
		if (t->state != T_LOADING || http_state(t->req) == HTTP_PENDING)
			continue;
		const struct http_result *r = http_result(t->req);
		int ok = http_state(t->req) == HTTP_DONE && r->status == 200 &&
			image_load_any(&t->img, r->body, r->body_len, 320, 240) == 0;
		if (!ok)
			WL_LOG_D("THUMB", "%s: %s (status %d, %s)", t->id, ok ? "ok" : "failed", r->status,
				r->content_type);
		http_release(t->req);
		t->req = NULL;
		t->state = ok ? T_READY : T_FAILED;
		decoded++;
	}
}

void thumbs_clear(void)
{
	for (int i = 0; i < MAX_THUMBS; i++)
		evict(&g_thumbs[i]);
}
