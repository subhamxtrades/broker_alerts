#ifndef ROUTE_STORM_OSPF_H
#define ROUTE_STORM_OSPF_H

#include <stdint.h>

#define OSPF_VERSION 2
#define OSPF_ALL_SPFRouters "224.0.0.5"
#define OSPF_ALL_DRouters "224.0.0.6"
#define IP_PROTOCOL_OSPF 89

/*
 * OSPF Packet Header
 * Describes the common header for all OSPF packets.
 */
struct ospf_header {
    uint8_t version;
    uint8_t type;
    uint16_t packet_length;
    uint32_t router_id;
    uint32_t area_id;
    uint16_t checksum;
    uint16_t auth_type;
    uint64_t auth_data;
};

/*
 * OSPF Hello Packet
 * Used for neighbor discovery and maintaining adjacencies.
 */
struct ospf_hello_packet {
    struct ospf_header header;
    uint32_t network_mask;
    uint16_t hello_interval;
    uint8_t options;
    uint8_t router_priority;
    uint32_t dead_interval;
    uint32_t designated_router;
    uint32_t backup_router;
    uint32_t neighbors[0]; // Variable length neighbors
};

/*
 * LSA Header
 * The common header for all Link-State Advertisements.
 */
struct lsa_header {
    uint16_t ls_age;
    uint8_t options;
    uint8_t ls_type;
    uint32_t link_state_id;
    uint32_t advertising_router;
    uint32_t ls_sequence_number;
    uint16_t ls_checksum;
    uint16_t length;
};

/*
 * Database Description (DD) Packet
 * Used to exchange database summaries.
 */
struct ospf_dd_packet {
    struct ospf_header header;
    uint16_t mtu;
    uint8_t options;
    uint8_t db_description_bits;
    uint32_t dd_sequence_number;
    struct lsa_header lsas[0]; // Variable length LSAs
};

/*
 * Link-State Request (LSR) Packet
 * Used to request specific LSAs from a neighbor.
 */
struct ospf_lsr_packet {
    struct ospf_header header;
    uint32_t ls_type;
    uint32_t link_state_id;
    uint32_t advertising_router;
};

/*
 * Link-State Update (LSU) Packet
 * Carries a collection of LSAs.
 */
struct ospf_lsu_packet {
    struct ospf_header header;
    uint32_t num_lsas;
    struct lsa_header lsas[0]; // Variable length LSAs
};

/*
 * Link-State Acknowledgment (LSAck) Packet
 * Acknowledges receipt of LSAs.
 */
struct ospf_lsack_packet {
    struct ospf_header header;
    struct lsa_header lsa_headers[0]; // Variable length LSA headers
};

/*
 * OSPF Packet Types
 */
enum ospf_packet_type {
    OSPF_HELLO = 1,
    OSPF_DD = 2,
    OSPF_LSR = 3,
    OSPF_LSU = 4,
    OSPF_LSACK = 5,
};

/*
 * OSPF Neighbor States
 */
enum ospf_neighbor_state {
    DOWN,
    ATTEMPT,
    INIT,
    TWO_WAY,
    EXSTART,
    EXCHANGE,
    LOADING,
    FULL,
};

#define OSPF_MAX_NEIGHBORS 16
#define OSPF_DD_I_BIT 0x04
#define OSPF_DD_M_BIT 0x02
#define OSPF_DD_MS_BIT 0x01

struct ospf_interface; // Forward declaration

/*
 * OSPF Neighbor
 * Represents a neighbor router.
 */
struct ospf_neighbor {
    uint32_t id;
    uint8_t router_priority;
    enum ospf_neighbor_state state;
    uint32_t dd_sequence_number;
    uint8_t is_master;
    struct ospf_interface *interface;
};

/*
 * OSPF Interface
 * Represents a network interface on which OSPF is running.
 */
struct ospf_interface {
    uint32_t ip_address;
    uint32_t network_mask;
    uint8_t router_priority;
    uint32_t designated_router;
    uint32_t backup_router;
    struct ospf_neighbor neighbors[OSPF_MAX_NEIGHBORS];
    int num_neighbors;
};

#endif // ROUTE_STORM_OSPF_H
