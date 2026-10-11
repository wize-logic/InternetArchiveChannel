#include "net/entropy.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <mbedtls/sha256.h>

#include "core/platform.h"

#ifdef HW_RVL
#include <ogc/es.h>
#include <ogc/lwp_watchdog.h>
#include <network.h>
#include <time.h>
#else
#include <sys/random.h>
#endif

static unsigned char g_pool[32];
static uint64_t g_counter;
static iac_mutex g_pool_lock;
static int g_pool_ready;

static void pool_lock(void)
{
	/* First use happens before any worker threads exist. */
	if (!g_pool_ready) {
		plat_mutex_init(&g_pool_lock);
		g_pool_ready = 1;
	}
	plat_mutex_lock(&g_pool_lock);
}

static void pool_mix_locked(const void *data, size_t len)
{
	mbedtls_sha256_context sha;
	mbedtls_sha256_init(&sha);
	mbedtls_sha256_starts(&sha, 0);
	mbedtls_sha256_update(&sha, g_pool, sizeof g_pool);
	mbedtls_sha256_update(&sha, data, len);
	mbedtls_sha256_finish(&sha, g_pool);
	mbedtls_sha256_free(&sha);
}

void entropy_stir(const void *data, size_t len)
{
	pool_lock();
	pool_mix_locked(data, len);
	plat_mutex_unlock(&g_pool_lock);
}

int entropy_load_seed(const char *path)
{
	unsigned char buf[64];
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	size_t n = fread(buf, 1, sizeof buf, f);
	fclose(f);
	if (n == 0)
		return -1;
	entropy_stir(buf, n);
	memset(buf, 0, sizeof buf);
	return 0;
}

int entropy_save_seed(const char *path, const unsigned char *seed, size_t len)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	size_t n = fwrite(seed, 1, len, f);
	fclose(f);
	return n == len ? 0 : -1;
}

#ifdef HW_RVL
/* Each IOS IPC round-trip takes a slightly different number of timebase ticks
 * (Starlet scheduling, bus contention). We keep all the bits and let SHA-256 condense
 * them, collecting far more samples than the nominal output size. */
static void gather_hw_noise(unsigned char *out, size_t samples)
{
	mbedtls_sha256_context sha;
	mbedtls_sha256_init(&sha);
	mbedtls_sha256_starts(&sha, 0);
	for (size_t i = 0; i < samples; i++) {
		u32 id;
		u64 t0 = gettime();
		ES_GetDeviceID(&id);
		u64 t1 = gettime();
		u64 sample[3] = {t0, t1 - t0, id};
		mbedtls_sha256_update(&sha, (const unsigned char *)sample, sizeof sample);
	}
	unsigned char mac[6];
	if (net_get_mac_address(mac) >= 0)
		mbedtls_sha256_update(&sha, mac, sizeof mac);
	time_t rtc = time(NULL);
	mbedtls_sha256_update(&sha, (const unsigned char *)&rtc, sizeof rtc);
	mbedtls_sha256_finish(&sha, out);
	mbedtls_sha256_free(&sha);
}
#endif

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
	(void)data;
	size_t done = 0;
	while (done < len) {
		unsigned char block[32];
#ifdef HW_RVL
		gather_hw_noise(block, 128);
#else
		if (getrandom(block, sizeof block, 0) != (ssize_t)sizeof block)
			return -1;
#endif
		pool_lock();
		pool_mix_locked(block, sizeof block);
		g_counter++;
		pool_mix_locked(&g_counter, sizeof g_counter);
		memcpy(block, g_pool, sizeof block);
		/* Ratchet so the returned bytes can't be used to recover the pool. */
		pool_mix_locked("ratchet", 7);
		plat_mutex_unlock(&g_pool_lock);

		size_t n = len - done < sizeof block ? len - done : sizeof block;
		memcpy(output + done, block, n);
		done += n;
	}
	*olen = len;
	return 0;
}
