#include "store.h"
#include "log.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SCHEMA =
"CREATE TABLE IF NOT EXISTS device("
"  id INTEGER PRIMARY KEY, machine TEXT NOT NULL UNIQUE, name TEXT, unit INTEGER NOT NULL DEFAULT 100, created TEXT);"
/* hour = local wall clock 'YYYY-MM-DD HH:00:00'; total in tip counts (mm = total / unit) */
"CREATE TABLE IF NOT EXISTS rain_hour("
"  dev_id INTEGER NOT NULL, hour TEXT NOT NULL, total INTEGER NOT NULL, temp10 INTEGER,"
"  final INTEGER NOT NULL DEFAULT 0, recv_at TEXT NOT NULL, PRIMARY KEY(dev_id, hour)) WITHOUT ROWID;"
/* only minutes with rain are stored; a missing minute inside a known hour means 0 */
"CREATE TABLE IF NOT EXISTS rain_min("
"  dev_id INTEGER NOT NULL, ts TEXT NOT NULL, cnt INTEGER NOT NULL, PRIMARY KEY(dev_id, ts)) WITHOUT ROWID;"
"CREATE TABLE IF NOT EXISTS device_state("
"  dev_id INTEGER PRIMARY KEY, state TEXT, msg TEXT, updated TEXT, last_ok TEXT,"
"  dev_time TEXT, clock_off INTEGER, v_static INTEGER, v_dyn INTEGER, v_sun INTEGER,"
"  v_alarm INTEGER, v_hang INTEGER, temp_c INTEGER, flags TEXT, raw TEXT);"
"CREATE TABLE IF NOT EXISTS device_status("
"  id INTEGER PRIMARY KEY AUTOINCREMENT, dev_id INTEGER NOT NULL, ts TEXT NOT NULL, dev_time TEXT,"
"  clock_off INTEGER, v_static INTEGER, v_dyn INTEGER, v_sun INTEGER, temp_c INTEGER, raw TEXT);"
"CREATE INDEX IF NOT EXISTS ix_status_ts ON device_status(dev_id, ts);"
"PRAGMA user_version = 1;";

static int exec(store_t *s, const char *sql)
{
    char *e = NULL;
    if (sqlite3_exec(s->db, sql, NULL, NULL, &e) != SQLITE_OK) {
        LOGE("sql: %s", e ? e : sqlite3_errmsg(s->db));
        if (e) sqlite3_free(e);
        return -1;
    }
    return 0;
}
static sqlite3_stmt *prep(store_t *s, const char *sql)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) != SQLITE_OK) {
        LOGE("sql prepare: %s", sqlite3_errmsg(s->db));
        return NULL;
    }
    return st;
}

store_t *store_open(const char *path, char *err, size_t errsz)
{
    store_t *s = (store_t *)calloc(1, sizeof *s);
    if (!s) return NULL;
    if (sqlite3_open_v2(path, &s->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        snprintf(err, errsz, "cannot open database '%s': %s", path, s->db ? sqlite3_errmsg(s->db) : "?");
        if (s->db) sqlite3_close(s->db);
        free(s);
        return NULL;
    }
    sqlite3_busy_timeout(s->db, 5000);
    exec(s, "PRAGMA journal_mode=WAL;");
    exec(s, "PRAGMA synchronous=NORMAL;");
    if (exec(s, SCHEMA) != 0) { snprintf(err, errsz, "cannot create schema"); store_close(s); return NULL; }
    return s;
}
void store_close(store_t *s) { if (s) { if (s->db) sqlite3_close(s->db); free(s); } }

int store_device(store_t *s, const char *machine, const char *name, int unit)
{
    sqlite3_stmt *st; int id = -1; wtime_t t; char now[32];
    st = prep(s, "SELECT id FROM device WHERE machine=?1");
    if (!st) return -1;
    sqlite3_bind_text(st, 1, machine, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (id >= 0) return id;
    wtime_now(&t); wtime_fmt(&t, WT_FULL, now, sizeof now);
    st = prep(s, "INSERT INTO device(machine,name,unit,created) VALUES(?1,?2,?3,?4)");
    if (!st) return -1;
    sqlite3_bind_text(st, 1, machine, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, name && *name ? name : machine, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, unit > 0 ? unit : 100);
    sqlite3_bind_text(st, 4, now, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_DONE) id = (int)sqlite3_last_insert_rowid(s->db);
    sqlite3_finalize(st);
    return id;
}
int store_default_device(store_t *s)
{
    sqlite3_stmt *st = prep(s, "SELECT MIN(id) FROM device"); int id = -1;
    if (!st) return -1;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) id = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return id;
}

int store_save_hour(store_t *s, int dev, const hourdata_t *h, int is_final, const char *recv_at)
{
    sqlite3_stmt *st; char hk[24], mk[24]; int cur_final = -1, i, rc = -1; wtime_t t;
    wtime_fmt(&h->hour, WT_HOUR, hk, sizeof hk);
    if (exec(s, "BEGIN IMMEDIATE") != 0) return -1;
    st = prep(s, "SELECT final FROM rain_hour WHERE dev_id=?1 AND hour=?2");
    if (!st) goto done;
    sqlite3_bind_int(st, 1, dev); sqlite3_bind_text(st, 2, hk, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) cur_final = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (cur_final == 1 && !is_final) { rc = 0; goto done; }     /* never downgrade */

    st = prep(s, "INSERT OR REPLACE INTO rain_hour(dev_id,hour,total,temp10,final,recv_at) VALUES(?1,?2,?3,?4,?5,?6)");
    if (!st) goto done;
    sqlite3_bind_int(st, 1, dev); sqlite3_bind_text(st, 2, hk, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, h->total);
    if (h->temp_valid) sqlite3_bind_int(st, 4, h->temp10); else sqlite3_bind_null(st, 4);
    sqlite3_bind_int(st, 5, is_final ? 1 : 0); sqlite3_bind_text(st, 6, recv_at, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_DONE) { LOGE("save hour: %s", sqlite3_errmsg(s->db)); sqlite3_finalize(st); goto done; }
    sqlite3_finalize(st);

    st = prep(s, "DELETE FROM rain_min WHERE dev_id=?1 AND ts>=?2 AND ts<=?3");
    if (!st) goto done;
    t = h->hour; t.mi = 0; wtime_fmt(&t, WT_MINUTE, mk, sizeof mk);
    sqlite3_bind_int(st, 1, dev); sqlite3_bind_text(st, 2, mk, -1, SQLITE_TRANSIENT);
    t.mi = 59; wtime_fmt(&t, WT_MINUTE, mk, sizeof mk);
    sqlite3_bind_text(st, 3, mk, -1, SQLITE_TRANSIENT);
    sqlite3_step(st); sqlite3_finalize(st);

    st = prep(s, "INSERT INTO rain_min(dev_id,ts,cnt) VALUES(?1,?2,?3)");
    if (!st) goto done;
    for (i = 0; i < 60; i++) {
        if (!h->min[i]) continue;
        t = h->hour; t.mi = i; wtime_fmt(&t, WT_MINUTE, mk, sizeof mk);
        sqlite3_reset(st);
        sqlite3_bind_int(st, 1, dev); sqlite3_bind_text(st, 2, mk, -1, SQLITE_TRANSIENT); sqlite3_bind_int(st, 3, h->min[i]);
        if (sqlite3_step(st) != SQLITE_DONE) { LOGE("save minute: %s", sqlite3_errmsg(s->db)); sqlite3_finalize(st); goto done; }
    }
    sqlite3_finalize(st);
    rc = 0;
done:
    if (exec(s, rc == 0 ? "COMMIT" : "ROLLBACK") != 0) rc = -1;
    return rc;
}

int store_device_name(store_t *s, int dev, char *out, size_t outsz)
{
    sqlite3_stmt *st; int rc = -1;
    if (out && outsz) out[0] = 0;
    if (dev <= 0) return -1;
    st = prep(s, "SELECT name FROM device WHERE id=?1");
    if (!st) return -1;
    sqlite3_bind_int(st, 1, dev);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(st, 0);
        if (out && outsz) snprintf(out, outsz, "%s", p ? p : "");
        rc = 0;
    }
    sqlite3_finalize(st);
    return rc;
}

int store_last_final_hour(store_t *s, int dev, char *out, size_t outsz)
{
    sqlite3_stmt *st = prep(s, "SELECT MAX(hour) FROM rain_hour WHERE dev_id=?1 AND final=1"); int rc = -1;
    if (!st) return -1;
    sqlite3_bind_int(st, 1, dev);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
        snprintf(out, outsz, "%s", (const char *)sqlite3_column_text(st, 0)); rc = 0;
    }
    sqlite3_finalize(st);
    return rc;
}

int store_hour_is_final(store_t *s, int dev, const char *hour_key)
{
    sqlite3_stmt *st = prep(s, "SELECT final FROM rain_hour WHERE dev_id=?1 AND hour=?2"); int final = 0;
    if (!st) return -1;
    sqlite3_bind_int(st, 1, dev); sqlite3_bind_text(st, 2, hour_key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) final = sqlite3_column_int(st, 0) ? 1 : 0;
    sqlite3_finalize(st);
    return final;
}

int store_set_state(store_t *s, int dev, const char *state, const char *msg, const char *now, int is_ok)
{
    sqlite3_stmt *st; int rc;
    st = prep(s, "INSERT OR IGNORE INTO device_state(dev_id,state) VALUES(?1,'starting')");
    if (!st) return -1;
    sqlite3_bind_int(st, 1, dev); sqlite3_step(st); sqlite3_finalize(st);
    st = prep(s, is_ok ? "UPDATE device_state SET state=?2,msg=?3,updated=?4,last_ok=?4 WHERE dev_id=?1"
                       : "UPDATE device_state SET state=?2,msg=?3,updated=?4 WHERE dev_id=?1");
    if (!st) return -1;
    sqlite3_bind_int(st, 1, dev);
    sqlite3_bind_text(st, 2, state, -1, SQLITE_TRANSIENT); sqlite3_bind_text(st, 3, msg ? msg : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, now, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    return rc;
}

int store_put_status(store_t *s, int dev, const status_t *x, int clock_off, const char *now, int keep_history)
{
    sqlite3_stmt *st; char dt[32], fl[16]; int rc;
    wtime_fmt(&x->t, WT_FULL, dt, sizeof dt);
    snprintf(fl, sizeof fl, "%02X%02X%02X%02X", x->flags[0], x->flags[1], x->flags[2], x->flags[3]);
    st = prep(s, "INSERT OR IGNORE INTO device_state(dev_id,state) VALUES(?1,'starting')");
    if (!st) return -1;
    sqlite3_bind_int(st, 1, dev); sqlite3_step(st); sqlite3_finalize(st);
    st = prep(s, "UPDATE device_state SET dev_time=?2,clock_off=?3,v_static=?4,v_dyn=?5,v_sun=?6,v_alarm=?7,v_hang=?8,"
                 "temp_c=?9,flags=?10,raw=?11 WHERE dev_id=?1");
    if (!st) return -1;
    sqlite3_bind_int(st, 1, dev); sqlite3_bind_text(st, 2, dt, -1, SQLITE_TRANSIENT); sqlite3_bind_int(st, 3, clock_off);
    sqlite3_bind_int(st, 4, x->v_static); sqlite3_bind_int(st, 5, x->v_dyn); sqlite3_bind_int(st, 6, x->v_sun);
    sqlite3_bind_int(st, 7, x->v_alarm); sqlite3_bind_int(st, 8, x->v_hang); sqlite3_bind_int(st, 9, x->temp_c);
    sqlite3_bind_text(st, 10, fl, -1, SQLITE_TRANSIENT); sqlite3_bind_text(st, 11, x->raw_hex, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    if (rc == 0 && keep_history) {
        st = prep(s, "INSERT INTO device_status(dev_id,ts,dev_time,clock_off,v_static,v_dyn,v_sun,temp_c,raw) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)");
        if (!st) return -1;
        sqlite3_bind_int(st, 1, dev); sqlite3_bind_text(st, 2, now, -1, SQLITE_TRANSIENT); sqlite3_bind_text(st, 3, dt, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 4, clock_off); sqlite3_bind_int(st, 5, x->v_static); sqlite3_bind_int(st, 6, x->v_dyn);
        sqlite3_bind_int(st, 7, x->v_sun); sqlite3_bind_int(st, 8, x->temp_c); sqlite3_bind_text(st, 9, x->raw_hex, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
        sqlite3_finalize(st);
    }
    return rc;
}

int store_unit(store_t *s, int dev)
{
    sqlite3_stmt *st = prep(s, "SELECT unit FROM device WHERE id=?1"); int u = 100;
    if (!st) return u;
    sqlite3_bind_int(st, 1, dev);
    if (sqlite3_step(st) == SQLITE_ROW) u = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return u > 0 ? u : 100;
}

int store_day_total(store_t *s, int dev, const char *ymd, long *counts)
{
    sqlite3_stmt *st; char lo[24], hi[24]; int rc = -1;
    snprintf(lo, sizeof lo, "%s 00:00:00", ymd); snprintf(hi, sizeof hi, "%s 23:00:00", ymd);
    st = prep(s, "SELECT COALESCE(SUM(total),0) FROM rain_hour WHERE dev_id=?1 AND hour>=?2 AND hour<=?3");
    if (!st) return -1;
    sqlite3_bind_int(st, 1, dev); sqlite3_bind_text(st, 2, lo, -1, SQLITE_TRANSIENT); sqlite3_bind_text(st, 3, hi, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) { *counts = (long)sqlite3_column_int64(st, 0); rc = 0; }
    sqlite3_finalize(st);
    return rc;
}

int store_hour_total(store_t *s, int dev, const char *hour_key, long *counts)
{
    sqlite3_stmt *st = prep(s, "SELECT total FROM rain_hour WHERE dev_id=?1 AND hour=?2"); int rc = -1;
    if (!st) return -1;
    sqlite3_bind_int(st, 1, dev); sqlite3_bind_text(st, 2, hour_key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) { *counts = (long)sqlite3_column_int64(st, 0); rc = 0; }
    sqlite3_finalize(st);
    return rc;
}
