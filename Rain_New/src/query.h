#ifndef RAIN_QUERY_H
#define RAIN_QUERY_H
#include "store.h"
#include "util.h"

/* All functions append to *o and return 0 on success, -1 on failure. Inputs must be pre-validated. */
int q_day_json(store_t *s, int dev, const char *date, sbuf_t *o);
int q_minutes_json(store_t *s, int dev, const char *hour_ymdh, sbuf_t *o);  /* "YYYY-MM-DD HH" */
int q_range(store_t *s, int dev, const char *from, const char *to, const char *by, int csv, sbuf_t *o);
int q_status_json(store_t *s, int dev, sbuf_t *o);
int q_summary_json(store_t *s, int dev, sbuf_t *o);
int q_alarms_json(store_t *s, int dev, sbuf_t *o);
int q_day_text(store_t *s, int dev, const char *date, sbuf_t *o);           /* console report, ASCII */
int q_unit(store_t *s, int dev);
#endif
