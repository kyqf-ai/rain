#include "../src/config.h"
#include "../src/log.h"
#include "../src/platform.h"
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main(void)
{
    cfg_t c; FILE *f;
    plat_init();
    log_init("", 0, 0);
    cfg_defaults(&c);
    CHECK(cfg_load(&c, "rain.ini.sample") == 0);            /* the shipped sample must parse cleanly */
    CHECK(!strcmp(c.port, "COM1") && c.baud == 9600 && c.unit == 100 && c.poll_sec == 60);
    CHECK(c.machine[0] == 0 && !strcmp(c.endian, "auto") && cfg_endian(&c) == EN_AUTO);
    CHECK(!strcmp(c.web_bind, "127.0.0.1") && c.web_port == 9099 && !strcmp(c.db, "rain.db") && c.trace == 0);
    f = fopen("test_cfg.ini", "wb");
    fputs("\xEF\xBB\xBF[Serial]\r\nPort = /dev/ttyUSB0   ; usb adapter\r\n[Device]\r\nmachine=A-1\r\nname=Rain #3\r\n[collect]\r\nendian=BE\r\npoll_sec=1\r\n", f);
    fclose(f);
    cfg_defaults(&c);
    CHECK(cfg_load(&c, "test_cfg.ini") == 0);
    CHECK(!strcmp(c.port, "/dev/ttyUSB0"));                 /* BOM, CRLF, case, spaces, inline comment */
    CHECK(!strcmp(c.machine, "A-1") && !strcmp(c.name, "Rain #3"));   /* '#' inside a value is kept */
    CHECK(c.poll_sec == 5);                                  /* clamped to the minimum */
    CHECK(cfg_load(&c, "does-not-exist.ini") == -1);
    /* first-run file written by the GUI parses back to the defaults */
    { cfg_t d, g; cfg_defaults(&d); cfg_defaults(&g);
      CHECK(cfg_write_default("test_gen.ini") == 0 && cfg_load(&g, "test_gen.ini") == 0);
      CHECK(!strcmp(g.port, d.port) && g.baud == d.baud && g.unit == d.unit && g.poll_sec == d.poll_sec && g.backfill_hours == d.backfill_hours
            && g.max_fetch == d.max_fetch && g.retries == d.retries && g.timeout_ms == d.timeout_ms && !strcmp(g.endian, d.endian)
            && !strcmp(g.db, d.db) && !strcmp(g.logdir, d.logdir) && g.machine[0] == 0 && g.trace == 0);
      remove("test_gen.ini"); remove("test_cfg.ini"); }
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
