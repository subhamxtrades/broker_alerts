#ifndef HEADERS_H
#define HEADERS_H

#include <stdint.h>
#include <arpa/inet.h>

#define ETHERTYPE_IP 0x0800
#define IP_DF 0x4000

struct ethernet_hdr {
    uint8_t ether_dhost[6];
    uint8_t ether_shost[6];
    uint16_t ether_type;
} __attribute__((packed));

struct iphdr {
    uint8_t ihl:4, version:4;
    uint8_t tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t check;
    uint32_t saddr;
    uint32_t daddr;
} __attribute__((packed));

#endif // HEADERS_H