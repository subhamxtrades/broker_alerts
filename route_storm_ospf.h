#ifndef ROUTE_STORM_OSPF_H
#define ROUTE_STORM_OSPF_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_mbuf.h>
#include <rte_hash.h>

/* OSPF base constants */
#define OSPF_VERSION                2
#define OSPF_PROTOCOL_NUMBER        89

#define OSPF_TYPE_HELLO             1
#define OSPF_TYPE_DD                2
#define OSPF_TYPE_LSR               3
#define OSPF_TYPE_LSU               4
#define OSPF_TYPE_LSACK             5

#define OSPF_OPTION_E               0x02
#define OSPF_DEFAULT_MTU            1500
#define OSPF_DEFAULT_AREA           0x00000000  /* 0.0.0.0 */

#define OSPF_HELLO_INTERVAL         10
#define OSPF_DEAD_INTERVAL          40

#define OSPF_ALLSPFROUTERS_MCAST    "224.0.0.5"
#define OSPF_ALLDROUTERS_MCAST      "224.0.0.6"

#define OSPF_MAX_LSAS_PER_UPDATE    10
#define OSPF_MAX_NEIGHBORS          256
#define OSPF_MAX_INTERFACES         16
#define OSPF_MAX_LSA_SIZE           4096

/* Neighbor states (RFC 2328 section 10) */
typedef enum {
    OSPF_STATE_DOWN = 0,
    OSPF_STATE_ATTEMPT,
    OSPF_STATE_INIT,
    OSPF_STATE_TWO_WAY,
    OSPF_STATE_EXSTART,
    OSPF_STATE_EXCHANGE,
    OSPF_STATE_LOADING,
    OSPF_STATE_FULL
} ospf_state_t;

/* Interface type (simplified) */
typedef enum {
    OSPF_IFTYPE_BROADCAST = 1,
    OSPF_IFTYPE_NBMA      = 2,
    OSPF_IFTYPE_P2P       = 3,
    OSPF_IFTYPE_VIRT      = 4
} ospf_iftype_t;

/* OSPF Header (RFC 2328, A.3.1) */
struct ospf_header {
    uint8_t  version;
    uint8_t  type;
    uint16_t length;
    uint32_t router_id;      /* network order */
    uint32_t area_id;        /* network order */
    uint16_t checksum;
    uint16_t auth_type;
    uint64_t auth_data;
} __attribute__((packed));

/* Hello packet body (RFC 2328, A.3.2) */
struct ospf_hello {
    uint32_t network_mask;   /* network order */
    uint16_t hello_interval;
    uint8_t  options;
    uint8_t  priority;
    uint32_t dead_interval;
    uint32_t designated_router;  /* IP addr, net order */
    uint32_t backup_dr;          /* IP addr, net order */
    /* followed by 0 or more neighbor Router IDs (net order, 4 bytes each) */
} __attribute__((packed));

/* Database Description (DD) packet body (RFC 2328, A.3.3) */
struct ospf_dd {
    uint16_t mtu;
    uint8_t  options;
    uint8_t  flags;
    uint32_t dd_sequence;
    /* followed by zero or more LSA headers */
} __attribute__((packed));

#define OSPF_DD_FLAG_MS  0x01  /* Master/Slave bit */
#define OSPF_DD_FLAG_M   0x02  /* More bit */
#define OSPF_DD_FLAG_I   0x04  /* Initial bit */

/* Link State Request (LSR) packet body (RFC 2328, A.3.4) */
struct ospf_lsr {
    uint32_t ls_type;
    uint32_t link_state_id;
    uint32_t advertising_router;
} __attribute__((packed));

/* LSA Types (RFC 2328, section 4.3) */
#define LSA_TYPE_ROUTER   1
#define LSA_TYPE_NETWORK  2
#define LSA_TYPE_SUMMARY  3
#define LSA_TYPE_ASBR     4
#define LSA_TYPE_EXTERNAL 5

/* LSA Header (RFC 2328, A.4.1) */
struct ospf_lsa_header {
    uint16_t age;
    uint8_t  options;
    uint8_t  type;
    uint32_t link_state_id;
    uint32_t advertising_router;
    uint32_t sequence_number;
    uint16_t checksum;
    uint16_t length;
} __attribute__((packed));

/* Link State Update (LSU) packet body (RFC 2328, A.3.5) */
struct ospf_lsu {
    uint32_t num_lsas;
    /* followed by one or more LSAs */
} __attribute__((packed));

/* Router LSA Flags */
#define OSPF_LSA_ROUTER_FLAG_B 0x01
#define OSPF_LSA_ROUTER_FLAG_E 0x02
#define OSPF_LSA_ROUTER_FLAG_V 0x04

/* Router LSA Link description (RFC 2328, A.4.2) */
struct ospf_router_lsa_link {
    uint32_t link_id;
    uint32_t link_data;
    uint8_t  type;
    uint8_t  num_tos;
    uint16_t metric;
} __attribute__((packed));

/* Message exchange tracking */
typedef struct {
    uint8_t step;           /* Current step in message exchange */
    uint8_t waiting_for;    /* What we're waiting for */
    uint64_t wait_until;    /* TSC cycles to wait until */
    uint8_t retry_count;    /* Number of retries */
} ospf_exchange_state_t;

/* Interface descriptor */
typedef struct {
    uint32_t ip_address;       /* network order */
    uint32_t network_mask;     /* network order */
    uint32_t area_id;          /* network order */
    ospf_iftype_t type;
    uint16_t hello_interval;
    uint16_t dead_interval;
    uint8_t  priority;
    uint8_t  options;
    uint32_t designated_router; /* IP address (network order) */
    uint32_t backup_dr;         /* IP address (network order) */
    uint16_t cost;
    uint8_t  state;             /* up/down flag */
    uint64_t last_hello_sent;
    uint64_t last_hello_received;
    uint8_t  neighbor_count_on_interface;
    uint8_t  is_passive;        /* Don't send Hellos on this interface */
    ospf_exchange_state_t exchange_state;
} ospf_interface_t;

/* Per-session test configuration / statistics */
typedef struct {
    uint32_t router_id;        /* network order */
    uint32_t area_id;          /* network order */
    uint32_t network_mask;     /* network order */
    uint16_t hello_interval;
    uint16_t dead_interval;
    uint8_t  priority;
    uint8_t  options;
    uint32_t designated_router;
    uint32_t backup_dr;

    /* Stats */
    uint64_t hello_sent;
    uint64_t hello_received;
    uint64_t dd_sent;
    uint64_t dd_received;
    uint64_t lsr_sent;
    uint64_t lsr_received;
    uint64_t lsu_sent;
    uint64_t lsu_received;
    uint64_t lsack_sent;
    uint64_t lsack_received;
    uint64_t neighbors_full;
} ospf_test_config_t;

/* Neighbor entry */
typedef struct {
    uint32_t router_id;            /* Router ID, network order */
    uint32_t ip_address;           /* src IP of neighbor, net order */
    uint8_t  interface_index;      /* Which interface this neighbor is on */
    ospf_state_t state;
    uint32_t dead_interval;        /* seconds */
    uint8_t  priority;
    uint32_t dd_sequence;          /* last DD sequence used */
    uint8_t  is_dr;
    uint8_t  is_bdr;
    uint8_t  is_master;            /* For DD exchange */
    uint64_t last_hello_received;  /* TSC cycles */
    uint64_t last_dd_received;     /* TSC cycles */
    uint64_t last_lsr_received;    /* TSC cycles */
    uint64_t last_lsu_received;    /* TSC cycles */
    uint32_t lsdb_summary[16];     /* Track LSAs exchanged */
    uint8_t  lsdb_summary_count;
    ospf_exchange_state_t exchange_state;
} ospf_neighbor_t;

/* Full OSPF session for a DPDK port (PID) */
typedef struct {
    uint8_t pid;
    uint8_t lid;
    uint32_t router_id;        /* network order */
    uint32_t area_id;          /* network order */
    ospf_test_config_t config;
    ospf_neighbor_t neighbors[OSPF_MAX_NEIGHBORS];
    uint16_t neighbor_count;
    uint8_t  state;
    uint64_t last_hello_sent;
    uint64_t last_dd_sent;
    uint64_t last_lsa_sent;
    struct rte_hash *lsdb;
    struct rte_hash *rib;
    struct rte_hash *fib;
    ospf_interface_t interfaces[OSPF_MAX_INTERFACES];
    uint8_t interface_count;
    uint8_t is_dr;              /* 1 if this router is DR */
    uint8_t is_bdr;             /* 1 if this router is BDR */
    uint8_t dr_election_done;
    uint32_t network_lsa_id;    /* For network LSA generation */
    uint8_t exchange_in_progress; /* Flag to control step-by-step exchange */
} ospf_session_t;

/* Helper / API */
const char* ip_to_string(uint32_t ip);
const char* ospf_state_to_string(ospf_state_t state);
uint32_t string_to_ip(const char* ip_str);

int ospf_initialize_test(uint8_t pid, uint32_t router_id, uint32_t area_id);
void ospf_cleanup_session(uint8_t pid);
struct rte_hash* ospf_create_hash_table(const char *name, uint32_t entries);

/* Send side */
int ospf_send_packet(uint8_t pid, ospf_session_t *session, uint8_t packet_type,
                     void *packet_data, uint16_t data_length, uint32_t dest_ip,
                     uint32_t src_ip);

int ospf_send_hello_packet(uint8_t pid, ospf_session_t *session, uint8_t iface_index);
int ospf_send_dd_packet(uint8_t pid, ospf_session_t *session,
                        uint32_t neighbor_rid, uint8_t flags, uint32_t dd_seq,
                        bool include_lsa_headers, uint8_t iface_index);
int ospf_send_lsr_packet(uint8_t pid, ospf_session_t *session,
                         uint32_t neighbor_rid, uint32_t ls_type,
                         uint32_t link_state_id, uint32_t adv_router, uint8_t iface_index);
int ospf_send_lsu_packet(uint8_t pid, ospf_session_t *session,
                         uint32_t neighbor_rid, uint8_t lsa_type,
                         uint8_t lsa_count, struct ospf_lsa_header **lsas, uint8_t iface_index);
int ospf_send_lsack_packet(uint8_t pid, ospf_session_t *session,
                           uint32_t neighbor_rid, struct ospf_lsa_header **lsas,
                           uint8_t lsa_count, uint8_t iface_index);

/* Receive side / FSM */
int ospf_process_packet(struct rte_mbuf *pkt, uint8_t pid, ospf_session_t *session);
int ospf_handle_hello_packet(struct ospf_header *ospf_hdr, struct ospf_hello *hello,
                             uint8_t pid, ospf_session_t *session, uint32_t src_ip);
int ospf_handle_dd_packet(struct ospf_header *ospf_hdr, struct ospf_dd *dd,
                          uint8_t pid, ospf_session_t *session, uint32_t src_ip);
int ospf_handle_lsr_packet(struct ospf_header *ospf_hdr, struct ospf_lsr *lsr,
                           uint8_t pid, ospf_session_t *session, uint32_t src_ip);
int ospf_handle_lsu_packet(struct ospf_header *ospf_hdr, struct ospf_lsu *lsu,
                           uint8_t pid, ospf_session_t *session, uint32_t src_ip);
int ospf_handle_lsack_packet(struct ospf_header *ospf_hdr,
                           uint8_t pid, ospf_session_t *session, uint32_t src_ip);

void ospf_update_neighbor_state(ospf_session_t *session, uint32_t neighbor_rid,
                                uint8_t interface_index, ospf_state_t state);
void ospf_process_neighbor_timeouts(ospf_session_t *session, uint8_t pid);
void ospf_interface_dr_election(ospf_session_t *session, uint8_t iface_index);

/* LSAs / SPF */
int ospf_generate_router_lsa(ospf_session_t *session, struct ospf_lsa_header *lsa, uint8_t iface_index);
int ospf_generate_network_lsa(ospf_session_t *session, struct ospf_lsa_header *lsa, uint8_t iface_index);
void ospf_run_spf(ospf_session_t *session);
int ospf_add_route(ospf_session_t *session, uint32_t prefix, uint32_t mask,
                   uint32_t next_hop, uint16_t metric, uint8_t lsa_type);
int ospf_remove_route(ospf_session_t *session, uint32_t prefix, uint32_t mask);

/* Checksums */
uint16_t ospf_checksum(struct ospf_header *ospf_hdr, uint16_t length);
uint16_t ospf_lsa_checksum(struct ospf_lsa_header *lsa_hdr);

/* Step-by-step exchange control */
void ospf_start_exchange(ospf_session_t *s, uint8_t iface_index);
int ospf_process_exchange_steps(ospf_session_t *s, uint8_t pid);

/* Main test loop */
int ospf_test_main_loop(uint8_t pid, int userId, uint8_t pairPid);

#endif /* ROUTE_STORM_OSPF_H */
