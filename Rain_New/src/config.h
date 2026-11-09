#ifndef RAIN_CONFIG_H
#define RAIN_CONFIG_H
#include "proto.h"

typedef struct {
    /* [serial] */
    char port[64]; int baud, dtr, rts;
    /* [device] */
    char machine[48]; char name[48]; int unit;          /* unit = tip counts per mm */
    /* [collect] */
    int poll_sec, backfill_hours, max_fetch, retries, timeout_ms, reconnect_sec, status_hist_sec;
    char endian[8]; int max_per_min;
    /* [web] */
    int web_enable; char web_bind[48]; int web_port;
    /* [paths] */
    char db[260]; char logdir[260];
    int trace;
} cfg_t;

void cfg_defaults(cfg_t *c);
int  cfg_load(cfg_t *c, const char *path);     /* 0 ok, -1 file missing (defaults kept) */
int  cfg_write_default(const char *path);   /* first-run helper; 0 ok */
endian_t cfg_endian(const cfg_t *c);
#endif
