#ifndef RAIN_CRC16_H
#define RAIN_CRC16_H
#include <stdint.h>
#include <stddef.h>
uint16_t crc16_kermit(const uint8_t *p, size_t n);   /* CRC-16/KERMIT, init 0, poly 0x8408 */
#endif
