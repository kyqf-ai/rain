#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include "serial.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct serial { HANDLE h; DWORD last_to; };

serial_t *serial_open(const char *port, int baud, int dtr, int rts, char *err, size_t errsz)
{
    char path[96]; HANDLE h; DCB dcb; serial_t *s;
    if (_strnicmp(port, "\\\\.\\", 4) == 0) snprintf(path, sizeof path, "%s", port);
    else snprintf(path, sizeof path, "\\\\.\\%s", port);   /* required for COM10 and above */
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { snprintf(err, errsz, "open %s failed (error %lu)", port, (unsigned long)GetLastError()); return NULL; }
    memset(&dcb, 0, sizeof dcb); dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb)) { snprintf(err, errsz, "GetCommState failed (%lu)", (unsigned long)GetLastError()); CloseHandle(h); return NULL; }
    dcb.BaudRate = (DWORD)baud; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE; dcb.fParity = FALSE; dcb.fOutxCtsFlow = FALSE; dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = dtr ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
    dcb.fRtsControl = rts ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE;
    dcb.fOutX = FALSE; dcb.fInX = FALSE; dcb.fNull = FALSE; dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) { snprintf(err, errsz, "SetCommState failed (%lu)", (unsigned long)GetLastError()); CloseHandle(h); return NULL; }
    SetupComm(h, 4096, 4096);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    s = (serial_t *)calloc(1, sizeof *s);
    if (!s) { CloseHandle(h); snprintf(err, errsz, "out of memory"); return NULL; }
    s->h = h; s->last_to = 0xFFFFFFFFu;
    return s;
}
int serial_write(serial_t *s, const uint8_t *buf, size_t n)
{
    DWORD done = 0;
    if (!WriteFile(s->h, buf, (DWORD)n, &done, NULL) || done != n) return -1;
    FlushFileBuffers(s->h);
    return 0;
}
int serial_read(serial_t *s, uint8_t *buf, size_t max, int timeout_ms)
{
    DWORD got = 0;
    if (timeout_ms < 1) timeout_ms = 1;
    if (s->last_to != (DWORD)timeout_ms) {
        COMMTIMEOUTS to;
        /* MAXDWORD/MAXDWORD/constant: return at once if bytes are waiting, else wait up to constant ms */
        to.ReadIntervalTimeout = MAXDWORD; to.ReadTotalTimeoutMultiplier = MAXDWORD;
        to.ReadTotalTimeoutConstant = (DWORD)timeout_ms;
        to.WriteTotalTimeoutMultiplier = 0; to.WriteTotalTimeoutConstant = 1000;
        if (!SetCommTimeouts(s->h, &to)) return -1;
        s->last_to = (DWORD)timeout_ms;
    }
    if (!ReadFile(s->h, buf, (DWORD)max, &got, NULL)) return -1;
    return (int)got;
}
void serial_flush_in(serial_t *s) { PurgeComm(s->h, PURGE_RXCLEAR); }
void serial_close(serial_t *s) { if (s) { CloseHandle(s->h); free(s); } }
