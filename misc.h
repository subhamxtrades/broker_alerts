#ifndef MISC_H
#define MISC_H

#include <stdint.h>

// Dummy checksum function, as it's a dependency
static inline uint16_t ipchksum(uint16_t *buf, int len) {
    uint32_t sum = 0;
    while (len > 1) {
        sum += *buf++;
        len -= 2;
    }
    if (len) {
        sum += *(uint8_t *)buf;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return ~sum;
}

#endif // MISC_H