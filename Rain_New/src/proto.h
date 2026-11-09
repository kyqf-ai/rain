#ifndef RAIN_PROTO_H
#define RAIN_PROTO_H
#include <stdint.h>
#include <stddef.h>
#include "util.h"

/*
 * Wire format (verified on the real device, see docs/DESIGN.md):
 *   PC -> dev : 55 LEN CODE SUB [ARGS...] CRC_L CRC_H      (LEN = total length)
 *   dev -> PC : AA TYPE DATA... CRC_L CRC_H
 *   CRC = CRC-16/KERMIT over every byte before the CRC, low byte first.
 */
#define PROTO_MAX_FRAME 200

enum {
    F_DATA0  = 0x00,   /* hour data, no rain: date(4) temp(2)                    len 10  */
    F_ID     = 0x01,   /* machine id, ASCII                                      var     */
    F_TIME   = 0x03,   /* yy mm dd hh mi ss wday                                 len 11  */
    F_DATA5  = 0x05,   /* hour data with rain: date(4) 60 x u16 temp(2)          len 130 */
    F_STATUS = 0x06,   /* status block                                           len 52  */
    F_A8     = 0x08,   /* seen in vendor program only                            var     */
    F_END    = 0x09    /* end-of-transfer marker  AA 09 00 03 16 D0              len 6   */
};

typedef struct { uint8_t type; size_t len; uint8_t raw[PROTO_MAX_FRAME]; } frame_t;
typedef enum { PS_NEED_MORE = 0, PS_FRAME, PS_BADCRC, PS_JUNK } ps_t;
typedef enum { EN_AUTO = 0, EN_LE = 1, EN_BE = 2 } endian_t;

/* Build a command; returns length. */
size_t proto_build(uint8_t *out, uint8_t code, uint8_t sub, const uint8_t *args, size_t nargs);

/*
 * Try to take one frame from the head of buf.
 * quiet=1 means the line has been idle for a while: variable-length frames and
 * bad-CRC cases may then be resolved. *used = bytes to drop from buf.
 */
ps_t proto_next(const uint8_t *buf, size_t n, frame_t *fr, size_t *used, int quiet);

typedef struct {
    wtime_t t; int wday;
    uint8_t flags[4];                       /* bytes 9..12, meaning unknown, kept raw */
    int v_static, v_dyn, v_sun, v_alarm, v_hang;   /* mV */
    int temp_c;                             /* byte 23, unverified */
    char raw_hex[3 * 52 + 1];
} status_t;

typedef struct {
    wtime_t hour;                           /* mi = s = 0 */
    int temp_valid, temp10;                 /* 0.1 degC */
    uint16_t min[60];                       /* tip counts per minute */
    int total;
    endian_t used;
    int suspicious;                         /* values implausible in both byte orders */
} hourdata_t;

int dec_id(const frame_t *f, char *out, size_t outsz);
int dec_time(const frame_t *f, wtime_t *t, int *wday);
int dec_status(const frame_t *f, status_t *s);
int dec_hour(const frame_t *f, hourdata_t *h, endian_t mode, int max_per_min);
#endif
