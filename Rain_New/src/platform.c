#ifdef _WIN32
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0601
#  endif
#  include <windows.h>
#  include <direct.h>
#else
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#  endif
#  include <pthread.h>
#  include <signal.h>
#  include <sys/stat.h>
#  include <errno.h>
#  include <unistd.h>
#endif
#include <stdlib.h>
#include "platform.h"

#if defined(__GNUC__)
int  plat_flag_get(const volatile int *f)  { return __atomic_load_n(f, __ATOMIC_SEQ_CST); }
void plat_flag_set(volatile int *f, int v) { __atomic_store_n(f, v, __ATOMIC_SEQ_CST); }
#elif defined(_WIN32)
int  plat_flag_get(const volatile int *f)  { return (int)InterlockedCompareExchange((volatile LONG *)f, 0, 0); }
void plat_flag_set(volatile int *f, int v) { InterlockedExchange((volatile LONG *)f, (LONG)v); }
#else
#  error "no atomic primitives for this compiler"
#endif

static volatile int g_stop = 0;
int  plat_stop_requested(void) { return plat_flag_get(&g_stop); }
void plat_request_stop(void)   { plat_flag_set(&g_stop, 1); }
void plat_clear_stop(void)     { plat_flag_set(&g_stop, 0); }

void plat_sleep_interruptible(int ms)
{
    while (ms > 0 && !plat_stop_requested()) {
        int s = ms > 100 ? 100 : ms;
        plat_sleep_ms(s);
        ms -= s;
    }
}

#ifdef _WIN32
static CRITICAL_SECTION g_cs;
static int g_cs_ok = 0;
void plat_init(void) { if (!g_cs_ok) { InitializeCriticalSection(&g_cs); g_cs_ok = 1; } }
void plat_lock(void) { if (g_cs_ok) EnterCriticalSection(&g_cs); }
void plat_unlock(void) { if (g_cs_ok) LeaveCriticalSection(&g_cs); }
void plat_sleep_ms(int ms) { Sleep((DWORD)ms); }
int64_t plat_mono_ms(void) { return (int64_t)GetTickCount64(); }
/* localtime() uses a per-thread buffer in the Windows C runtime, and exists in every toolchain (localtime_s does not) */
void plat_localtime(time_t t, struct tm *out) { struct tm *p = localtime(&t); if (p) *out = *p; }
int plat_mkdir(const char *path)
{
    if (_mkdir(path) == 0) return 0;
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES ? 0 : -1;
}
struct plat_thread { HANDLE h; void (*fn)(void *); void *arg; };
static DWORD WINAPI thread_tramp(LPVOID p)
{
    struct plat_thread *t = (struct plat_thread *)p;
    t->fn(t->arg);
    return 0;
}
plat_thread_t *plat_thread_start(void (*fn)(void *), void *arg)
{
    struct plat_thread *t = (struct plat_thread *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn; t->arg = arg;
    t->h = CreateThread(NULL, 0, thread_tramp, t, 0, NULL);
    if (!t->h) { free(t); return NULL; }
    return t;
}
void plat_thread_join(plat_thread_t *t)
{
    if (!t) return;
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}
static BOOL WINAPI ctrl_handler(DWORD ev)
{
    if (ev == CTRL_C_EVENT || ev == CTRL_BREAK_EVENT || ev == CTRL_CLOSE_EVENT ||
        ev == CTRL_SHUTDOWN_EVENT || ev == CTRL_LOGOFF_EVENT) { plat_request_stop(); return TRUE; }
    return FALSE;
}
void plat_install_stop_handler(void) { SetConsoleCtrlHandler(ctrl_handler, TRUE); }
#else
static pthread_mutex_t g_mu = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
void plat_init(void) {}
void plat_lock(void) { pthread_mutex_lock(&g_mu); }
void plat_unlock(void) { pthread_mutex_unlock(&g_mu); }
void plat_sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000; ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR && !plat_stop_requested()) {}
}
int64_t plat_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
void plat_localtime(time_t t, struct tm *out) { localtime_r(&t, out); }
int plat_mkdir(const char *path)
{
    if (mkdir(path, 0755) == 0) return 0;
    return errno == EEXIST ? 0 : -1;
}
struct plat_thread { pthread_t th; void (*fn)(void *); void *arg; };
static void *thread_tramp(void *p)
{
    struct plat_thread *t = (struct plat_thread *)p;
    t->fn(t->arg);
    return NULL;
}
plat_thread_t *plat_thread_start(void (*fn)(void *), void *arg)
{
    struct plat_thread *t = (struct plat_thread *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn; t->arg = arg;
    if (pthread_create(&t->th, NULL, thread_tramp, t) != 0) { free(t); return NULL; }
    return t;
}
void plat_thread_join(plat_thread_t *t)
{
    if (!t) return;
    pthread_join(t->th, NULL);
    free(t);
}
static void on_sig(int s) { (void)s; plat_request_stop(); }
void plat_install_stop_handler(void)
{
    struct sigaction sa;
    sa.sa_handler = on_sig; sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}
#endif
