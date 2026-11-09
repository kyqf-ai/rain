#ifndef RAIN_PLATFORM_H
#define RAIN_PLATFORM_H
#include <stdint.h>
#include <time.h>

/* Atomic int flag helpers for cross-thread requests (stop, poke). Plain 'volatile int' is not atomic in C. */
int     plat_flag_get(const volatile int *f);
void    plat_flag_set(volatile int *f, int v);

void    plat_init(void);                            /* call once from the main thread, before any other thread or log call */
void    plat_lock(void);                            /* one global recursive lock for short critical sections */
void    plat_unlock(void);
void    plat_clear_stop(void);                      /* re-arm after a stop (used when the collector is restarted) */
void    plat_sleep_ms(int ms);
int64_t plat_mono_ms(void);
void    plat_localtime(time_t t, struct tm *out);
int     plat_mkdir(const char *path);               /* 0 if created or exists */

typedef struct plat_thread plat_thread_t;
plat_thread_t *plat_thread_start(void (*fn)(void *), void *arg);
void    plat_thread_join(plat_thread_t *t);

void    plat_install_stop_handler(void);            /* Ctrl+C / SIGTERM */
int     plat_stop_requested(void);
void    plat_request_stop(void);
/* sleep that returns early when stop is requested */
void    plat_sleep_interruptible(int ms);
#endif
