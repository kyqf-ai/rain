#ifndef RAIN_SERIAL_H
#define RAIN_SERIAL_H
#include <stdint.h>
#include <stddef.h>
typedef struct serial serial_t;
serial_t *serial_open(const char *port, int baud, int dtr, int rts, char *err, size_t errsz);
int  serial_write(serial_t *s, const uint8_t *buf, size_t n);            /* 0 ok, -1 error */
int  serial_read(serial_t *s, uint8_t *buf, size_t max, int timeout_ms); /* >=0 bytes, -1 error */
void serial_flush_in(serial_t *s);
void serial_close(serial_t *s);
#endif
