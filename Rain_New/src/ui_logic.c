#include "ui_logic.h"
#include <stdio.h>
#include <string.h>

#define BATT_WARN_INTERVAL_MS (6LL * 3600 * 1000)

void ui_state_init(ui_state_t *st) { memset(st, 0, sizeof *st); }

static void volts(char *out, size_t n, int mv)
{
    snprintf(out, n, "%d.%02d V", mv / 1000, (mv % 1000) / 10);
}
static void mm100(char *out, size_t n, int v)
{
    if (v < 0) v = 0;
    snprintf(out, n, "%d.%02d mm", v / 100, v % 100);
}

void ui_compute(ui_state_t *st, const col_status_t *cs, const char *port, int poll_sec, int64_t now_ms, ui_view_t *v)
{
    int online = !strcmp(cs->state, "online");
    int stale = cs->heartbeat_ms > 0 && now_ms - cs->heartbeat_ms > ((int64_t)poll_sec * 3 + 30) * 1000;
    int batt_low = cs->have_status && cs->v_alarm > 0 && cs->v_static > 0 && cs->v_static < cs->v_alarm;
    int clock_bad = cs->have_status && (cs->clock_off > 300 || cs->clock_off < -300);
    char a[32], b[32], c[32];

    memset(v, 0, sizeof *v);

    /* headline + colour */
    if (stale) {
        v->level = LV_RED; snprintf(v->status, sizeof v->status, "采集线程无响应");
        snprintf(v->detail, sizeof v->detail, "超过 %d 秒没有任何动静，请重启程序", poll_sec * 3 + 30);
    } else if (online) {
        v->level = (batt_low || clock_bad) ? LV_YELLOW : LV_GREEN;
        snprintf(v->status, sizeof v->status, "在线");
        if (batt_low) snprintf(v->detail, sizeof v->detail, "电池电压低于报警值");
        else if (clock_bad) snprintf(v->detail, sizeof v->detail, "设备时钟与电脑相差较大");
    } else if (!strcmp(cs->state, "starting")) {
        v->level = LV_GRAY; snprintf(v->status, sizeof v->status, "正在连接…");
    } else if (!strcmp(cs->state, "backfill")) {
        v->level = LV_GRAY; snprintf(v->status, sizeof v->status, "正在补齐历史数据");
        snprintf(v->detail, sizeof v->detail, "%s", cs->msg);
    } else if (!strcmp(cs->state, "stopped")) {
        v->level = LV_GRAY; snprintf(v->status, sizeof v->status, "已停止");
    } else {
        v->level = LV_RED;
        if (!strcmp(cs->state, "offline")) snprintf(v->status, sizeof v->status, "设备无响应");
        else if (!strcmp(cs->state, "no_reply")) snprintf(v->status, sizeof v->status, "设备无应答");
        else if (!strcmp(cs->state, "port_error")) snprintf(v->status, sizeof v->status, "串口异常");
        else if (!strcmp(cs->state, "machine_mismatch")) snprintf(v->status, sizeof v->status, "机号不符");
        else snprintf(v->status, sizeof v->status, "%s", cs->state);
        snprintf(v->detail, sizeof v->detail, "%s", cs->msg);
    }

    /* lines */
    if (cs->name[0] && strcmp(cs->name, cs->machine) != 0)
        snprintf(v->device, sizeof v->device, "站点：%s（%s，%s）", cs->name, cs->machine[0] ? cs->machine : "—", port ? port : "");
    else
        snprintf(v->device, sizeof v->device, "设备：%s（%s）", cs->machine[0] ? cs->machine : "—", port ? port : "");
    snprintf(v->contact, sizeof v->contact, "最近通信：%s", cs->last_ok[0] ? cs->last_ok : "—");
    if (cs->have_today) {
        mm100(a, sizeof a, cs->today_x100); mm100(b, sizeof b, cs->hour_x100);
        if (cs->last_rain[0]) snprintf(v->today, sizeof v->today, "今日雨量：%s    本小时：%s    最近有雨：%s", a, b, cs->last_rain);
        else snprintf(v->today, sizeof v->today, "今日雨量：%s    本小时：%s    最近有雨：—", a, b);
    } else snprintf(v->today, sizeof v->today, "今日雨量：—");
    if (cs->have_status) {
        volts(a, sizeof a, cs->v_static); volts(b, sizeof b, cs->v_sun); volts(c, sizeof c, cs->v_alarm);
        snprintf(v->power, sizeof v->power, "电池：%s    太阳能：%s    报警阈值：%s", a, b, c);
        snprintf(v->clock, sizeof v->clock, "设备时钟偏差：%+d 秒%s", cs->clock_off, clock_bad ? "（偏差较大）" : "");
    } else {
        snprintf(v->power, sizeof v->power, "电池：—");
        snprintf(v->clock, sizeof v->clock, "设备时钟偏差：—");
    }

    /* tray tooltip (Windows limits it to 127 UTF-16 units; stay well below) */
    if (cs->have_today) { mm100(a, sizeof a, cs->today_x100); snprintf(v->tip, sizeof v->tip, "雨量采集 - %s | 今日 %s", v->status, a); }
    else snprintf(v->tip, sizeof v->tip, "雨量采集 - %s", v->status);

    /* balloons: tell the user once when something breaks, once when it recovers */
    if (v->level == LV_RED && !st->down_notified) {
        st->down_notified = 1;
        v->balloon = 2;
        snprintf(v->balloon_title, sizeof v->balloon_title, "雨量采集：%s", v->status);
        snprintf(v->balloon_text, sizeof v->balloon_text, "%s", v->detail[0] ? v->detail : "请检查设备与串口连接");
    } else if ((v->level == LV_GREEN || v->level == LV_YELLOW) && st->down_notified) {
        st->down_notified = 0;
        v->balloon = 1;
        snprintf(v->balloon_title, sizeof v->balloon_title, "雨量采集：通信已恢复");
        snprintf(v->balloon_text, sizeof v->balloon_text, "设备重新在线");
    } else if (batt_low && online && (!st->batt_warned || now_ms - st->batt_warn_ms >= BATT_WARN_INTERVAL_MS)) {
        st->batt_warned = 1; st->batt_warn_ms = now_ms;
        v->balloon = 2;
        volts(a, sizeof a, cs->v_static); volts(b, sizeof b, cs->v_alarm);
        snprintf(v->balloon_title, sizeof v->balloon_title, "雨量采集：电池电压偏低");
        snprintf(v->balloon_text, sizeof v->balloon_text, "当前 %s，低于报警值 %s", a, b);
    }
}
