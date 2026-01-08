#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <arpa/inet.h>

#include "route_storm_ospf.h"

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

struct ospf_router {
    uint32_t router_id;
    struct ospf_interface interfaces[2];
};

static struct ospf_router g_ospf_router;

static void send_hello_packet(uint16_t port_id, struct rte_mempool *mbuf_pool);
static void send_dd_packet(uint16_t port_id, struct rte_mempool *mbuf_pool, struct ospf_neighbor *neighbor);
static void process_packet(struct rte_mbuf *mbuf, uint16_t port_id);
static void ospf_handle_hello_packet(struct ospf_hello_packet *hello_pkt, uint16_t port_id, struct rte_mempool *mbuf_pool);
static void run_dr_bdr_election(struct ospf_interface *interface);
static void ospf_handle_lsr_packet(struct ospf_lsr_packet *lsr_pkt, uint16_t port_id, struct rte_mempool *mbuf_pool);
static void ospf_handle_lsu_packet(struct ospf_lsu_packet *lsu_pkt, uint16_t port_id, struct rte_mempool *mbuf_pool);
static void ospf_handle_lsack_packet(struct ospf_lsack_packet *lsack_pkt, uint16_t port_id, struct rte_mempool *mbuf_pool);
static inline int port_init(uint16_t port, struct rte_mempool *mbuf_pool);

int main(int argc, char *argv[]) {
    // Initialize the Environment Abstraction Layer (EAL)
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
    }

    argc -= ret;
    argv += ret;

    // Initialize the OSPF router
    g_ospf_router.router_id = rte_cpu_to_be_32(0x02020202); // 2.2.2.2
    g_ospf_router.interfaces[0].ip_address = rte_cpu_to_be_32(0xc0a80164); // 192.168.1.100
    g_ospf_router.interfaces[0].network_mask = rte_cpu_to_be_32(0xffffff00); // 255.255.255.0
    g_ospf_router.interfaces[0].router_priority = 1;
    g_ospf_router.interfaces[1].ip_address = rte_cpu_to_be_32(0xc0a80264); // 192.168.2.100
    g_ospf_router.interfaces[1].network_mask = rte_cpu_to_be_32(0xffffff00); // 255.255.255.0
    g_ospf_router.interfaces[1].router_priority = 1;

    // Check that there are two ports to send/receive on
    if (rte_eth_dev_count_avail() < 2) {
        rte_exit(EXIT_FAILURE, "Error: not enough ports available\n");
    }

    // Create a memory pool to hold the mbufs
    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mbuf_pool == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
    }

    // Initialize the ports
    uint16_t portid;
    RTE_ETH_FOREACH_DEV(portid) {
        if (port_init(portid, mbuf_pool) != 0) {
            rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu16 "\n", portid);
        }
    }

    // We are done with initialization, let's just print a success message
    printf("DPDK Initialized Successfully!\n");

    // Send a hello packet from each port
    RTE_ETH_FOREACH_DEV(portid) {
        send_hello_packet(portid, mbuf_pool);
    }

    // Main processing loop
    while (1) {
        RTE_ETH_FOREACH_DEV(portid) {
            struct rte_mbuf *bufs[32];
            const uint16_t nb_rx = rte_eth_rx_burst(portid, 0, bufs, 32);
            if (nb_rx == 0)
                continue;

            for (uint16_t i = 0; i < nb_rx; i++) {
                process_packet(bufs[i], portid);
                rte_pktmbuf_free(bufs[i]);
            }
        }
    }

    return 0;
}

static void send_hello_packet(uint16_t port_id, struct rte_mempool *mbuf_pool) {
    const unsigned total_length = sizeof(struct rte_ether_hdr) +
                                  sizeof(struct rte_ipv4_hdr) +
                                  sizeof(struct ospf_hello_packet);

    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mbuf_pool);
    if (!mbuf) {
        rte_exit(EXIT_FAILURE, "Cannot allocate mbuf\n");
    }

    mbuf->data_len = total_length;
    mbuf->pkt_len = total_length;

    // Ethernet header
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    rte_eth_macaddr_get(port_id, &eth_hdr->src_addr);
    // Destination MAC for OSPF is a multicast address
    eth_hdr->dst_addr = (struct rte_ether_addr){.addr_bytes = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x05}};
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    // IP header
    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    ip_hdr->version_ihl = (4 << 4) | 5;
    ip_hdr->type_of_service = 0;
    ip_hdr->total_length = rte_cpu_to_be_16(total_length - sizeof(struct rte_ether_hdr));
    ip_hdr->packet_id = 0;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live = 1; // OSPF packets are not forwarded
    ip_hdr->next_proto_id = IP_PROTOCOL_OSPF;
    ip_hdr->hdr_checksum = 0;
    ip_hdr->src_addr = g_ospf_router.interfaces[port_id].ip_address;
    inet_pton(AF_INET, OSPF_ALL_SPFRouters, &ip_hdr->dst_addr);

    // OSPF Hello packet
    struct ospf_hello_packet *hello_pkt = (struct ospf_hello_packet *)(ip_hdr + 1);
    hello_pkt->header.version = OSPF_VERSION;
    hello_pkt->header.type = OSPF_HELLO;
    hello_pkt->header.packet_length = rte_cpu_to_be_16(sizeof(struct ospf_hello_packet));
    hello_pkt->header.router_id = g_ospf_router.router_id;
    hello_pkt->header.area_id = rte_cpu_to_be_32(0);
    hello_pkt->header.checksum = 0;
    hello_pkt->header.auth_type = 0;
    hello_pkt->header.auth_data = 0;
    hello_pkt->network_mask = g_ospf_router.interfaces[port_id].network_mask;
    hello_pkt->hello_interval = rte_cpu_to_be_16(10);
    hello_pkt->options = 0x02; // E-bit
    hello_pkt->router_priority = g_ospf_router.interfaces[port_id].router_priority;
    hello_pkt->dead_interval = rte_cpu_to_be_32(40);
    hello_pkt->designated_router = g_ospf_router.interfaces[port_id].designated_router;
    hello_pkt->backup_router = g_ospf_router.interfaces[port_id].backup_router;

    // Checksums
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
    // Checksums
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
    hello_pkt->header.checksum = ospf_checksum(hello_pkt, sizeof(struct ospf_hello_packet));

    // Send the packet
    const uint16_t nb_tx = rte_eth_tx_burst(port_id, 0, &mbuf, 1);
    if (nb_tx == 0) {
        rte_pktmbuf_free(mbuf);
        printf("Failed to send hello packet on port %u\n", port_id);
    } else {
        printf("Sent hello packet on port %u\n", port_id);
    }
}

static uint16_t ospf_checksum(const void *data, size_t len);
static void ospf_handle_dd_packet(struct ospf_dd_packet *dd_pkt, uint16_t port_id, struct rte_mempool *mbuf_pool);


static void process_packet(struct rte_mbuf *mbuf, uint16_t port_id) {
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);

    if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        return;
    }

    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

    if (ip_hdr->next_proto_id != IP_PROTOCOL_OSPF) {
        return;
    }

    struct ospf_header *ospf_hdr = (struct ospf_header *)(ip_hdr + 1);

    switch (ospf_hdr->type) {
        case OSPF_HELLO:
            ospf_handle_hello_packet((struct ospf_hello_packet *)ospf_hdr, port_id, rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS, MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id()));
            break;
        case OSPF_DD:
            ospf_handle_dd_packet((struct ospf_dd_packet *)ospf_hdr, port_id, rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS, MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id()));
            break;
        case OSPF_LSR:
            ospf_handle_lsr_packet((struct ospf_lsr_packet *)ospf_hdr, port_id, rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS, MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id()));
            break;
        case OSPF_LSU:
            ospf_handle_lsu_packet((struct ospf_lsu_packet *)ospf_hdr, port_id, rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS, MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id()));
            break;
        case OSPF_LSACK:
            ospf_handle_lsack_packet((struct ospf_lsack_packet *)ospf_hdr, port_id, rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS, MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id()));
            break;
        default:
            printf("Received unknown OSPF packet type %u\n", ospf_hdr->type);
            break;
    }
}

static void ospf_handle_hello_packet(struct ospf_hello_packet *hello_pkt, uint16_t port_id, struct rte_mempool *mbuf_pool) {
    struct ospf_interface *interface = &g_ospf_router.interfaces[port_id];
    struct ospf_neighbor *neighbor = NULL;

    // Check if we already have a neighbor entry for this router
    for (int i = 0; i < interface->num_neighbors; i++) {
        if (interface->neighbors[i].id == hello_pkt->header.router_id) {
            neighbor = &interface->neighbors[i];
            break;
        }
    }

    // If not, create a new neighbor entry
    if (!neighbor) {
        if (interface->num_neighbors >= OSPF_MAX_NEIGHBORS) {
            return; // Too many neighbors
        }
        neighbor = &interface->neighbors[interface->num_neighbors++];
        neighbor->id = hello_pkt->header.router_id;
        neighbor->router_priority = hello_pkt->router_priority;
        neighbor->state = DOWN;
        neighbor->interface = interface;
    }

    // Handle neighbor state transitions
    if (neighbor->state == DOWN) {
        neighbor->state = INIT;
    }

    // Check for 2-Way communication
    for (int i = 0; i < (rte_be_to_cpu_16(hello_pkt->header.packet_length) - sizeof(struct ospf_hello_packet)) / 4; i++) {
        if (hello_pkt->neighbors[i] == g_ospf_router.router_id) {
            if (neighbor->state == INIT) {
                neighbor->state = TWO_WAY;
                run_dr_bdr_election(interface);
            }
            break;
        }
    }

    // Adjacency logic for broadcast networks
    if (interface->designated_router == g_ospf_router.router_id || interface->backup_router == g_ospf_router.router_id) {
        if (neighbor->state == TWO_WAY) {
            neighbor->state = EXSTART;
            neighbor->dd_sequence_number = rte_be_to_cpu_32(g_ospf_router.router_id);
            neighbor->is_master = 1;
            send_dd_packet(port_id, mbuf_pool, neighbor);
        }
    }
}

static void run_dr_bdr_election(struct ospf_interface *interface) {
    uint32_t dr = 0;
    uint32_t bdr = 0;
    uint8_t dr_priority = 0;
    uint8_t bdr_priority = 0;

    // First, elect the BDR
    // Include ourself in the election
    if (interface->router_priority > bdr_priority) {
        bdr = g_ospf_router.router_id;
        bdr_priority = interface->router_priority;
    }
    for (int i = 0; i < interface->num_neighbors; i++) {
        struct ospf_neighbor *neighbor = &interface->neighbors[i];
        if (neighbor->state >= TWO_WAY) {
            if (neighbor->router_priority > bdr_priority) {
                bdr = neighbor->id;
                bdr_priority = neighbor->router_priority;
            } else if (neighbor->router_priority == bdr_priority && neighbor->id > bdr) {
                bdr = neighbor->id;
            }
        }
    }

    // Then, elect the DR
    // Include ourself in the election
    if (interface->router_priority > dr_priority && g_ospf_router.router_id != bdr) {
        dr = g_ospf_router.router_id;
        dr_priority = interface->router_priority;
    }
    for (int i = 0; i < interface->num_neighbors; i++) {
        struct ospf_neighbor *neighbor = &interface->neighbors[i];
        if (neighbor->state >= TWO_WAY && neighbor->id != bdr) {
            if (neighbor->router_priority > dr_priority) {
                dr = neighbor->id;
                dr_priority = neighbor->router_priority;
            } else if (neighbor->router_priority == dr_priority && neighbor->id > dr) {
                dr = neighbor->id;
            }
        }
    }

    interface->designated_router = dr;
    interface->backup_router = bdr;
}

static void send_dd_packet(uint16_t port_id, struct rte_mempool *mbuf_pool, struct ospf_neighbor *neighbor) {
    const unsigned total_length = sizeof(struct rte_ether_hdr) +
                                  sizeof(struct rte_ipv4_hdr) +
                                  sizeof(struct ospf_dd_packet);

    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mbuf_pool);
    if (!mbuf) {
        rte_exit(EXIT_FAILURE, "Cannot allocate mbuf\n");
    }

    mbuf->data_len = total_length;
    mbuf->pkt_len = total_length;

    // Ethernet header
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    rte_eth_macaddr_get(port_id, &eth_hdr->src_addr);
    eth_hdr->dst_addr = (struct rte_ether_addr){.addr_bytes = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x05}};
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    // IP header
    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    ip_hdr->version_ihl = (4 << 4) | 5;
    ip_hdr->type_of_service = 0;
    ip_hdr->total_length = rte_cpu_to_be_16(total_length - sizeof(struct rte_ether_hdr));
    ip_hdr->packet_id = 0;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live = 1;
    ip_hdr->next_proto_id = IP_PROTOCOL_OSPF;
    ip_hdr->hdr_checksum = 0;
    ip_hdr->src_addr = g_ospf_router.interfaces[port_id].ip_address;
    inet_pton(AF_INET, OSPF_ALL_SPFRouters, &ip_hdr->dst_addr);

    // OSPF DD packet
    struct ospf_dd_packet *dd_pkt = (struct ospf_dd_packet *)(ip_hdr + 1);
    dd_pkt->header.version = OSPF_VERSION;
    dd_pkt->header.type = OSPF_DD;
    dd_pkt->header.packet_length = rte_cpu_to_be_16(sizeof(struct ospf_dd_packet));
    dd_pkt->header.router_id = g_ospf_router.router_id;
    dd_pkt->header.area_id = rte_cpu_to_be_32(0);
    dd_pkt->header.checksum = 0;
    dd_pkt->header.auth_type = 0;
    dd_pkt->header.auth_data = 0;
    dd_pkt->mtu = rte_cpu_to_be_16(1500);
    dd_pkt->options = 0x02; // E-bit
    dd_pkt->db_description_bits = OSPF_DD_I_BIT | OSPF_DD_M_BIT | OSPF_DD_MS_BIT;
    dd_pkt->dd_sequence_number = rte_cpu_to_be_32(neighbor->dd_sequence_number);

    // Checksums
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
    dd_pkt->header.checksum = ospf_checksum(dd_pkt, sizeof(struct ospf_dd_packet));

    // Send the packet
    const uint16_t nb_tx = rte_eth_tx_burst(port_id, 0, &mbuf, 1);
    if (nb_tx == 0) {
        rte_pktmbuf_free(mbuf);
        printf("Failed to send DD packet on port %u\n", port_id);
    } else {
        printf("Sent DD packet on port %u\n", port_id);
    }
}

static void ospf_handle_dd_packet(struct ospf_dd_packet *dd_pkt, uint16_t port_id, struct rte_mempool *mbuf_pool) {
    struct ospf_interface *interface = &g_ospf_router.interfaces[port_id];
    struct ospf_neighbor *neighbor = NULL;

    // Find the neighbor
    for (int i = 0; i < interface->num_neighbors; i++) {
        if (interface->neighbors[i].id == dd_pkt->header.router_id) {
            neighbor = &interface->neighbors[i];
            break;
        }
    }

    if (!neighbor) {
        return; // DD packet from an unknown neighbor
    }

    // Handle ExStart state
    if (neighbor->state == EXSTART) {
        if ((dd_pkt->db_description_bits & (OSPF_DD_I_BIT | OSPF_DD_M_BIT | OSPF_DD_MS_BIT)) == (OSPF_DD_I_BIT | OSPF_DD_M_BIT | OSPF_DD_MS_BIT)) {
            // Master/Slave negotiation
            if (rte_be_to_cpu_32(g_ospf_router.router_id) > rte_be_to_cpu_32(neighbor->id)) {
                // We are the master
                neighbor->is_master = 1;
                neighbor->dd_sequence_number = rte_be_to_cpu_32(g_ospf_router.router_id);
                send_dd_packet(port_id, mbuf_pool, neighbor);
            } else {
                // We are the slave
                neighbor->is_master = 0;
                neighbor->dd_sequence_number = rte_be_to_cpu_32(dd_pkt->dd_sequence_number);
                send_dd_packet(port_id, mbuf_pool, neighbor);
            }
            neighbor->state = EXCHANGE;
        }
    }

    // Handle Exchange state
    if (neighbor->state == EXCHANGE) {
        if (neighbor->is_master) {
            if (dd_pkt->dd_sequence_number == rte_cpu_to_be_32(neighbor->dd_sequence_number)) {
                neighbor->dd_sequence_number++;
                // TODO: Send DD packet with LSA headers
            }
        } else {
            if (dd_pkt->dd_sequence_number == rte_cpu_to_be_32(neighbor->dd_sequence_number + 1)) {
                neighbor->dd_sequence_number = rte_be_to_cpu_32(dd_pkt->dd_sequence_number);
                // TODO: Send DD packet with LSA headers
            }
        }
    }

    // Check if the DD exchange is complete
    if (!(dd_pkt->db_description_bits & OSPF_DD_M_BIT)) {
        neighbor->state = LOADING;
        // TODO: Send LSR packets
    }
}

static void ospf_handle_lsr_packet(struct ospf_lsr_packet *lsr_pkt, uint16_t port_id, struct rte_mempool *mbuf_pool) {
    printf("Received OSPF LSR on port %u\n", port_id);
}

static void ospf_handle_lsu_packet(struct ospf_lsu_packet *lsu_pkt, uint16_t port_id, struct rte_mempool *mbuf_pool) {
    printf("Received OSPF LSU on port %u\n", port_id);
}

static void ospf_handle_lsack_packet(struct ospf_lsack_packet *lsack_pkt, uint16_t port_id, struct rte_mempool *mbuf_pool) {
    printf("Received OSPF LSAck on port %u\n", port_id);
}

static uint16_t ospf_checksum(const void *data, size_t len) {
    const uint8_t *p = data;
    uint16_t sum1 = 0, sum2 = 0;

    for (size_t i = 0; i < len; ++i) {
        sum1 = (sum1 + p[i]) % 255;
        sum2 = (sum2 + sum1) % 255;
    }

    return (sum2 << 8) | sum1;
}

static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool)
{
    struct rte_eth_conf port_conf;
    const uint16_t rx_rings = 1, tx_rings = 1;
    int retval;
    uint16_t q;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_txconf txconf;

    if (!rte_eth_dev_is_valid_port(port))
        return -1;

    memset(&port_conf, 0, sizeof(struct rte_eth_conf));

    retval = rte_eth_dev_info_get(port, &dev_info);
    if (retval != 0) {
        printf("Error during getting device (port %u) info: %s\n",
                port, strerror(-retval));
        return retval;
    }

    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
        port_conf.txmode.offloads |=
            RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (retval != 0)
        return retval;

    for (q = 0; q < rx_rings; q++) {
        retval = rte_eth_rx_queue_setup(port, q, RX_RING_SIZE,
                rte_eth_dev_socket_id(port), NULL, mbuf_pool);
        if (retval < 0)
            return retval;
    }

    txconf = dev_info.default_txconf;
    txconf.offloads = port_conf.txmode.offloads;
    for (q = 0; q < tx_rings; q++) {
        retval = rte_eth_tx_queue_setup(port, q, TX_RING_SIZE,
                rte_eth_dev_socket_id(port), &txconf);
        if (retval < 0)
            return retval;
    }

    retval  = rte_eth_dev_start(port);
    if (retval < 0)
        return retval;

    struct rte_ether_addr addr;
    retval = rte_eth_macaddr_get(port, &addr);
    if (retval != 0)
        return retval;

    printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
               " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
            port, RTE_ETHER_ADDR_BYTES(&addr));

    retval = rte_eth_promiscuous_enable(port);
    if (retval != 0)
        return retval;

    return 0;
}
