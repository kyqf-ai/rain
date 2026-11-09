#include "crc16.h"
uint16_t crc16_kermit(const uint8_t *p, size_t n)
{
    uint16_t c = 0; size_t i; int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1);
    }
    return c;
}
