#include "../src/proto.h"
#include "../src/crc16.h"
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static size_t hx(const char *s, uint8_t *o)
{
    size_t n = 0; unsigned v;
    while (*s) { if (*s == ' ') { s++; continue; } sscanf(s, "%2x", &v); o[n++] = (uint8_t)v; s += 2; }
    return n;
}
static size_t mk_aa05(uint8_t *o, int mo, int d, int h, const uint16_t *m, int be, uint16_t temp)
{
    size_t n = 0, i; uint16_t c;
    o[n++] = 0xAA; o[n++] = 0x05; o[n++] = 26; o[n++] = (uint8_t)mo; o[n++] = (uint8_t)d; o[n++] = (uint8_t)h;
    for (i = 0; i < 60; i++) {
        if (be) { o[n++] = (uint8_t)(m[i] >> 8); o[n++] = (uint8_t)m[i]; }
        else    { o[n++] = (uint8_t)m[i]; o[n++] = (uint8_t)(m[i] >> 8); }
    }
    o[n++] = (uint8_t)temp; o[n++] = (uint8_t)(temp >> 8);
    c = crc16_kermit(o, n); o[n++] = (uint8_t)c; o[n++] = (uint8_t)(c >> 8);
    return n;
}

int main(void)
{
    uint8_t b[512], t[64]; size_t n, used; frame_t f; wtime_t tm; int wd; char id[40];
    status_t st; hourdata_t hd;

    /* --- commands captured from the real device session --- */
    n = proto_build(t, 0x01, 0x01, NULL, 0);            hx("55 06 01 01 C9 65", b);    CHECK(n == 6 && !memcmp(t, b, 6));
    n = proto_build(t, 0x03, 0x01, NULL, 0);            hx("55 06 03 01 79 56", b);    CHECK(n == 6 && !memcmp(t, b, 6));
    n = proto_build(t, 0x06, 0x01, NULL, 0);            hx("55 06 06 01 C1 28", b);    CHECK(n == 6 && !memcmp(t, b, 6));
    { uint8_t a[5] = {26, 11, 7, 13, 1};
      n = proto_build(t, 0x00, 0x01, a, 5);             hx("55 0B 00 01 1A 0B 07 0D 01 A1 DD", b); CHECK(n == 11 && !memcmp(t, b, 11)); }

    /* --- responses captured from the real device --- */
    n = hx("AA 01 53 52 59 31 43 2D 32 32 31 31 2D 30 30 30 30 31 7C B8", b);
    CHECK(proto_next(b, n, &f, &used, 0) == PS_FRAME && used == 20 && f.type == F_ID);
    CHECK(dec_id(&f, id, sizeof id) == 0 && !strcmp(id, "SRY1C-2211-00001"));

    n = hx("AA 03 1A 0B 07 0E 1D 2F 06 0C 0D", b);
    CHECK(proto_next(b, n, &f, &used, 0) == PS_FRAME && f.type == F_TIME);
    CHECK(dec_time(&f, &tm, &wd) == 0 && tm.y == 2026 && tm.mo == 11 && tm.d == 7 && tm.h == 14 && tm.mi == 29 && tm.s == 47 && wd == 6);

    n = hx("AA 06 1A 0B 07 0E 1D 32 06 00 00 00 00 66 1B 66 1B 28 23 0C 17 A8 16 16 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 84 2F", b);
    CHECK(proto_next(b, n, &f, &used, 0) == PS_FRAME && used == 52 && f.type == F_STATUS);
    CHECK(dec_status(&f, &st) == 0);
    CHECK(st.v_static == 7014 && st.v_dyn == 7014 && st.v_sun == 9000 && st.v_alarm == 5900 && st.v_hang == 5800 && st.temp_c == 22 && st.wday == 6);

    /* AA00 + AA09 glued together, as the device really sends them */
    n = hx("AA 00 1A 0B 07 0D 00 00 CD DF AA 09 00 03 16 D0", b);
    CHECK(proto_next(b, n, &f, &used, 0) == PS_FRAME && used == 10 && f.type == F_DATA0);
    CHECK(dec_hour(&f, &hd, EN_AUTO, 1000) == 0 && hd.hour.h == 13 && hd.total == 0 && hd.temp_valid && hd.temp10 == 0);
    CHECK(proto_next(b + 10, n - 10, &f, &used, 0) == PS_FRAME && used == 6 && f.type == F_END);

    /* junk before the frame, and a frame delivered in two pieces */
    n = hx("00 11 22 AA 09 00 03 16 D0", b);
    CHECK(proto_next(b, n, &f, &used, 0) == PS_JUNK && used == 3);
    CHECK(proto_next(b + 3, 3, &f, &used, 0) == PS_NEED_MORE);
    CHECK(proto_next(b + 3, 6, &f, &used, 0) == PS_FRAME);

    /* corrupted CRC: not accepted, and cleared once the line is idle */
    n = hx("AA 03 1A 0B 07 0E 1D 2F 06 0C 0E", b);
    CHECK(proto_next(b, n, &f, &used, 0) == PS_NEED_MORE);
    CHECK(proto_next(b, n, &f, &used, 1) == PS_BADCRC && used == n);

    /* AA05: rain minutes, both byte orders, auto detection */
    { uint16_t m[60] = {0}; uint8_t fr[200];
      m[2] = 10; m[3] = 20; m[40] = 10;
      n = mk_aa05(fr, 11, 5, 20, m, 0, 35);
      CHECK(n == 130 && proto_next(fr, n, &f, &used, 0) == PS_FRAME && f.type == F_DATA5);
      CHECK(dec_hour(&f, &hd, EN_AUTO, 1000) == 0 && hd.used == EN_LE && hd.total == 40 && hd.min[2] == 10 && hd.min[3] == 20 && hd.temp10 == 35 && !hd.suspicious);
      n = mk_aa05(fr, 11, 5, 20, m, 1, 0xFFFF);
      CHECK(proto_next(fr, n, &f, &used, 0) == PS_FRAME);
      CHECK(dec_hour(&f, &hd, EN_AUTO, 1000) == 0 && hd.used == EN_BE && hd.total == 40 && hd.min[40] == 10 && !hd.temp_valid);
      CHECK(dec_hour(&f, &hd, EN_LE, 1000) == 0 && hd.suspicious);   /* forced wrong order is flagged */
      n = mk_aa05(fr, 11, 5, 20, m, 0, 0x8000 | 25);
      proto_next(fr, n, &f, &used, 0);
      CHECK(dec_hour(&f, &hd, EN_AUTO, 1000) == 0 && hd.temp10 == -25);
    }

    /* time helpers */
    { wtime_t a; wtime_parse("2026-12-31 23:00:00", &a); wtime_add_sec(&a, 3600);
      CHECK(a.y == 2027 && a.mo == 1 && a.d == 1 && a.h == 0);
      wtime_parse("2028-02-28 12:00", &a); wtime_add_sec(&a, 86400);
      CHECK(a.mo == 2 && a.d == 29);
      CHECK(date_ok("2026-11-07") && !date_ok("2026-13-07") && !date_ok("2026-11-7") && !date_ok("2026-02-30")); }

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
