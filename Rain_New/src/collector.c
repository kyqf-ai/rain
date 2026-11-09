#include "collector.h"
#include "serial.h"
#include "store.h"
#include "log.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { XE_OK = 0, XE_NODATA = 1, XE_TIMEOUT = -1, XE_IO = -2 };
#define QUIET_MS 60
#define NODATA_MAX 512

static col_status_t g_cs;            /* guarded by plat_lock() */
static volatile int g_poke = 0;

typedef struct {
    char machine[48];
    int raw5_logged;
    cfg_t cfg;
    store_t *st;
    serial_t *ser;
    int dev;
    int swept;                                   /* the startup gap sweep has finished */
    int64_t stuck_e; int stuck_n;                /* an hour that keeps timing out */
    int64_t nodata[NODATA_MAX]; int n_nodata;     /* past hours the device answered with "no data" */
    int64_t last_hist_ms;
    int64_t prov_e; int prov_total;              /* last provisional save: skip unchanged rewrites */
    unsigned long crc_err, tx, rx_ok;
} col_t;

static void now_str(char *out, size_t n) { wtime_t t; wtime_now(&t); wtime_fmt(&t, WT_FULL, out, n); }

static void beat(void) { plat_lock(); g_cs.heartbeat_ms = plat_mono_ms(); plat_unlock(); }

void collector_reset_status(void)
{
    plat_lock();
    memset(&g_cs, 0, sizeof g_cs);
    snprintf(g_cs.state, sizeof g_cs.state, "starting");
    g_cs.heartbeat_ms = plat_mono_ms();
    plat_flag_set(&g_poke, 0);
    plat_unlock();
}
void collector_get_status(col_status_t *out) { plat_lock(); *out = g_cs; plat_unlock(); }
void collector_poke(void) { plat_flag_set(&g_poke, 1); }
void collector_thread(void *p) { collector_run((const cfg_t *)p); }

static void set_state(col_t *c, const char *state, const char *msg, int ok)
{
    char now[32];
    now_str(now, sizeof now);
    plat_lock();
    snprintf(g_cs.state, sizeof g_cs.state, "%s", state);
    snprintf(g_cs.msg, sizeof g_cs.msg, "%s", msg ? msg : "");
    if (c->machine[0]) snprintf(g_cs.machine, sizeof g_cs.machine, "%s", c->machine);
    if (ok) snprintf(g_cs.last_ok, sizeof g_cs.last_ok, "%s", now);
    plat_unlock();
    if (c->dev <= 0 && c->cfg.machine[0] && c->st)
        c->dev = store_device(c->st, c->cfg.machine, c->cfg.name, c->cfg.unit);
    if (c->dev > 0 && c->st) store_set_state(c->st, c->dev, state, msg, now, ok);
}

/* sleep that wakes early on stop or poke, and keeps the heartbeat alive */
static void wait_poll(int ms)
{
    int64_t end = plat_mono_ms() + ms, last = 0;
    while (!plat_stop_requested() && !plat_flag_get(&g_poke) && plat_mono_ms() < end) {
        int64_t now = plat_mono_ms();
        if (now - last >= 1000) { beat(); last = now; }
        plat_sleep_ms(100);
    }
    plat_flag_set(&g_poke, 0);
}

static void trace(col_t *c, const char *dir, const uint8_t *b, size_t n)
{
    char hx[3 * 256 + 1];
    if (!c->cfg.trace) return;
    hex_str(b, n > 256 ? 256 : n, hx, sizeof hx);
    LOGD("%s %u bytes: %s", dir, (unsigned)n, hx);
}

/* One request/response exchange with retries. */
static int xact(col_t *c, const uint8_t *cmd, size_t ncmd, const uint8_t *want, int nwant, frame_t *got)
{
    int attempt, i;
    for (attempt = 0; attempt < c->cfg.retries; attempt++) {
        uint8_t buf[512]; size_t len = 0; int found = 0, saw_end = 0, bad = 0;
        int64_t t0, last_rx, deadline;
        beat();
        serial_flush_in(c->ser);
        if (serial_write(c->ser, cmd, ncmd) < 0) return XE_IO;
        c->tx++; trace(c, "TX", cmd, ncmd);
        t0 = plat_mono_ms(); last_rx = t0; deadline = t0 + c->cfg.timeout_ms;
        while (!plat_stop_requested()) {
            int64_t now = plat_mono_ms(); int slice, r, quiet;
            if (now >= deadline) break;
            if (len >= sizeof buf) {                 /* nothing parseable and no room left: junk flood */
                LOGW("rx buffer full (%u bytes of junk), retrying", (unsigned)len);
                c->crc_err++; bad = 1; len = 0;
                break;
            }
            slice = (int)(deadline - now); if (slice > 40) slice = 40;
            r = serial_read(c->ser, buf + len, sizeof buf - len, slice);
            if (r < 0) return XE_IO;
            if (r > 0) { trace(c, "RX", buf + len, (size_t)r); len += (size_t)r; last_rx = plat_mono_ms(); }
            /* "quiet" = the line has been idle for QUIET_MS (with or without pending bytes).
             * It used to require len > 0, which made the "saw_end && len == 0" (device has no
             * data) and "bad && len == 0" (retry after a corrupt frame) checks below unreachable. */
            quiet = plat_mono_ms() - last_rx >= QUIET_MS;
            for (;;) {
                frame_t fr; size_t used = 0; ps_t ps = proto_next(buf, len, &fr, &used, quiet);
                if (ps == PS_NEED_MORE) break;
                if (ps == PS_FRAME) {
                    if (fr.type == F_END) saw_end = 1;
                    else for (i = 0; i < nwant; i++) if (fr.type == want[i] && !found) { *got = fr; found = 1; }
                } else if (ps == PS_BADCRC) { c->crc_err++; bad = 1; LOGW("bad frame / CRC error, dropped %u bytes", (unsigned)used); }
                if (used == 0) break;
                memmove(buf, buf + used, len - used); len -= used;
            }
            if (found) {
                int is_data = got->type == F_DATA0 || got->type == F_DATA5;
                if (is_data) {                      /* swallow the trailing AA09 marker */
                    int64_t until = plat_mono_ms() + QUIET_MS;
                    while (plat_mono_ms() < until) { uint8_t d[64]; int k = serial_read(c->ser, d, sizeof d, 20); if (k < 0) return XE_IO; if (k > 0) trace(c, "RX", d, (size_t)k); }
                }
                c->rx_ok++;
                return XE_OK;
            }
            if (saw_end && quiet && len == 0) return XE_NODATA;
            if (bad && quiet && len == 0) break;    /* retry right away */
        }
        if (plat_stop_requested()) return XE_TIMEOUT;
        LOGD("no valid reply (attempt %d/%d)", attempt + 1, c->cfg.retries);
    }
    return XE_TIMEOUT;
}

static int cmd_simple(col_t *c, uint8_t code, uint8_t want, frame_t *fr)
{
    uint8_t cmd[16]; size_t n = proto_build(cmd, code, 0x01, NULL, 0);
    return xact(c, cmd, n, &want, 1, fr);
}

static void disconnect(col_t *c) { if (c->ser) { serial_close(c->ser); c->ser = NULL; } }

static int connect_dev(col_t *c)
{
    char err[160], id[48]; frame_t fr; int rc;
    c->ser = serial_open(c->cfg.port, c->cfg.baud, c->cfg.dtr, c->cfg.rts, err, sizeof err);
    if (!c->ser) { LOGE("%s", err); set_state(c, "port_error", err, 0); return -1; }
    rc = cmd_simple(c, 0x01, F_ID, &fr);
    if (rc != XE_OK || dec_id(&fr, id, sizeof id) != 0) {
        LOGW("device did not answer the id request on %s", c->cfg.port);
        set_state(c, "no_reply", "device did not answer the id request", 0);
        disconnect(c); return -1;
    }
    if (c->cfg.machine[0] && strcmp(c->cfg.machine, id) != 0) {
        char m[160]; snprintf(m, sizeof m, "device id %s does not match configured %s", id, c->cfg.machine);
        LOGE("%s", m); set_state(c, "machine_mismatch", m, 0); disconnect(c); return -1;
    }
    snprintf(c->machine, sizeof c->machine, "%s", id);
    c->dev = store_device(c->st, id, c->cfg.name, c->cfg.unit);
    if (c->dev < 0) { LOGE("cannot register device in database"); disconnect(c); return -1; }
    {   /* the station name lives in the database so it can be edited there */
        char nm[64]; nm[0] = 0;
        store_device_name(c->st, c->dev, nm, sizeof nm);
        plat_lock();
        snprintf(g_cs.name, sizeof g_cs.name, "%s", nm);
        plat_unlock();
        if (nm[0] && strcmp(nm, id) != 0) LOGI("station name: %s", nm);
    }
    LOGI("connected to %s on %s (device #%d)", id, c->cfg.port, c->dev);
    return 0;
}

static int nodata_has(col_t *c, int64_t e) { int i; for (i = 0; i < c->n_nodata; i++) if (c->nodata[i] == e) return 1; return 0; }
static void nodata_add(col_t *c, int64_t e)
{
    if (c->n_nodata >= NODATA_MAX) { memmove(c->nodata, c->nodata + 1, sizeof(int64_t) * (NODATA_MAX - 1)); c->n_nodata--; }
    c->nodata[c->n_nodata++] = e;
}

static int request_hour(col_t *c, const wtime_t *h, frame_t *fr)
{
    uint8_t a[5], cmd[16], want[2] = { F_DATA0, F_DATA5 }; size_t n;
    a[0] = (uint8_t)(h->y - 2000); a[1] = (uint8_t)h->mo; a[2] = (uint8_t)h->d; a[3] = (uint8_t)h->h; a[4] = 1;
    n = proto_build(cmd, 0x00, 0x01, a, 5);
    return xact(c, cmd, n, want, 2, fr);
}

static int fetch_hour(col_t *c, const wtime_t *h, const wtime_t *dev_now, int is_current)
{
    frame_t fr; hourdata_t hd; int rc; char now[32], hk[24]; int is_final; int64_t he = wtime_epoch(h), ge;
    rc = request_hour(c, h, &fr);
    wtime_fmt(h, WT_HOUR, hk, sizeof hk);
    if (rc == XE_NODATA) { if (!is_current) { nodata_add(c, he); LOGW("device has no data for %s", hk); } return 0; }
    if (rc != XE_OK) return rc;
    if (dec_hour(&fr, &hd, cfg_endian(&c->cfg), c->cfg.max_per_min) != 0) { LOGW("cannot decode hour frame for %s", hk); return 0; }
    ge = wtime_epoch(&hd.hour);
    if (ge != he) {
        if (ge == he + 3600) {
            /* Seen in the field (run log 2026-11-08): a request for hour 23 of some days
             * is answered with the NEXT hour's record (device-side address rollover).
             * The answer itself is valid data for that next hour: store it there, and
             * record the requested hour as a known gap so sweeps stop re-requesting it. */
            char gk[24];
            now_str(now, sizeof now);
            wtime_fmt(&hd.hour, WT_HOUR, gk, sizeof gk);
            if (store_save_hour(c->st, c->dev, &hd, wtime_epoch(dev_now) >= ge + 3600 + 10, now) != 0) return XE_IO;
            nodata_add(c, he);
            LOGW("request for %s answered with next hour %s (device rollover): %s stored, %s recorded as a data gap",
                 hk, gk, gk, hk);
            return 0;
        }
        LOGW("device returned %02d-%02d %02d:00 for request %s, ignored", hd.hour.mo, hd.hour.d, hd.hour.h, hk);
        return 0;
    }
    if (hd.suspicious) LOGW("hour %s has implausible minute values (byte order setting?)", hk);
    if (fr.type == F_DATA5 && c->raw5_logged < 3) {     /* evidence for verifying byte order / unit afterwards */
        char hx[3 * PROTO_MAX_FRAME + 1]; hex_str(fr.raw, fr.len, hx, sizeof hx);
        LOGI("first rain frame #%d for %s decoded as %s-endian, total %d counts: %s", ++c->raw5_logged, hk, hd.used == EN_BE ? "big" : "little", hd.total, hx);
    }
    is_final = wtime_epoch(dev_now) >= he + 3600 + 10;     /* 10 s grace after the hour ends */
    /* The current hour is requested every cycle; re-saving an unchanged provisional row
     * only churns the database and spams the log (observed: 51 identical writes in 4 min
     * at poll=5 s). total is the sum of the minute counters, so an unchanged total means
     * unchanged data. The final write after the hour closes always goes through. */
    if (!is_final && c->prov_e == he && c->prov_total == hd.total) return 0;
    now_str(now, sizeof now);
    if (store_save_hour(c->st, c->dev, &hd, is_final, now) != 0) return XE_IO;
    if (!is_final) { c->prov_e = he; c->prov_total = hd.total; }
    if (hd.total > 0) {
        LOGI("hour %s: %.2f mm (%s)", hk, hd.total / (double)c->cfg.unit, is_final ? "final" : "provisional");
        plat_lock();
        { char lr[40]; snprintf(lr, sizeof lr, "%.13s %.2f mm", hk, hd.total / (double)c->cfg.unit);
          if (strncmp(lr, g_cs.last_rain, 13) >= 0) snprintf(g_cs.last_rain, sizeof g_cs.last_rain, "%s", lr); }
        plat_unlock();
    }
    return 0;
}

static void sync_progress(col_t *c, const char *hk, int64_t left)
{
    char msg[160];
    snprintf(msg, sizeof msg, "正在补齐历史数据：%s（还剩 %lld 小时）", hk, (long long)left);
    set_state(c, "backfill", msg, 0);
}

static int sync_hours(col_t *c, const wtime_t *dev_now)
{
    wtime_t cur = *dev_now, h0, h; char last[24], hk[24]; int64_t e, he, earliest;
    int n = 0, rc, tick = 0;
    int sweep = !c->swept;          /* the first cycle walks the whole window once */
    wtime_hour_floor(&cur);
    he = wtime_epoch(&cur);
    earliest = he - 3600LL * c->cfg.backfill_hours;
    if (store_last_final_hour(c->st, c->dev, last, sizeof last) == 0 && wtime_parse(last, &h0) == 0) e = wtime_epoch(&h0) + 3600;
    else e = earliest;
    if (e < earliest) e = earliest;
    if (sweep) e = earliest;        /* also look at holes older than the newest final hour */
    if (e > he) e = he;
    for (; e <= he; e += 3600) {
        wtime_from_epoch(e, &h);
        wtime_fmt(&h, WT_HOUR, hk, sizeof hk);
        if (e < he) {
            if (nodata_has(c, e)) continue;                        /* a real hole: pass it by */
            if (store_hour_is_final(c->st, c->dev, hk) == 1) continue;
            if (!sweep && n >= c->cfg.max_fetch) { e = he - 3600; continue; }
            n++;
            if (sweep && tick++ % 24 == 0) sync_progress(c, hk, (he - e) / 3600);
        }
        rc = fetch_hour(c, &h, dev_now, e == he);
        if (rc == XE_IO) return rc;
        if (rc == XE_TIMEOUT) {
            if (e == he) return rc;                                /* always retry the current hour */
            if (e == c->stuck_e) c->stuck_n++; else { c->stuck_e = e; c->stuck_n = 1; }
            if (c->stuck_n >= 3) {                                 /* one dead hour must not block the rest */
                LOGW("no reply for %s, skipped after 3 tries", hk);
                nodata_add(c, e);
                c->stuck_e = 0; c->stuck_n = 0;
                continue;
            }
            return rc;
        }
    }
    c->swept = 1;
    c->stuck_e = 0; c->stuck_n = 0;
    return 0;
}

static void publish_totals(col_t *c)
{
    wtime_t t; char d[16], hk[24]; long day = 0, hour = 0; int unit = store_unit(c->st, c->dev);
    wtime_now(&t); wtime_fmt(&t, WT_DATE, d, sizeof d); wtime_fmt(&t, WT_HOUR, hk, sizeof hk);
    if (store_day_total(c->st, c->dev, d, &day) != 0) return;
    if (store_hour_total(c->st, c->dev, hk, &hour) != 0) hour = 0;
    plat_lock();
    g_cs.have_today = 1;
    g_cs.today_x100 = (int)((long long)day * 100 / unit);
    g_cs.hour_x100 = (int)((long long)hour * 100 / unit);
    plat_unlock();
}

static int cycle(col_t *c)
{
    frame_t fr; status_t st; int rc, off, keep; char now[32]; wtime_t pc;
    rc = cmd_simple(c, 0x06, F_STATUS, &fr);
    if (rc != XE_OK) return rc;
    if (dec_status(&fr, &st) != 0) { LOGW("status frame decode failed"); return XE_TIMEOUT; }
    wtime_now(&pc); now_str(now, sizeof now);
    off = (int)(wtime_epoch(&st.t) - wtime_epoch(&pc));
    keep = plat_mono_ms() - c->last_hist_ms >= (int64_t)c->cfg.status_hist_sec * 1000 || c->last_hist_ms == 0;
    if (keep) c->last_hist_ms = plat_mono_ms();
    store_put_status(c->st, c->dev, &st, off, now, keep);
    plat_lock();
    g_cs.have_status = 1; g_cs.clock_off = off;
    wtime_fmt(&st.t, WT_FULL, g_cs.dev_time, sizeof g_cs.dev_time);
    g_cs.v_static = st.v_static; g_cs.v_sun = st.v_sun; g_cs.v_alarm = st.v_alarm;
    plat_unlock();
    if (st.v_static < st.v_alarm && st.v_alarm > 0) LOGW("battery voltage %d mV is below alarm level %d mV", st.v_static, st.v_alarm);
    if (off > 300 || off < -300) LOGW("device clock differs from PC by %d s", off);
    rc = sync_hours(c, &st.t);
    if (rc != 0) return rc;
    publish_totals(c);
    set_state(c, "online", "", 1);
    return 0;
}

static int col_init(col_t *c, const cfg_t *cfg)
{
    char err[200];
    memset(c, 0, sizeof *c);
    c->cfg = *cfg;
    c->st = store_open(cfg->db, err, sizeof err);
    if (!c->st) { LOGE("%s", err); return -1; }
    return 0;
}
static void col_free(col_t *c) { disconnect(c); store_close(c->st); c->st = NULL; }

int collector_run(const cfg_t *cfg)
{
    col_t c; int fails = 0;
    if (col_init(&c, cfg) != 0) return 1;
    LOGI("collector started: port %s %d baud, poll %d s, db %s", cfg->port, cfg->baud, cfg->poll_sec, cfg->db);
    beat();
    while (!plat_stop_requested()) {
        int64_t t0 = plat_mono_ms(); int rc;
        beat();
        if (!c.ser) {
            if (connect_dev(&c) != 0) { wait_poll(cfg->reconnect_sec * 1000); continue; }
            fails = 0;
        }
        rc = cycle(&c);
        plat_lock(); g_cs.cycles++; g_cs.crc_errors = c.crc_err; g_cs.frames_ok = c.rx_ok; plat_unlock();
        if (rc == XE_IO) {
            LOGE("serial I/O error, reconnecting"); set_state(&c, "port_error", "serial I/O error", 0);
            disconnect(&c); wait_poll(cfg->reconnect_sec * 1000); continue;
        }
        if (rc != 0) {
            fails++;
            LOGW("device not responding (%d consecutive)", fails);
            if (fails >= 3) set_state(&c, "offline", "device not responding", 0);
            if (fails >= 10) { disconnect(&c); }
        } else fails = 0;
        { int64_t wait = (int64_t)cfg->poll_sec * 1000 - (plat_mono_ms() - t0); if (wait > 0) wait_poll((int)wait); }
    }
    set_state(&c, "stopped", "collector stopped", 0);
    LOGI("collector stopped (frames ok %lu, crc errors %lu)", c.rx_ok, c.crc_err);
    col_free(&c);
    return 0;
}

int collector_once(const cfg_t *cfg)
{
    col_t c; int rc;
    if (col_init(&c, cfg) != 0) return 1;
    c.swept = 1;                                  /* one incremental cycle, no full sweep */
    if (connect_dev(&c) != 0) { col_free(&c); return 2; }
    rc = cycle(&c);
    if (rc != 0) LOGE("sync cycle failed (%d)", rc); else LOGI("sync cycle ok");
    col_free(&c);
    return rc == 0 ? 0 : 3;
}

int collector_probe(const cfg_t *cfg, const char *hour)
{
    col_t c; wtime_t h; frame_t fr; hourdata_t a; char hx[3 * PROTO_MAX_FRAME + 1]; int rc, i, k;
    if (wtime_parse(hour, &h) != 0) { fprintf(stderr, "bad hour '%s' (use \"YYYY-MM-DD HH\")\n", hour); return 1; }
    h.mi = 0; h.s = 0;
    if (col_init(&c, cfg) != 0) return 1;
    if (connect_dev(&c) != 0) { col_free(&c); return 2; }
    rc = request_hour(&c, &h, &fr);
    if (rc == XE_NODATA) { printf("device answered with the end marker only: no data for that hour\n"); col_free(&c); return 0; }
    if (rc != XE_OK) { printf("no valid reply (%d)\n", rc); col_free(&c); return 3; }
    hex_str(fr.raw, fr.len, hx, sizeof hx);
    printf("frame type %02X, %u bytes\n%s\n", fr.type, (unsigned)fr.len, hx);
    for (k = 0; k < 2; k++) {
        if (dec_hour(&fr, &a, k ? EN_BE : EN_LE, cfg->max_per_min) != 0) { printf("decode failed\n"); break; }
        printf("%s decode: total %d counts (%.2f mm), temp %s%.1f, %s\n", k ? "big-endian   " : "little-endian",
               a.total, a.total / (double)cfg->unit, a.temp_valid ? "" : "invalid ", a.temp10 / 10.0, a.suspicious ? "IMPLAUSIBLE" : "plausible");
        for (i = 0; i < 60; i++) if (a.min[i]) printf("   minute %02d: %u\n", i, a.min[i]);
    }
    col_free(&c);
    return 0;
}
