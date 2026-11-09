#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "serial.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/ioctl.h>

struct serial { int fd; };

static int baud_const(int b)
{
    switch (b) {
    case 1200: return B1200; case 2400: return B2400; case 4800: return B4800;
    case 9600: return B9600; case 19200: return B19200; case 38400: return B38400;
    case 57600: return B57600; case 115200: return B115200;
    default: return -1;
    }
}

serial_t *serial_open(const char *port, int baud, int dtr, int rts, char *err, size_t errsz)
{
    struct termios tio; int fd, bc, bits; serial_t *s;
    bc = baud_const(baud);
    if (bc < 0) { snprintf(err, errsz, "unsupported baud %d", baud); return NULL; }
    fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { snprintf(err, errsz, "open %s: %s", port, strerror(errno)); return NULL; }
    if (tcgetattr(fd, &tio) != 0) { snprintf(err, errsz, "tcgetattr: %s", strerror(errno)); close(fd); return NULL; }
    cfmakeraw(&tio);
    cfsetispeed(&tio, (speed_t)bc); cfsetospeed(&tio, (speed_t)bc);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tio.c_cflag |= CS8;
    tio.c_cc[VMIN] = 0; tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) { snprintf(err, errsz, "tcsetattr: %s", strerror(errno)); close(fd); return NULL; }
    bits = 0;
    if (ioctl(fd, TIOCMGET, &bits) == 0) {
        if (dtr) bits |= TIOCM_DTR; else bits &= ~TIOCM_DTR;
        if (rts) bits |= TIOCM_RTS; else bits &= ~TIOCM_RTS;
        ioctl(fd, TIOCMSET, &bits);
    }
    s = (serial_t *)calloc(1, sizeof *s);
    if (!s) { close(fd); snprintf(err, errsz, "out of memory"); return NULL; }
    s->fd = fd;
    return s;
}
int serial_write(serial_t *s, const uint8_t *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(s->fd, buf + off, n - off);
        if (w < 0) {
            if (errno == EAGAIN || errno == EINTR) { usleep(2000); continue; }
            return -1;
        }
        off += (size_t)w;
    }
    tcdrain(s->fd);
    return 0;
}
int serial_read(serial_t *s, uint8_t *buf, size_t max, int timeout_ms)
{
    fd_set rf; struct timeval tv; int r; ssize_t n;
    FD_ZERO(&rf); FD_SET(s->fd, &rf);
    tv.tv_sec = timeout_ms / 1000; tv.tv_usec = (timeout_ms % 1000) * 1000;
    r = select(s->fd + 1, &rf, NULL, NULL, &tv);
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (r == 0) return 0;
    n = read(s->fd, buf, max);
    if (n < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    if (n == 0) return -1;
    return (int)n;
}
void serial_flush_in(serial_t *s) { tcflush(s->fd, TCIFLUSH); }
void serial_close(serial_t *s) { if (s) { close(s->fd); free(s); } }
