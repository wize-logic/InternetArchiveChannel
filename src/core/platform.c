#include "core/platform.h"

#ifdef HW_RVL
#include <ogc/lwp_watchdog.h>

uint64_t plat_ms(void) { return ticks_to_millisecs(gettime()); }
uint64_t plat_us(void) { return ticks_to_microsecs(gettime()); }

void plat_mutex_init(iac_mutex *m) { LWP_MutexInit(m, false); }
void plat_mutex_lock(iac_mutex *m) { LWP_MutexLock(*m); }
void plat_mutex_unlock(iac_mutex *m) { LWP_MutexUnlock(*m); }
void plat_mutex_destroy(iac_mutex *m) { LWP_MutexDestroy(*m); }

int plat_thread_create(iac_thread *t, void *(*fn)(void *), void *arg, size_t stack_size, int priority)
{
	/* NULL stack: libogc allocates it. */
	return LWP_CreateThread(t, fn, arg, NULL, (u32)stack_size, (u8)priority) < 0 ? -1 : 0;
}
void plat_thread_join(iac_thread t) { LWP_JoinThread(t, NULL); }

#else
#include <time.h>

uint64_t plat_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
uint64_t plat_ms(void) { return plat_us() / 1000u; }

void plat_mutex_init(iac_mutex *m) { pthread_mutex_init(m, NULL); }
void plat_mutex_lock(iac_mutex *m) { pthread_mutex_lock(m); }
void plat_mutex_unlock(iac_mutex *m) { pthread_mutex_unlock(m); }
void plat_mutex_destroy(iac_mutex *m) { pthread_mutex_destroy(m); }

int plat_thread_create(iac_thread *t, void *(*fn)(void *), void *arg, size_t stack_size, int priority)
{
	(void)priority;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, stack_size < 65536 ? 65536 : stack_size);
	int rc = pthread_create(t, &attr, fn, arg);
	pthread_attr_destroy(&attr);
	return rc == 0 ? 0 : -1;
}
void plat_thread_join(iac_thread t) { pthread_join(t, NULL); }
#endif
