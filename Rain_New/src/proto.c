#include "proto.h"
#include "crc16.h"
#include <string.h>

size_t proto_build(uint8_t *out, uint8_t code, uint8_t sub, const uint8_t *args, size_t nargs)
{
    size_t n = 0; uint16_t c;
    out[n++] = 0x55;
    out[n++] = (uint8_t)(4 + nargs + 2);
    out[n++] = code;
    out[n++] = sub;
    if (nargs) { memcpy(out + n, args, nargs); n += nargs; }
    c = crc16_kermit(out, n);
    out[n++] = (uint8_t)(c & 0xFF);
    out[n++] = (uint8_t)(c >> 8);
    return n;
}

static size_t expected_len(uint8_t type)
{
    switch (type) {
    case F_DATA0: return 10;
    case F_TIME: return 11;
    case F_DATA5: return 130;
    case F_STATUS: return 52;
    case F_END: return 6;
    case F_ID: return 20;       /* 16 char id; other lengths resolved by CRC scan */
    default: return 0;
    }
}
static int crc_ok(const uint8_t *p, size_t n)
{
    uint16_t c;
    if (n < 4) return 0;
    c = crc16_kermit(p, n - 2);
    return p[n - 2] == (uint8_t)(c & 0xFF) && p[n - 1] == (uint8_t)(c >> 8);
}
static ps_t take(const uint8_t *b, size_t l, frame_t *fr, size_t *used)
{
    fr->type = b[1]; fr->len = l; memcpy(fr->raw, b, l); *used = l;
    return PS_FRAME;
}

ps_t proto_next(const uint8_t *b, size_t n, frame_t *fr, size_t *used, int quiet)
{
    size_t i = 0, l, maxl, L;
    *used = 0;
    while (i < n && b[i] != 0xAA) i++;
    if (i > 0) { *used = i; return PS_JUNK; }
    if (n < 2) return PS_NEED_MORE;
    L = expected_len(b[1]);
    if (L) {
        if (n >= L && crc_ok(b, L)) return take(b, L, fr, used);
        if (n < L && !quiet) return PS_NEED_MORE;
        if (!quiet) return PS_NEED_MORE;        /* wait: maybe a different length */
    } else if (!quiet) {
        return PS_NEED_MORE;
    }
    /* line idle: look for any length with a valid CRC */
    maxl = n < PROTO_MAX_FRAME ? n : PROTO_MAX_FRAME;
    for (l = 6; l <= maxl; l++)
        if (crc_ok(b, l)) return take(b, l, fr, used);
    /* nothing valid: drop up to the next 0xAA */
    for (i = 1; i < n && b[i] != 0xAA; i++) {}
    *used = i;
    return PS_BADCRC;
}

static int u16le(const uint8_t *p) { return p[0] | (p[1] << 8); }

int dec_id(const frame_t *f, char *out, size_t outsz)
{
    size_t i, n;
    if (f->type != F_ID || f->len < 5 || outsz < 2) return -1;
    n = f->len - 4;
    if (n >= outsz) n = outsz - 1;
    for (i = 0; i < n; i++) {
        uint8_t c = f->raw[2 + i];
        if (c < 0x20 || c > 0x7E) return -1;
        out[i] = (char)c;
    }
    out[n] = 0;
    return 0;
}

int dec_time(const frame_t *f, wtime_t *t, int *wday)
{
    if (f->type != F_TIME || f->len != 11) return -1;
    t->y = 2000 + f->raw[2]; t->mo = f->raw[3]; t->d = f->raw[4];
    t->h = f->raw[5]; t->mi = f->raw[6]; t->s = f->raw[7];
    if (wday) *wday = f->raw[8];
    return wtime_valid(t) ? 0 : -1;
}

int dec_status(const frame_t *f, status_t *s)
{
    if (f->type != F_STATUS || f->len != 52) return -1;
    memset(s, 0, sizeof *s);
    s->t.y = 2000 + f->raw[2]; s->t.mo = f->raw[3]; s->t.d = f->raw[4];
    s->t.h = f->raw[5]; s->t.mi = f->raw[6]; s->t.s = f->raw[7];
    s->wday = f->raw[8];
    memcpy(s->flags, f->raw + 9, 4);
    s->v_static = u16le(f->raw + 13);
    s->v_dyn    = u16le(f->raw + 15);
    s->v_sun    = u16le(f->raw + 17);
    s->v_alarm  = u16le(f->raw + 19);
    s->v_hang   = u16le(f->raw + 21);
    s->temp_c   = f->raw[23];
    hex_str(f->raw, f->len, s->raw_hex, sizeof s->raw_hex);
    return wtime_valid(&s->t) ? 0 : -1;
}

static int fill_minutes(const uint8_t *p, int be, uint16_t *m, int *max)
{
    int i, sum = 0, mx = 0;
    for (i = 0; i < 60; i++) {
        int v = be ? ((p[2 * i] << 8) | p[2 * i + 1]) : u16le(p + 2 * i);
        m[i] = (uint16_t)v; sum += v; if (v > mx) mx = v;
    }
    *max = mx;
    return sum;
}

int dec_hour(const frame_t *f, hourdata_t *h, endian_t mode, int max_per_min)
{
    const uint8_t *tp;
    int tv;
    memset(h, 0, sizeof *h);
    if (f->type != F_DATA0 && f->type != F_DATA5) return -1;
    h->hour.y = 2000 + f->raw[2]; h->hour.mo = f->raw[3];
    h->hour.d = f->raw[4]; h->hour.h = f->raw[5];
    if (!wtime_valid(&h->hour)) return -1;
    h->used = mode == EN_BE ? EN_BE : EN_LE;
    if (f->type == F_DATA5) {
        uint16_t le[60], be[60]; int mle, mbe, sle, sbe;
        if (f->len != 130) return -1;
        sle = fill_minutes(f->raw + 6, 0, le, &mle);
        sbe = fill_minutes(f->raw + 6, 1, be, &mbe);
        if (mode == EN_AUTO) {
            if (mle <= max_per_min) h->used = EN_LE;
            else if (mbe <= max_per_min) h->used = EN_BE;
            else { h->used = EN_LE; h->suspicious = 1; }
        }
        if (h->used == EN_BE) { memcpy(h->min, be, sizeof be); h->total = sbe; }
        else                  { memcpy(h->min, le, sizeof le); h->total = sle; }
        if ((h->used == EN_BE ? mbe : mle) > max_per_min) h->suspicious = 1;
        tp = f->raw + 126;
    } else {
        if (f->len != 10) return -1;
        tp = f->raw + 6;
    }
    tv = u16le(tp);
    if (tv == 0xFFFF) { h->temp_valid = 0; h->temp10 = 0; }
    else { h->temp_valid = 1; h->temp10 = (tv & 0x8000) ? -(tv & 0x7FFF) : tv; }
    return 0;
}
