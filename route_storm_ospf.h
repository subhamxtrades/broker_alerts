#ifndef ROUTE_STORM_OSPF_H
#define ROUTE_STORM_OSPF_H

#include <stdint.h>
#include <rte_ether.h>

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

struct ospf_router_lsa_link {
    uint32_t link_id;
    uint32_t link_data;
    uint8_t type;
    uint8_t num_tos;
    uint16_t metric;
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

#define OSPF_MAX_ROUTER_INSTANCES 1000
#define OSPF_MAX_VIRTUAL_INTERFACES 4
#define OSPF_MAX_IPS_PER_VIF 128
#define OSPF_MAX_LSA 1024

/*
 * LSDB Entry
 * Represents a single LSA in the Link-State Database.
 */
struct lsdb_entry {
    uint64_t last_retransmitted;
    struct lsa_header lsa[]; // Flexible array member
};

struct ospf_virtual_interface; // Forward declaration

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
    struct ospf_virtual_interface *interface;
    uint64_t packets_sent;
    uint64_t packets_received;
    struct lsdb_entry *retransmission_list[OSPF_MAX_LSA];
    int retransmission_list_len;
    uint64_t last_seen;
};

/*
 * OSPF Virtual Interface
 * Represents a virtual interface on which OSPF is running.
 */
struct ospf_router_instance; // Forward declaration

struct ospf_virtual_interface {
    struct ospf_router_instance *router;
    uint16_t port_id;
    struct rte_ether_addr mac_addr;
    uint32_t ip_addresses[OSPF_MAX_IPS_PER_VIF];
    int num_ip_addresses;
    uint32_t network_mask;
    uint32_t area;
    uint16_t hello_interval;
    uint16_t dead_interval;
    uint8_t router_priority;
    uint32_t designated_router;
    uint32_t backup_router;
    struct ospf_neighbor neighbors[OSPF_MAX_NEIGHBORS];
    int num_neighbors;
    uint64_t packets_sent;
    uint64_t packets_received;
};

/*
 * OSPF Router Instance
 * Represents a simulated OSPF router.
 */
struct ospf_router_instance {
    uint32_t router_id;
    struct ospf_virtual_interface virtual_interfaces[OSPF_MAX_VIRTUAL_INTERFACES];
    int num_virtual_interfaces;
    struct lsdb_entry *lsdb[OSPF_MAX_LSA];
    int lsdb_len;
};

/*
 * OSPF Simulator
 * The top-level data structure for the OSPF simulator.
 */
struct ospf_simulator {
    struct ospf_router_instance router_instances[OSPF_MAX_ROUTER_INSTANCES];
    int num_router_instances;
    int num_hosts;
    uint32_t start_ip;
    struct rte_ether_addr start_mac;
};

enum timer_type {
    HELLO_TIMER,
    DEAD_TIMER,
    LSA_AGING_TIMER,
    LSA_RETRANSMIT_TIMER,
    LSA_GENERATE_TIMER,
};

struct timer {
    enum timer_type type;
    uint64_t expiration;
    void *data;
    struct timer *next;
};

struct timer_wheel {
    struct timer *slots[1024];
};

#endif // ROUTE_STORM_OSPF_H
