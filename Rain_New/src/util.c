#include "util.h"
#include "platform.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void sb_reserve(sbuf_t *b, size_t add)
{
    size_t nc;
    char *np;
    if (b->err || b->n + add + 1 <= b->cap) return;
    nc = b->cap ? b->cap : 256;
    while (nc < b->n + add + 1) nc *= 2;
    np = (char *)realloc(b->p, nc);
    if (!np) { b->err = 1; return; }
    b->p = np; b->cap = nc;
}
void sb_init(sbuf_t *b)
{
    b->p = NULL; b->n = 0; b->cap = 0; b->err = 0;
    sb_reserve(b, 0);
    if (b->p) b->p[0] = 0;
}
void sb_free(sbuf_t *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }
void sb_puts(sbuf_t *b, const char *s)
{
    size_t l = strlen(s);
    sb_reserve(b, l);
    if (b->err) return;
    memcpy(b->p + b->n, s, l + 1);
    b->n += l;
}
void sb_printf(sbuf_t *b, const char *fmt, ...)
{
    va_list ap, ap2;
    int l;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    l = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (l > 0) {
        sb_reserve(b, (size_t)l);
        if (!b->err) { vsnprintf(b->p + b->n, (size_t)l + 1, fmt, ap2); b->n += (size_t)l; }
    }
    va_end(ap2);
}
void sb_json_str(sbuf_t *b, const char *s)
{
    char tmp[8];
    sb_puts(b, "\"");
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') sb_puts(b, "\\\"");
        else if (c == '\\') sb_puts(b, "\\\\");
        else if (c < 0x20) { snprintf(tmp, sizeof tmp, "\\u%04x", c); sb_puts(b, tmp); }
        else { tmp[0] = (char)c; tmp[1] = 0; sb_puts(b, tmp); }
    }
    sb_puts(b, "\"");
}

/* Howard Hinnant's civil date algorithms */
static int64_t days_from_civil(int y, int m, int d)
{
    int64_t era; unsigned yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    int64_t era, yy; unsigned doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    yy = (int64_t)yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}
static int days_in_month(int y, int m)
{
    static const int dm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return dm[m - 1];
}
void wtime_now(wtime_t *t)
{
    struct tm tmv;
    plat_localtime(time(NULL), &tmv);
    t->y = tmv.tm_year + 1900; t->mo = tmv.tm_mon + 1; t->d = tmv.tm_mday;
    t->h = tmv.tm_hour; t->mi = tmv.tm_min; t->s = tmv.tm_sec;
}
int wtime_valid(const wtime_t *t)
{
    if (t->y < 2000 || t->y > 2099 || t->mo < 1 || t->mo > 12) return 0;
    if (t->d < 1 || t->d > days_in_month(t->y, t->mo)) return 0;
    return t->h >= 0 && t->h < 24 && t->mi >= 0 && t->mi < 60 && t->s >= 0 && t->s < 60;
}
int64_t wtime_epoch(const wtime_t *t)
{
    return days_from_civil(t->y, t->mo, t->d) * 86400 + t->h * 3600 + t->mi * 60 + t->s;
}
void wtime_from_epoch(int64_t e, wtime_t *t)
{
    int64_t days = e / 86400, rem = e % 86400;
    if (rem < 0) { rem += 86400; days--; }
    civil_from_days(days, &t->y, &t->mo, &t->d);
    t->h = (int)(rem / 3600); t->mi = (int)(rem % 3600 / 60); t->s = (int)(rem % 60);
}
void wtime_add_sec(wtime_t *t, int64_t sec) { wtime_from_epoch(wtime_epoch(t) + sec, t); }
void wtime_hour_floor(wtime_t *t) { t->mi = 0; t->s = 0; }
void wtime_fmt(const wtime_t *t, int style, char *out, size_t n)
{
    switch (style) {
    case WT_HOUR:   snprintf(out, n, "%04d-%02d-%02d %02d:00:00", t->y, t->mo, t->d, t->h); break;
    case WT_MINUTE: snprintf(out, n, "%04d-%02d-%02d %02d:%02d", t->y, t->mo, t->d, t->h, t->mi); break;
    case WT_DATE:   snprintf(out, n, "%04d-%02d-%02d", t->y, t->mo, t->d); break;
    default:        snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d", t->y, t->mo, t->d, t->h, t->mi, t->s);
    }
}
int wtime_parse(const char *s, wtime_t *t)
{
    int n;
    memset(t, 0, sizeof *t);
    n = sscanf(s, "%d-%d-%d %d:%d:%d", &t->y, &t->mo, &t->d, &t->h, &t->mi, &t->s);
    if (n < 3) return -1;
    return wtime_valid(t) ? 0 : -1;
}
int date_ok(const char *s)
{
    wtime_t t; int i;
    if (!s || strlen(s) != 10) return 0;
    for (i = 0; i < 10; i++) {
        if (i == 4 || i == 7) { if (s[i] != '-') return 0; }
        else if (s[i] < '0' || s[i] > '9') return 0;
    }
    return wtime_parse(s, &t) == 0;
}
void hex_str(const uint8_t *b, size_t n, char *out, size_t outsz)
{
    size_t i, o = 0;
    if (!outsz) return;
    for (i = 0; i < n && o + 3 < outsz; i++) o += (size_t)snprintf(out + o, outsz - o, i ? " %02X" : "%02X", b[i]);
    out[o] = 0;
}
