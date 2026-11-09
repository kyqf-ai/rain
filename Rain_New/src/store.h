#ifndef RAIN_STORE_H
#define RAIN_STORE_H
#include "sqlite3.h"
#include "proto.h"

typedef struct store { sqlite3 *db; } store_t;

store_t *store_open(const char *path, char *err, size_t errsz);
void     store_close(store_t *s);

int store_device(store_t *s, const char *machine, const char *name, int unit);   /* id or -1 */
int store_default_device(store_t *s);                                            /* lowest id or -1 */
int store_device_name(store_t *s, int dev, char *out, size_t outsz);             /* 0 ok, -1 error */

/* is_final: hour is complete. A final row is never replaced by a provisional one. */
int store_save_hour(store_t *s, int dev, const hourdata_t *h, int is_final, const char *recv_at);
int store_last_final_hour(store_t *s, int dev, char *out, size_t outsz);         /* 0 found, -1 none */
int store_hour_is_final(store_t *s, int dev, const char *hour_key);              /* 1 final, 0 missing/provisional, -1 error */

int store_unit(store_t *s, int dev);                                             /* counts per mm */
int store_day_total(store_t *s, int dev, const char *ymd, long *counts);          /* 0 ok */
int store_hour_total(store_t *s, int dev, const char *hour_key, long *counts);   /* 0 found, -1 no row */

/* collector heartbeat / latest status (read by the web UI) */
int store_set_state(store_t *s, int dev, const char *state, const char *msg, const char *now, int is_ok);
int store_put_status(store_t *s, int dev, const status_t *st, int clock_off, const char *now, int keep_history);
#endif
