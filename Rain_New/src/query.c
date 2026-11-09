#include "query.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sqlite3_stmt *prep(store_t *s, const char *sql)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) != SQLITE_OK) { LOGE("sql prepare: %s", sqlite3_errmsg(s->db)); return NULL; }
    return st;
}
static const char *col(sqlite3_stmt *st, int i)
{
    const char *p = (const char *)sqlite3_column_text(st, i);
    return p ? p : "";
}

int q_unit(store_t *s, int dev)
{
    sqlite3_stmt *st = prep(s, "SELECT unit FROM device WHERE id=?1"); int u = 100;
    if (!st) return u;
    sqlite3_bind_int(st, 1, dev);
    if (sqlite3_step(st) == SQLITE_ROW) u = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return u > 0 ? u : 100;
}

typedef struct { int s; int total; int has_t; int t10; } hrow_t;   /* s: 0 none, 1 provisional, 2 final */

static int load_day(store_t *s, int dev, const char *date, hrow_t *r)
{
    sqlite3_stmt *st; char lo[24], hi[24]; int i;
    for (i = 0; i < 24; i++) { r[i].s = 0; r[i].total = 0; r[i].has_t = 0; r[i].t10 = 0; }
    snprintf(lo, sizeof lo, "%s 00:00:00", date); snprintf(hi, sizeof hi, "%s 23:00:00", date);
    st = prep(s, "SELECT hour,total,temp10,final FROM rain_hour WHERE dev_id=?1 AND hour>=?2 AND hour<=?3");
    if (!st) return -1;
    sqlite3_bind_int(st, 1, dev); sqlite3_bind_text(st, 2, lo, -1, SQLITE_TRANSIENT); sqlite3_bind_text(st, 3, hi, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        int h = atoi(col(st, 0) + 11);
        if (h < 0 || h > 23) continue;
        r[h].total = sqlite3_column_int(st, 1);
        r[h].has_t = sqlite3_column_type(st, 2) != SQLITE_NULL;
        r[h].t10 = r[h].has_t ? sqlite3_column_int(st, 2) : 0;
        r[h].s = sqlite3_column_int(st, 3) ? 2 : 1;
    }
    sqlite3_finalize(st);
    return 0;
}

int q_day_json(store_t *s, int dev, const char *date, sbuf_t *o)
{
    hrow_t r[24]; int i, unit = q_unit(s, dev), sum = 0, mx = 0, mxh = -1, rain_h = 0, have = 0; wtime_t now; char nows[32];
    if (load_day(s, dev, date, r) != 0) return -1;
    for (i = 0; i < 24; i++) {
        if (!r[i].s) continue;
        have++; sum += r[i].total;
        if (r[i].total > 0) rain_h++;
        if (r[i].total > mx) { mx = r[i].total; mxh = i; }
    }
    wtime_now(&now); wtime_fmt(&now, WT_FULL, nows, sizeof nows);
    sb_printf(o, "{\"date\":\"%s\",\"unit\":%d,\"now\":\"%s\",\"hours\":[", date, unit, nows);
    for (i = 0; i < 24; i++) {
        sb_printf(o, "%s{\"h\":%d,\"v\":%.2f,\"s\":%d", i ? "," : "", i, r[i].total / (double)unit, r[i].s);
        if (r[i].has_t) sb_printf(o, ",\"t\":%.1f", r[i].t10 / 10.0);
        sb_puts(o, "}");
    }
    sb_printf(o, "],\"total\":%.2f,\"max\":%.2f,\"max_h\":%d,\"rain_hours\":%d,\"hours_with_data\":%d}",
              sum / (double)unit, mx / (double)unit, mxh, rain_h, have);
    return o->err ? -1 : 0;
}

int q_minutes_json(store_t *s, int dev, const char *hour, sbuf_t *o)
{
    sqlite3_stmt *st; int m[60] = {0}, unit = q_unit(s, dev), total = 0, state = 0, i; char lo[24], hi[24], hk[24];
    snprintf(lo, sizeof lo, "%s:00", hour); snprintf(hi, sizeof hi, "%s:59", hour); snprintf(hk, sizeof hk, "%s:00:00", hour);
    st = prep(s, "SELECT final FROM rain_hour WHERE dev_id=?1 AND hour=?2");
    if (!st) return -1;
    sqlite3_bind_int(st, 1, dev); sqlite3_bind_text(st, 2, hk, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) state = sqlite3_column_int(st, 0) ? 2 : 1;
    sqlite3_finalize(st);
    st = prep(s, "SELECT ts,cnt FROM rain_min WHERE dev_id=?1 AND ts>=?2 AND ts<=?3");
    if (!st) return -1;
    sqlite3_bind_int(st, 1, dev); sqlite3_bind_text(st, 2, lo, -1, SQLITE_TRANSIENT); sqlite3_bind_text(st, 3, hi, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        int mi = atoi(col(st, 0) + 14);
        if (mi >= 0 && mi < 60) { m[mi] = sqlite3_column_int(st, 1); total += m[mi]; }
    }
    sqlite3_finalize(st);
    sb_printf(o, "{\"hour\":\"%s\",\"unit\":%d,\"s\":%d,\"total\":%.2f,\"min\":[", hour, unit, state, total / (double)unit);
    for (i = 0; i < 60; i++) sb_printf(o, "%s%.2f", i ? "," : "", m[i] / (double)unit);
    sb_puts(o, "]}");
    return o->err ? -1 : 0;
}

int q_range(store_t *s, int dev, const char *from, const char *to, const char *by, int csv, sbuf_t *o)
{
    sqlite3_stmt *st; char lo[24], hi[24]; int unit = q_unit(s, dev), len, first = 1;
    long tot = 0, nh = 0, rh = 0, days = 0, rdays = 0, maxday = 0; char maxday_t[16] = "";
    long maxh = 0; char maxh_t[24] = ""; wtime_t a, b; sbuf_t rows;
    if (strcmp(by, "hour") && strcmp(by, "day") && strcmp(by, "month")) return -1;
    len = !strcmp(by, "month") ? 7 : !strcmp(by, "day") ? 10 : 19;
    snprintf(lo, sizeof lo, "%s 00:00:00", from); snprintf(hi, sizeof hi, "%s 23:00:00", to);
    wtime_parse(from, &a); wtime_parse(to, &b);

    /* summary comes from a per-day aggregation regardless of the requested grouping */
    st = prep(s, "SELECT substr(hour,1,10),SUM(total),COUNT(*),SUM(total>0) FROM rain_hour "
                 "WHERE dev_id=?1 AND hour>=?2 AND hour<=?3 GROUP BY 1");
    if (!st) return -1;
    sqlite3_bind_int(st, 1, dev); sqlite3_bind_text(st, 2, lo, -1, SQLITE_TRANSIENT); sqlite3_bind_text(st, 3, hi, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        long d = sqlite3_column_int64(st, 1);
        days++; tot += d; nh += sqlite3_column_int(st, 2); rh += sqlite3_column_int(st, 3);
        if (d > 0) rdays++;
        if (d > maxday) { maxday = d; snprintf(maxday_t, sizeof maxday_t, "%s", col(st, 0)); }
    }
    sqlite3_finalize(st);
    st = prep(s, "SELECT hour,total FROM rain_hour WHERE dev_id=?1 AND hour>=?2 AND hour<=?3 AND total>0 ORDER BY total DESC, hour LIMIT 1");
    if (!st) return -1;
    sqlite3_bind_int(st, 1, dev); sqlite3_bind_text(st, 2, lo, -1, SQLITE_TRANSIENT); sqlite3_bind_text(st, 3, hi, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) { maxh = sqlite3_column_int64(st, 1); snprintf(maxh_t, sizeof maxh_t, "%s", col(st, 0)); }
    sqlite3_finalize(st);

    sb_init(&rows);
    st = prep(s, "SELECT substr(hour,1,?4) k,SUM(total),COUNT(*),SUM(total>0),MAX(total) FROM rain_hour "
                 "WHERE dev_id=?1 AND hour>=?2 AND hour<=?3 GROUP BY 1 ORDER BY 1");
    if (!st) { sb_free(&rows); return -1; }
    sqlite3_bind_int(st, 1, dev); sqlite3_bind_text(st, 2, lo, -1, SQLITE_TRANSIENT); sqlite3_bind_text(st, 3, hi, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 4, len);
    if (csv) sb_puts(&rows, "time,rain_mm,hours_with_data,rainy_hours,max_hour_mm\n");
    while (sqlite3_step(st) == SQLITE_ROW) {
        double v = sqlite3_column_int64(st, 1) / (double)unit, mx = sqlite3_column_int64(st, 4) / (double)unit;
        if (csv) sb_printf(&rows, "%s,%.2f,%d,%d,%.2f\n", col(st, 0), v, sqlite3_column_int(st, 2), sqlite3_column_int(st, 3), mx);
        else {
            sb_printf(&rows, "%s{\"t\":", first ? "" : ",");
            sb_json_str(&rows, col(st, 0));
            sb_printf(&rows, ",\"v\":%.2f,\"n\":%d,\"rainy\":%d,\"max\":%.2f}", v, sqlite3_column_int(st, 2), sqlite3_column_int(st, 3), mx);
        }
        first = 0;
    }
    sqlite3_finalize(st);
    if (csv) sb_puts(o, rows.p);
    else {
        long span = (long)((wtime_epoch(&b) - wtime_epoch(&a)) / 86400) + 1;
        sb_printf(o, "{\"from\":\"%s\",\"to\":\"%s\",\"by\":\"%s\",\"unit\":%d,\"summary\":{\"total\":%.2f,\"rain_hours\":%ld,"
                     "\"hours_with_data\":%ld,\"hours_expected\":%ld,\"days_with_data\":%ld,\"rainy_days\":%ld,"
                     "\"max_hour\":{\"t\":\"%s\",\"v\":%.2f},\"max_day\":{\"t\":\"%s\",\"v\":%.2f}},\"rows\":[",
                  from, to, by, unit, tot / (double)unit, rh, nh, span * 24, days, rdays,
                  maxh_t, maxh / (double)unit, maxday_t, maxday / (double)unit);
        sb_puts(o, rows.p); sb_puts(o, "]}");
    }
    sb_free(&rows);
    return o->err ? -1 : 0;
}

int q_status_json(store_t *s, int dev, sbuf_t *o)
{
    sqlite3_stmt *st; wtime_t now; char nows[32], lastfinal[24] = "", lasthour[24] = ""; int found = 0;
    wtime_now(&now); wtime_fmt(&now, WT_FULL, nows, sizeof nows);
    st = prep(s, "SELECT d.machine,d.name,d.unit,s.state,s.msg,s.updated,s.last_ok,s.dev_time,s.clock_off,s.v_static,s.v_dyn,"
                 "s.v_sun,s.v_alarm,s.v_hang,s.temp_c,s.flags FROM device d LEFT JOIN device_state s ON s.dev_id=d.id WHERE d.id=?1");
    if (!st) return -1;
    sqlite3_bind_int(st, 1, dev);
    if (sqlite3_step(st) == SQLITE_ROW) {
        found = 1;
        sb_printf(o, "{\"now\":\"%s\",\"machine\":", nows); sb_json_str(o, col(st, 0));
        sb_puts(o, ",\"name\":"); sb_json_str(o, col(st, 1));
        sb_printf(o, ",\"unit\":%d,\"state\":", sqlite3_column_int(st, 2)); sb_json_str(o, sqlite3_column_type(st, 3) == SQLITE_NULL ? "unknown" : col(st, 3));
        sb_puts(o, ",\"msg\":"); sb_json_str(o, col(st, 4));
        sb_puts(o, ",\"updated\":"); sb_json_str(o, col(st, 5));
        sb_puts(o, ",\"last_ok\":"); sb_json_str(o, col(st, 6));
        sb_puts(o, ",\"dev_time\":"); sb_json_str(o, col(st, 7));
        sb_printf(o, ",\"clock_off\":%d,\"v_static\":%d,\"v_dyn\":%d,\"v_sun\":%d,\"v_alarm\":%d,\"v_hang\":%d,\"temp_c\":%d,\"flags\":",
                  sqlite3_column_int(st, 8), sqlite3_column_int(st, 9), sqlite3_column_int(st, 10), sqlite3_column_int(st, 11),
                  sqlite3_column_int(st, 12), sqlite3_column_int(st, 13), sqlite3_column_int(st, 14));
        sb_json_str(o, col(st, 15));
    }
    sqlite3_finalize(st);
    if (!found) { sb_puts(o, "{\"error\":\"no device yet\"}"); return 0; }
    st = prep(s, "SELECT MAX(CASE WHEN final=1 THEN hour END), MAX(hour) FROM rain_hour WHERE dev_id=?1");
    if (st) {
        sqlite3_bind_int(st, 1, dev);
        if (sqlite3_step(st) == SQLITE_ROW) { snprintf(lastfinal, sizeof lastfinal, "%s", col(st, 0)); snprintf(lasthour, sizeof lasthour, "%s", col(st, 1)); }
        sqlite3_finalize(st);
    }
    sb_puts(o, ",\"last_final_hour\":"); sb_json_str(o, lastfinal);
    sb_puts(o, ",\"last_hour\":"); sb_json_str(o, lasthour);
    sb_puts(o, "}");
    return o->err ? -1 : 0;
}

static long sum_prefix(store_t *s, int dev, const char *prefix, int plen)
{
    sqlite3_stmt *st; long v = 0;
    st = prep(s, "SELECT COALESCE(SUM(total),0) FROM rain_hour WHERE dev_id=?1 AND substr(hour,1,?2)=?3");
    if (!st) return 0;
    sqlite3_bind_int(st, 1, dev); sqlite3_bind_int(st, 2, plen); sqlite3_bind_text(st, 3, prefix, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) v = (long)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

int q_summary_json(store_t *s, int dev, sbuf_t *o)
{
    int unit = q_unit(s, dev), j, dry = 0, have;
    wtime_t now, t; char nows[32], today[16], monp[8], yrp[6], key[24], mk[24], s60[24], ev_start[24] = "";
    char rowkey[24] = ""; int rowtot = 0; long slid = 0, ev = 0, mon, yr; int64_t nowe, he;
    sqlite3_stmt *st;

    wtime_now(&now);
    wtime_fmt(&now, WT_FULL, nows, sizeof nows);
    wtime_fmt(&now, WT_DATE, today, sizeof today);
    snprintf(monp, sizeof monp, "%.7s", today);
    snprintf(yrp, sizeof yrp, "%.4s", today);

    /* sliding hour: the last 60 minutes from the per-minute table */
    nowe = wtime_epoch(&now);
    wtime_from_epoch(nowe - 59 * 60, &t);
    wtime_fmt(&t, WT_MINUTE, s60, sizeof s60);
    wtime_fmt(&now, WT_MINUTE, mk, sizeof mk);
    st = prep(s, "SELECT COALESCE(SUM(cnt),0) FROM rain_min WHERE dev_id=?1 AND ts>=?2 AND ts<=?3");
    if (st) {
        sqlite3_bind_int(st, 1, dev); sqlite3_bind_text(st, 2, s60, -1, SQLITE_TRANSIENT); sqlite3_bind_text(st, 3, mk, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) slid = (long)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }

    /* this rain event: walk hourly totals back until 6 dry hours (missing hours count as dry) */
    wtime_hour_floor(&now);
    he = wtime_epoch(&now);
    wtime_from_epoch(he - 719 * 3600LL, &t);
    wtime_fmt(&t, WT_HOUR, key, sizeof key);
    st = prep(s, "SELECT hour,total FROM rain_hour WHERE dev_id=?1 AND hour>=?2 AND hour<=?3 ORDER BY hour DESC");
    if (st) {
        sqlite3_bind_int(st, 1, dev); sqlite3_bind_text(st, 2, key, -1, SQLITE_TRANSIENT);
        wtime_from_epoch(he, &t); wtime_fmt(&t, WT_HOUR, key, sizeof key);
        sqlite3_bind_text(st, 3, key, -1, SQLITE_TRANSIENT);
        have = sqlite3_step(st) == SQLITE_ROW;
        if (have) { snprintf(rowkey, sizeof rowkey, "%s", col(st, 0)); rowtot = sqlite3_column_int(st, 1); }
        for (j = 0; j < 720; j++) {
            int total = 0;
            wtime_from_epoch(he - (int64_t)j * 3600, &t);
            wtime_fmt(&t, WT_HOUR, key, sizeof key);
            if (have && !strcmp(rowkey, key)) {
                total = rowtot;
                have = sqlite3_step(st) == SQLITE_ROW;
                if (have) { snprintf(rowkey, sizeof rowkey, "%s", col(st, 0)); rowtot = sqlite3_column_int(st, 1); }
            }
            if (total > 0) { ev += total; dry = 0; snprintf(ev_start, sizeof ev_start, "%s", key); }
            else if (++dry >= 6) break;
        }
        sqlite3_finalize(st);
    }
    mon = sum_prefix(s, dev, monp, 7);
    yr = sum_prefix(s, dev, yrp, 4);

    sb_printf(o, "{\"now\":\"%s\",\"date\":\"%s\",\"unit\":%d,\"sliding_hour\":%.2f,\"day_total\":%.2f,"
                 "\"event_total\":%.2f,\"month_total\":%.2f,\"year_total\":%.2f,\"event_start\":",
              nows, today, unit, slid / (double)unit, sum_prefix(s, dev, today, 10) / (double)unit,
              ev / (double)unit, mon / (double)unit, yr / (double)unit);
    sb_json_str(o, ev_start);
    sb_puts(o, "}");
    return o->err ? -1 : 0;
}

int q_alarms_json(store_t *s, int dev, sbuf_t *o)
{
    sqlite3_stmt *st; int alarm = 0, first = 1, n = 0; char state[32] = "", upd[32] = "";
    st = prep(s, "SELECT COALESCE(state,''),COALESCE(updated,''),COALESCE(v_alarm,0) FROM device_state WHERE dev_id=?1");
    if (st) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            snprintf(state, sizeof state, "%s", col(st, 0));
            snprintf(upd, sizeof upd, "%s", col(st, 1));
            alarm = sqlite3_column_int(st, 2);
        }
        sqlite3_finalize(st);
    }
    sb_printf(o, "{\"threshold\":%d,\"state\":", alarm);
    sb_json_str(o, state);
    sb_puts(o, ",\"updated\":"); sb_json_str(o, upd);
    sb_puts(o, ",\"rows\":[");
    st = prep(s, "SELECT ts,clock_off,COALESCE(v_static,0),COALESCE(v_dyn,0) FROM device_status "
                 "WHERE dev_id=?1 ORDER BY ts DESC LIMIT 3000");
    if (st) {
        sqlite3_bind_int(st, 1, dev);
        while (sqlite3_step(st) == SQLITE_ROW && n < 500) {
            int co = sqlite3_column_int(st, 1), vs = sqlite3_column_int(st, 2), vd = sqlite3_column_int(st, 3);
            const char *ts = col(st, 0); char msg[160]; const char *kind;
            if (abs(co) > 300) {
                kind = "clock";
                snprintf(msg, sizeof msg, "设备时钟与电脑相差 %+d 秒", co);
            } else if (alarm > 0 && vs > 0 && vs < alarm) {
                kind = "voltage";
                snprintf(msg, sizeof msg, "电池电压 %d.%03d V 低于报警值 %d.%03d V", vs / 1000, vs % 1000, alarm / 1000, alarm % 1000);
            } else if (alarm > 0 && vd > 0 && vd < alarm) {
                kind = "voltage";
                snprintf(msg, sizeof msg, "电池电压（动态）%d.%03d V 低于报警值 %d.%03d V", vd / 1000, vd % 1000, alarm / 1000, alarm % 1000);
            } else continue;
            sb_puts(o, first ? "{\"t\":" : ",{\"t\":"); first = 0;
            sb_json_str(o, ts);
            sb_printf(o, ",\"kind\":\"%s\",\"detail\":", kind);
            sb_json_str(o, msg);
            sb_puts(o, "}");
            n++;
        }
        sqlite3_finalize(st);
    }
    sb_puts(o, "]}");
    return o->err ? -1 : 0;
}

int q_day_text(store_t *s, int dev, const char *date, sbuf_t *o)
{
    hrow_t r[24]; int i, unit = q_unit(s, dev), sum = 0, k, bar; double cum = 0;
    if (load_day(s, dev, date, r) != 0) return -1;
    sb_printf(o, "Date %s   (unit: %d counts/mm)\n", date, unit);
    sb_puts(o, "Hour   mm      Cum mm  State        Chart\n");
    for (i = 0; i < 24; i++) {
        double v = r[i].total / (double)unit; cum += v; sum += r[i].total;
        bar = (int)(v * 4 + 0.5); if (bar > 40) bar = 40;
        sb_printf(o, "%02d:00  %-6.2f  %-6.2f  %-11s  ", i, v, cum, r[i].s == 2 ? "final" : r[i].s == 1 ? "provisional" : "-");
        for (k = 0; k < bar; k++) sb_puts(o, "#");
        sb_puts(o, "\n");
    }
    sb_printf(o, "Total  %.2f mm\n", sum / (double)unit);
    return o->err ? -1 : 0;
}
