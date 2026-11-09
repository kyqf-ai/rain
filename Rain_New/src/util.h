#ifndef RAIN_UTIL_H
#define RAIN_UTIL_H
#include <stdint.h>
#include <stddef.h>
#include "version.h"

/* Naive wall-clock time (device and PC are both local time, no timezone). */
typedef struct { int y, mo, d, h, mi, s; } wtime_t;

/* growable string buffer */
typedef struct { char *p; size_t n, cap; int err; } sbuf_t;
void sb_init(sbuf_t *b);
void sb_free(sbuf_t *b);
void sb_puts(sbuf_t *b, const char *s);
void sb_printf(sbuf_t *b, const char *fmt, ...);
void sb_json_str(sbuf_t *b, const char *s);   /* writes quoted JSON string */

enum { WT_FULL = 0, WT_HOUR = 1, WT_MINUTE = 2, WT_DATE = 3 };
void    wtime_now(wtime_t *t);
int     wtime_valid(const wtime_t *t);
int64_t wtime_epoch(const wtime_t *t);          /* naive seconds since 1970 */
void    wtime_from_epoch(int64_t e, wtime_t *t);
void    wtime_add_sec(wtime_t *t, int64_t sec);
void    wtime_hour_floor(wtime_t *t);
void    wtime_fmt(const wtime_t *t, int style, char *out, size_t outsz);
int     wtime_parse(const char *s, wtime_t *t); /* "YYYY-MM-DD[ HH[:MM[:SS]]]" */
int     date_ok(const char *ymd);               /* strict "YYYY-MM-DD" */

void hex_str(const uint8_t *b, size_t n, char *out, size_t outsz);
#endif
