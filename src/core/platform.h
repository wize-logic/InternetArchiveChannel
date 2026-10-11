/* Small platform abstraction so core/net/ia code builds both for the Wii and on the host. */
#ifndef IAC_PLATFORM_H
#define IAC_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef HW_RVL
#include <ogc/lwp.h>
#include <ogc/mutex.h>
typedef mutex_t iac_mutex;
typedef lwp_t iac_thread;
#else
#include <pthread.h>
typedef pthread_mutex_t iac_mutex;
typedef pthread_t iac_thread;
#endif

/* Monotonic milliseconds since an arbitrary epoch. */
uint64_t plat_ms(void);
/* Monotonic microseconds since an arbitrary epoch. */
uint64_t plat_us(void);

void plat_mutex_init(iac_mutex *m);
void plat_mutex_lock(iac_mutex *m);
void plat_mutex_unlock(iac_mutex *m);
void plat_mutex_destroy(iac_mutex *m);

/* priority is an LWP priority (1-127, higher runs first) and is ignored on the host. */
int plat_thread_create(iac_thread *t, void *(*fn)(void *), void *arg, size_t stack_size, int priority);
void plat_thread_join(iac_thread t);

#endif
