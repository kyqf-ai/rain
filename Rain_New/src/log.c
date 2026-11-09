#include "log.h"
#include "util.h"
#include "platform.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define RING_N 300
#define RING_W 220
static char g_ring[RING_N][RING_W];
static unsigned long g_head = 0;           /* sequence number of the newest line */
static char g_dir[260];
static int g_console = 1, g_debug = 0;

void log_init(const char *dir, int console, int debug)
{
    g_dir[0] = 0;
    if (dir) { strncpy(g_dir, dir, sizeof g_dir - 1); g_dir[sizeof g_dir - 1] = 0; }
    g_console = console; g_debug = debug;
    if (g_dir[0]) plat_mkdir(g_dir);
}

void log_msg(int lvl, const char *fmt, ...)
{
    static const char *N[] = { "ERR ", "WARN", "INFO", "DBG " };
    wtime_t t; char ts[32], msg[1024], line[1100], path[320];
    va_list ap; FILE *f;
    if (lvl == 3 && !g_debug) return;
    wtime_now(&t); wtime_fmt(&t, WT_FULL, ts, sizeof ts);
    va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    snprintf(line, sizeof line, "%s [%s] %s\n", ts, N[lvl], msg);
    plat_lock();
    g_head++;
    {   /* store at most RING_W-1 bytes, never cutting a UTF-8 sequence, without the trailing newline */
        size_t n = strlen(line);
        char *dst = g_ring[g_head % RING_N];
        if (n && line[n - 1] == '\n') n--;
        if (n >= RING_W) { n = RING_W - 1; while (n > 0 && ((unsigned char)line[n] & 0xC0) == 0x80) n--; }
        memcpy(dst, line, n); dst[n] = 0;
    }
    plat_unlock();
    if (g_console) { fputs(line, lvl <= 1 ? stderr : stdout); fflush(lvl <= 1 ? stderr : stdout); }
    if (g_dir[0]) {
        snprintf(path, sizeof path, "%s/rain-%04d%02d%02d.log", g_dir, t.y, t.mo, t.d);
        f = fopen(path, "ab");
        if (f) { fputs(line, f); fclose(f); }
    }
}

int log_ring_next(unsigned long *cursor, char *out, size_t outsz)
{
    int ok = 0;
    plat_lock();
    if (*cursor < g_head) {
        if (g_head - *cursor > RING_N) *cursor = g_head - RING_N;
        (*cursor)++;
        snprintf(out, outsz, "%s", g_ring[*cursor % RING_N]);
        ok = 1;
    }
    plat_unlock();
    return ok;
}
