#include "../src/store.h"
#include "../src/query.h"
#include "../src/log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/platform.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define HAS(sb, sub) do { checks++; if (!strstr((sb).p, (sub))) { fails++; printf("FAIL line %d: missing %s\n   in %s\n", __LINE__, sub, (sb).p); } } while (0)

static hourdata_t mk(int d, int h, int a, int b, int temp_valid)
{
    hourdata_t x; memset(&x, 0, sizeof x);
    x.hour.y = 2026; x.hour.mo = 11; x.hour.d = d; x.hour.h = h;
    x.min[2] = (uint16_t)a; x.min[30] = (uint16_t)b; x.total = a + b;
    x.temp_valid = temp_valid; x.temp10 = 25;
    return x;
}

int main(void)
{
    char err[200], last[32]; store_t *s; int dev; sbuf_t o; hourdata_t h;
    const char *path = "test_store.db";
    plat_init();
    remove(path); remove("test_store.db-wal"); remove("test_store.db-shm");
    log_init("", 0, 0);

    s = store_open(path, err, sizeof err);
    CHECK(s != NULL);
    dev = store_device(s, "TEST-0001", "unit test", 100);
    CHECK(dev == 1 && store_device(s, "TEST-0001", "x", 100) == 1 && store_default_device(s) == 1);
    CHECK(store_last_final_hour(s, dev, last, sizeof last) == -1);

    /* provisional first, then final, then a late provisional must not downgrade */
    h = mk(7, 8, 10, 0, 0);   CHECK(store_save_hour(s, dev, &h, 0, "2026-11-07 08:30:00") == 0);
    sb_init(&o); q_day_json(s, dev, "2026-11-07", &o);
    HAS(o, "{\"h\":8,\"v\":0.10,\"s\":1}"); sb_free(&o);
    h = mk(7, 8, 10, 20, 1);  CHECK(store_save_hour(s, dev, &h, 1, "2026-11-07 09:00:10") == 0);
    h = mk(7, 8, 99, 0, 0);   CHECK(store_save_hour(s, dev, &h, 0, "2026-11-07 09:30:00") == 0);
    sb_init(&o); q_day_json(s, dev, "2026-11-07", &o);
    HAS(o, "{\"h\":8,\"v\":0.30,\"s\":2,\"t\":2.5}");
    HAS(o, "\"total\":0.30"); HAS(o, "\"max_h\":8"); HAS(o, "\"rain_hours\":1"); HAS(o, "\"hours_with_data\":1");
    sb_free(&o);
    CHECK(store_last_final_hour(s, dev, last, sizeof last) == 0 && !strcmp(last, "2026-11-07 08:00:00"));

    /* minute detail follows the hour, empty hours stored as zero rain */
    sb_init(&o); q_minutes_json(s, dev, "2026-11-07 08", &o);
    HAS(o, "\"s\":2"); HAS(o, "\"total\":0.30"); HAS(o, "0.10,0.00"); HAS(o, "0.20"); sb_free(&o);
    h = mk(7, 9, 0, 0, 1); store_save_hour(s, dev, &h, 1, "2026-11-07 10:00:05");
    h = mk(8, 0, 50, 0, 1); store_save_hour(s, dev, &h, 1, "2026-11-08 01:00:05");
    h = mk(9, 3, 30, 10, 1); store_save_hour(s, dev, &h, 0, "2026-11-09 03:30:00");

    /* range statistics */
    sb_init(&o); CHECK(q_range(s, dev, "2026-11-07", "2026-11-09", "day", 0, &o) == 0);
    HAS(o, "\"total\":1.20"); HAS(o, "\"rain_hours\":3"); HAS(o, "\"hours_with_data\":4"); HAS(o, "\"hours_expected\":72");
    HAS(o, "\"rainy_days\":3"); HAS(o, "\"max_hour\":{\"t\":\"2026-11-08 00:00:00\",\"v\":0.50}");
    HAS(o, "\"max_day\":{\"t\":\"2026-11-08\",\"v\":0.50}");
    HAS(o, "{\"t\":\"2026-11-07\",\"v\":0.30,\"n\":2,\"rainy\":1,\"max\":0.30}"); sb_free(&o);
    sb_init(&o); CHECK(q_range(s, dev, "2026-11-01", "2026-11-30", "month", 0, &o) == 0);
    HAS(o, "{\"t\":\"2026-11\",\"v\":1.20,\"n\":4"); sb_free(&o);
    sb_init(&o); CHECK(q_range(s, dev, "2026-11-07", "2026-11-07", "hour", 1, &o) == 0);
    HAS(o, "time,rain_mm"); HAS(o, "2026-11-07 08:00:00,0.30,1,1,0.30"); sb_free(&o);
    sb_init(&o); CHECK(q_range(s, dev, "2026-11-07", "2026-11-09", "bogus", 0, &o) == -1); sb_free(&o);

    /* state + status */
    store_set_state(s, dev, "online", "ok", "2026-11-07 10:00:00", 1);
    { status_t st; memset(&st, 0, sizeof st); st.t.y = 2026; st.t.mo = 11; st.t.d = 7; st.t.h = 10; st.v_static = 7014; st.v_sun = 9000; st.temp_c = 22;
      CHECK(store_put_status(s, dev, &st, 11, "2026-11-07 10:00:00", 1) == 0); }
    sb_init(&o); q_status_json(s, dev, &o);
    HAS(o, "\"state\":\"online\""); HAS(o, "\"v_static\":7014"); HAS(o, "\"clock_off\":11"); HAS(o, "\"last_final_hour\":\"2026-11-08 00:00:00\""); sb_free(&o);

    sb_init(&o); q_day_text(s, dev, "2026-11-07", &o); HAS(o, "08:00  0.30"); HAS(o, "Total  0.30 mm"); sb_free(&o);

    store_close(s);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
