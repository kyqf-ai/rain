/* Drives the collector exactly the way the GUI does: thread start/stop/restart, status polling, poke, failures.
   usage: test_ctl <pty-of-fake-device> <db-file> */
#include "../src/collector.h"
#include "../src/ui_logic.h"
#include "../src/log.h"
#include "../src/platform.h"
#include "../src/util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static int wait_state(const char *state, int ms, col_status_t *out)
{
    int64_t end = plat_mono_ms() + ms;
    while (plat_mono_ms() < end) {
        collector_get_status(out);
        if (!strcmp(out->state, state)) return 1;
        plat_sleep_ms(50);
    }
    collector_get_status(out);
    return 0;
}
static plat_thread_t *start(cfg_t *cfg) { plat_clear_stop(); collector_reset_status(); return plat_thread_start(collector_thread, cfg); }
static void stop(plat_thread_t *t) { plat_request_stop(); plat_thread_join(t); }

int main(int argc, char **argv)
{
    cfg_t cfg; col_status_t cs; ui_state_t us; ui_view_t v; plat_thread_t *t; unsigned long c0, cur = 0; char line[300]; int found = 0;
    if (argc < 3) { fprintf(stderr, "usage\n"); return 2; }
    plat_init(); log_init("", 0, 0);
    cfg_defaults(&cfg);
    snprintf(cfg.port, sizeof cfg.port, "%s", argv[1]); snprintf(cfg.db, sizeof cfg.db, "%s", argv[2]);
    cfg.poll_sec = 60; cfg.backfill_hours = 6; cfg.timeout_ms = 500; cfg.retries = 2; cfg.reconnect_sec = 1;   /* poll 60 s: only poke can make it fast */
    ui_state_init(&us);

    /* 1. start -> online, status published */
    t = start(&cfg);
    CHECK(t != NULL);
    collector_get_status(&cs);
    CHECK(!strcmp(cs.state, "starting"));
    CHECK(wait_state("online", 20000, &cs));
    CHECK(!strcmp(cs.machine, "SRY1C-2211-00001") && cs.last_ok[0] && cs.have_status && cs.have_today);
    CHECK(cs.v_static == 7014 && cs.v_sun == 9000 && cs.v_alarm == 5900);
    CHECK(cs.clock_off >= 5 && cs.clock_off <= 17);
    CHECK(cs.cycles >= 1 && cs.frames_ok >= 5);
    CHECK(cs.today_x100 >= 0 && cs.hour_x100 >= 0);
    ui_compute(&us, &cs, cfg.port, cfg.poll_sec, plat_mono_ms(), &v);
    CHECK(v.level == LV_GREEN && strstr(v.tip, "在线"));

    /* 2. log ring received the collector's messages */
    while (log_ring_next(&cur, line, sizeof line)) if (strstr(line, "connected to SRY1C-2211-00001")) found = 1;
    CHECK(found);

    /* 3. "Sync now": a poke must start a cycle long before the 60 s poll interval */
    c0 = cs.cycles; collector_poke();
    { int64_t end = plat_mono_ms() + 6000; while (plat_mono_ms() < end) { collector_get_status(&cs); if (cs.cycles > c0) break; plat_sleep_ms(50); } }
    CHECK(cs.cycles > c0);

    /* 4. heartbeat keeps moving while the thread idles between cycles */
    collector_get_status(&cs); { int64_t h0 = cs.heartbeat_ms; plat_sleep_ms(2500); collector_get_status(&cs); CHECK(cs.heartbeat_ms > h0); }

    /* 5. stop: thread ends quickly even though it is in the middle of a 60 s wait */
    { int64_t t0 = plat_mono_ms(); stop(t); CHECK(plat_mono_ms() - t0 < 3000); }
    collector_get_status(&cs); CHECK(!strcmp(cs.state, "stopped"));

    /* 6. restart (the GUI's "Reload") */
    t = start(&cfg);
    CHECK(wait_state("online", 20000, &cs));
    stop(t);

    /* 7. serial port missing -> port_error, red, one balloon */
    { cfg_t bad = cfg; ui_state_t s2; ui_state_init(&s2); snprintf(bad.port, sizeof bad.port, "/dev/does-not-exist");
      t = start(&bad);
      CHECK(wait_state("port_error", 8000, &cs));
      ui_compute(&s2, &cs, bad.port, bad.poll_sec, plat_mono_ms(), &v);
      CHECK(v.level == LV_RED && v.balloon == 2 && strstr(v.status, "串口"));
      stop(t); }

    /* 8. wrong machine id -> refused, red */
    { cfg_t bad = cfg; ui_state_t s2; ui_state_init(&s2); snprintf(bad.machine, sizeof bad.machine, "OTHER-0000");
      t = start(&bad);
      CHECK(wait_state("machine_mismatch", 8000, &cs));
      ui_compute(&s2, &cs, bad.port, bad.poll_sec, plat_mono_ms(), &v);
      CHECK(v.level == LV_RED && strstr(v.status, "机号不符"));
      stop(t); }

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
