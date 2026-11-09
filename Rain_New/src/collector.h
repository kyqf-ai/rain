#ifndef RAIN_COLLECTOR_H
#define RAIN_COLLECTOR_H
#include <stdint.h>
#include "config.h"

/* Snapshot of the collector, safe to read from another thread (the GUI). */
typedef struct {
    char state[24];          /* starting backfill online offline no_reply port_error machine_mismatch stopped */
    char msg[160];
    char machine[48];
    char name[64];           /* station name as stored in the database (device.name) */
    char last_ok[32];        /* PC time of the last fully successful cycle */
    char dev_time[32];
    int  have_status;
    int  clock_off;          /* device minus PC, seconds */
    int  v_static, v_sun, v_alarm;     /* mV */
    int  have_today;
    int  today_x100, hour_x100;        /* mm * 100 */
    char last_rain[40];      /* "YYYY-MM-DD HH:00 0.30 mm" of the newest hour with rain */
    unsigned long cycles, crc_errors, frames_ok;
    int64_t heartbeat_ms;    /* plat_mono_ms() of the last sign of life of the collector thread */
} col_status_t;

void collector_reset_status(void);                 /* call before starting the thread */
void collector_get_status(col_status_t *out);      /* thread-safe copy */
void collector_poke(void);                         /* run the next cycle now */
void collector_thread(void *cfg_ptr);              /* thread entry: collector_run((const cfg_t *)cfg_ptr) */

int collector_run(const cfg_t *cfg);                     /* until plat_request_stop() */
int collector_once(const cfg_t *cfg);                    /* connect, one sync cycle, exit; 0 ok */
int collector_probe(const cfg_t *cfg, const char *hour); /* "YYYY-MM-DD HH": print raw + decoded, no DB write */
#endif
