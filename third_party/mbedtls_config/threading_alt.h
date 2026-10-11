/* mbedTLS MBEDTLS_THREADING_ALT mutex type: LWP mutexes on the Wii, pthreads on the host.
 * The functions are registered by tls_global_init() (src/net/tls.c). */
#ifndef IAC_MBEDTLS_THREADING_ALT_H
#define IAC_MBEDTLS_THREADING_ALT_H

#ifdef HW_RVL
#include <ogc/mutex.h>
typedef struct {
	mutex_t mutex;
	int valid;
} mbedtls_threading_mutex_t;
#else
#include <pthread.h>
typedef struct {
	pthread_mutex_t mutex;
	int valid;
} mbedtls_threading_mutex_t;
#endif

#endif
