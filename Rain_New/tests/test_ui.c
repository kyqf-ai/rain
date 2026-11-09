#include "../src/ui_logic.h"
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static col_status_t base(const char *state)
{
    col_status_t c; memset(&c, 0, sizeof c);
    snprintf(c.state, sizeof c.state, "%s", state);
    snprintf(c.machine, sizeof c.machine, "SRY1C-2211-00001");
    c.heartbeat_ms = 1000;
    return c;
}

int main(void)
{
    ui_state_t st; ui_view_t v; col_status_t c; int64_t t = 2000;
    ui_state_init(&st);

    /* starting: grey, no balloon */
    c = base("starting");
    ui_compute(&st, &c, "COM1", 60, t, &v);
    CHECK(v.level == LV_GRAY && v.balloon == 0 && strstr(v.status, "连接"));

    /* online with data: green, tooltip carries today's total */
    c = base("online"); c.have_status = 1; c.v_static = 7014; c.v_sun = 9000; c.v_alarm = 5900; c.clock_off = 11;
    c.have_today = 1; c.today_x100 = 320; c.hour_x100 = 30; snprintf(c.last_ok, sizeof c.last_ok, "2026-11-07 14:30:12");
    snprintf(c.last_rain, sizeof c.last_rain, "2026-11-07 09:00 0.30 mm"); c.heartbeat_ms = t;
    ui_compute(&st, &c, "COM1", 60, t, &v);
    CHECK(v.level == LV_GREEN && v.balloon == 0);
    CHECK(strstr(v.tip, "3.20 mm") && strstr(v.tip, "在线"));
    CHECK(strstr(v.power, "7.01 V") && strstr(v.power, "9.00 V") && strstr(v.power, "5.90 V"));
    CHECK(strstr(v.today, "今日雨量：3.20 mm") && strstr(v.today, "本小时：0.30 mm") && strstr(v.today, "2026-11-07 09:00"));
    CHECK(strstr(v.device, "SRY1C-2211-00001") && strstr(v.device, "COM1") && strstr(v.contact, "14:30:12") && strstr(v.clock, "+11 秒"));

    /* link lost: red + exactly one warning balloon, none while it stays down */
    c = base("offline"); snprintf(c.msg, sizeof c.msg, "device not responding"); c.heartbeat_ms = t;
    ui_compute(&st, &c, "COM1", 60, t, &v);
    CHECK(v.level == LV_RED && v.balloon == 2 && strstr(v.balloon_title, "设备无响应"));
    ui_compute(&st, &c, "COM1", 60, t + 1000, &v);
    CHECK(v.level == LV_RED && v.balloon == 0);

    /* recovery: one info balloon */
    c = base("online"); c.heartbeat_ms = t + 2000;
    ui_compute(&st, &c, "COM1", 60, t + 2000, &v);
    CHECK(v.level == LV_GREEN && v.balloon == 1 && strstr(v.balloon_title, "恢复"));
    ui_compute(&st, &c, "COM1", 60, t + 3000, &v);
    CHECK(v.balloon == 0);

    /* each failure kind maps to red with its own wording */
    { const char *k[] = { "no_reply", "port_error", "machine_mismatch" }; const char *w[] = { "无应答", "串口异常", "机号不符" }; int i;
      for (i = 0; i < 3; i++) { ui_state_t s2; ui_state_init(&s2); c = base(k[i]); c.heartbeat_ms = t; ui_compute(&s2, &c, "COM1", 60, t, &v);
        CHECK(v.level == LV_RED && strstr(v.status, w[i]) && v.balloon == 2); } }

    /* frozen collector thread: heartbeat too old although the state still says online */
    { ui_state_t s2; ui_state_init(&s2); c = base("online"); c.heartbeat_ms = 1000;
      ui_compute(&s2, &c, "COM1", 60, 1000 + 200 * 1000 - 1, &v);     /* limit is 3*60+30 = 210 s */
      CHECK(v.level == LV_GREEN);
      ui_compute(&s2, &c, "COM1", 60, 1000 + 211 * 1000, &v);
      CHECK(v.level == LV_RED && strstr(v.status, "无响应") && v.balloon == 2); }

    /* low battery: yellow, balloon once, repeated only after 6 hours */
    { ui_state_t s2; ui_state_init(&s2); c = base("online"); c.have_status = 1; c.v_static = 5700; c.v_alarm = 5900; c.heartbeat_ms = 5000;
      ui_compute(&s2, &c, "COM1", 60, 5000, &v);
      CHECK(v.level == LV_YELLOW && v.balloon == 2 && strstr(v.balloon_text, "5.70 V") && strstr(v.balloon_text, "5.90 V"));
      c.heartbeat_ms = 5000 + 3600 * 1000; ui_compute(&s2, &c, "COM1", 60, 5000 + 3600 * 1000, &v); CHECK(v.level == LV_YELLOW && v.balloon == 0);
      c.heartbeat_ms = 5000 + 6 * 3600 * 1000; ui_compute(&s2, &c, "COM1", 60, 5000 + 6 * 3600 * 1000, &v); CHECK(v.balloon == 2); }

    /* clock far off: yellow, with a note */
    { ui_state_t s2; ui_state_init(&s2); c = base("online"); c.have_status = 1; c.v_static = 7000; c.v_alarm = 5900; c.clock_off = -900; c.heartbeat_ms = t;
      ui_compute(&s2, &c, "COM1", 60, t, &v); CHECK(v.level == LV_YELLOW && strstr(v.clock, "-900") && strstr(v.clock, "偏差较大")); }

    /* every string must stay valid UTF-8 and inside Windows tooltip/balloon limits */
    { ui_state_t s2; ui_state_init(&s2); c = base("machine_mismatch"); memset(c.msg, 'x', 150); c.msg[150] = 0; c.heartbeat_ms = t;
      ui_compute(&s2, &c, "COM1", 60, t, &v);
      CHECK(strlen(v.tip) < 120 && strlen(v.balloon_title) < 96 && strlen(v.balloon_text) < 200); }

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
