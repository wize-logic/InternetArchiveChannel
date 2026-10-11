/* Single-producer/single-consumer ring of stereo PCM16 frames between the decoder thread and the
 * audio callback (which must never block). 32-bit indices: Broadway has no 64-bit atomics. */
#ifndef IAC_PCM_RING_H
#define IAC_PCM_RING_H

#include <stdint.h>
#include <string.h>

struct pcm_ring {
	int16_t *buf;      /* capacity * 2 samples */
	uint32_t capacity; /* frames, power of two */
	uint32_t head;     /* written frames (producer) */
	uint32_t tail;     /* read frames (consumer) */
};

static inline uint32_t pcm_ring_count(const struct pcm_ring *r)
{
	return __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) - __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
}

static inline uint32_t pcm_ring_space(const struct pcm_ring *r)
{
	return r->capacity - pcm_ring_count(r);
}

/* Producer: copies up to `frames`; returns how many fit. */
static inline uint32_t pcm_ring_write(struct pcm_ring *r, const int16_t *src, uint32_t frames)
{
	uint32_t space = pcm_ring_space(r);
	if (frames > space)
		frames = space;
	uint32_t head = r->head;
	for (uint32_t i = 0; i < frames;) {
		uint32_t at = (head + i) & (r->capacity - 1);
		uint32_t run = r->capacity - at < frames - i ? r->capacity - at : frames - i;
		memcpy(r->buf + at * 2, src + i * 2, run * 4);
		i += run;
	}
	__atomic_store_n(&r->head, head + frames, __ATOMIC_RELEASE);
	return frames;
}

/* Consumer: copies up to `frames`; returns how many were available. */
static inline uint32_t pcm_ring_read(struct pcm_ring *r, int16_t *dst, uint32_t frames)
{
	uint32_t count = pcm_ring_count(r);
	if (frames > count)
		frames = count;
	uint32_t tail = r->tail;
	for (uint32_t i = 0; i < frames;) {
		uint32_t at = (tail + i) & (r->capacity - 1);
		uint32_t run = r->capacity - at < frames - i ? r->capacity - at : frames - i;
		memcpy(dst + i * 2, r->buf + at * 2, run * 4);
		i += run;
	}
	__atomic_store_n(&r->tail, tail + frames, __ATOMIC_RELEASE);
	return frames;
}

/* Consumer side reset (e.g. after a seek, with the producer paused). */
static inline void pcm_ring_clear(struct pcm_ring *r)
{
	__atomic_store_n(&r->tail, __atomic_load_n(&r->head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
}

#endif
