/* Entropy for mbedTLS (MBEDTLS_ENTROPY_HARDWARE_ALT).
 *
 * The Wii exposes no RNG to PPC code, so on hardware we hash together timebase jitter
 * around IOS IPC round-trips, a persisted seed file and anything the app stirs in
 * (Wiimote accelerometer samples, input timing). On the host we use getrandom(). */
#ifndef IAC_ENTROPY_H
#define IAC_ENTROPY_H

#include <stddef.h>

/* Mixes caller-provided noise into the pool. Thread-safe, cheap. */
void entropy_stir(const void *data, size_t len);
/* Mixes a previously saved seed file into the pool. Returns 0 if one was read. */
int entropy_load_seed(const char *path);
/* Writes len bytes of fresh DRBG output for the next boot. */
int entropy_save_seed(const char *path, const unsigned char *seed, size_t len);

#endif
