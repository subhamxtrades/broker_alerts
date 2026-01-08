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

static struct ospf_simulator g_ospf_simulator;

static void send_hello_packet(struct ospf_virtual_interface *vif, struct rte_mempool *mbuf_pool);
static void send_dd_packet(struct ospf_virtual_interface *vif, struct rte_mempool *mbuf_pool, struct ospf_neighbor *neighbor);
static void process_packet(struct rte_mbuf *mbuf, uint16_t port_id);
static void ospf_handle_hello_packet(struct ospf_hello_packet *hello_pkt, struct ospf_virtual_interface *vif, struct rte_mempool *mbuf_pool);
static void run_dr_bdr_election(struct ospf_virtual_interface *vif);
static void ospf_handle_dd_packet(struct ospf_dd_packet *dd_pkt, struct ospf_virtual_interface *vif, struct rte_mempool *mbuf_pool);
static void ospf_handle_lsr_packet(struct ospf_lsr_packet *lsr_pkt, struct ospf_virtual_interface *vif, struct rte_mempool *mbuf_pool);
static void ospf_handle_lsu_packet(struct ospf_lsu_packet *lsu_pkt, struct ospf_virtual_interface *vif, struct rte_mempool *mbuf_pool);
static void ospf_handle_lsack_packet(struct ospf_lsack_packet *lsack_pkt, struct ospf_virtual_interface *vif, struct rte_mempool *mbuf_pool);
static struct ospf_virtual_interface *find_virtual_interface(uint16_t port_id, struct rte_ether_addr *mac);
static inline int port_init(uint16_t port, struct rte_mempool *mbuf_pool);
static uint16_t ospf_checksum(const void *data, size_t len);
static int load_config(const char *filename);
static void display_stats(void);


int main(int argc, char *argv[]) {
    // Initialize the Environment Abstraction Layer (EAL)
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
    }

    argc -= ret;
    argv += ret;

    // Load the configuration
    if (load_config("ospf_sim.conf") != 0) {
        rte_exit(EXIT_FAILURE, "Error loading configuration\n");
    }

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

    // Send a hello packet from each virtual interface
    for (int i = 0; i < g_ospf_simulator.num_router_instances; i++) {
        for (int j = 0; j < g_ospf_simulator.router_instances[i].num_virtual_interfaces; j++) {
            send_hello_packet(&g_ospf_simulator.router_instances[i].virtual_interfaces[j], mbuf_pool);
        }
    }

    // Main processing loop
    uint64_t last_stats_display = rte_rdtsc();
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

        uint64_t now = rte_rdtsc();
        if ((now - last_stats_display) > rte_get_tsc_hz()) {
            display_stats();
            last_stats_display = now;
        }
    }

    return 0;
}

static void send_hello_packet(struct ospf_virtual_interface *vif, struct rte_mempool *mbuf_pool) {
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
    eth_hdr->src_addr = vif->mac_addr;
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
    ip_hdr->src_addr = vif->ip_address;
    inet_pton(AF_INET, OSPF_ALL_SPFRouters, &ip_hdr->dst_addr);

    // OSPF Hello packet
    struct ospf_hello_packet *hello_pkt = (struct ospf_hello_packet *)(ip_hdr + 1);
    hello_pkt->header.version = OSPF_VERSION;
    hello_pkt->header.type = OSPF_HELLO;
    hello_pkt->header.packet_length = rte_cpu_to_be_16(sizeof(struct ospf_hello_packet));
    hello_pkt->header.router_id = vif->router->router_id;
    hello_pkt->header.area_id = rte_cpu_to_be_32(0);
    hello_pkt->header.checksum = 0;
    hello_pkt->header.auth_type = 0;
    hello_pkt->header.auth_data = 0;
    hello_pkt->network_mask = vif->network_mask;
    hello_pkt->hello_interval = rte_cpu_to_be_16(10);
    hello_pkt->options = 0x02;
    hello_pkt->router_priority = vif->router_priority;
    hello_pkt->dead_interval = rte_cpu_to_be_32(40);
    hello_pkt->designated_router = vif->designated_router;
    hello_pkt->backup_router = vif->backup_router;

    // Checksums
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
    hello_pkt->header.checksum = ospf_checksum(hello_pkt, sizeof(struct ospf_hello_packet));

    // Send the packet
    const uint16_t nb_tx = rte_eth_tx_burst(vif->port_id, 0, &mbuf, 1);
    if (nb_tx > 0) {
        vif->packets_sent++;
    } else {
        rte_pktmbuf_free(mbuf);
    }
}

static void process_packet(struct rte_mbuf *mbuf, uint16_t port_id) {
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    struct ospf_virtual_interface *vif = find_virtual_interface(port_id, &eth_hdr->dst_addr);

    if (!vif) {
        return;
    }

    if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        return;
    }

    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

    if (ip_hdr->next_proto_id != IP_PROTOCOL_OSPF) {
        return;
    }

    struct ospf_header *ospf_hdr = (struct ospf_header *)(ip_hdr + 1);

    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS, MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

    switch (ospf_hdr->type) {
        case OSPF_HELLO:
            ospf_handle_hello_packet((struct ospf_hello_packet *)ospf_hdr, vif, mbuf_pool);
            break;
        case OSPF_DD:
            ospf_handle_dd_packet((struct ospf_dd_packet *)ospf_hdr, vif, mbuf_pool);
            break;
        case OSPF_LSR:
            ospf_handle_lsr_packet((struct ospf_lsr_packet *)ospf_hdr, vif, mbuf_pool);
            break;
        case OSPF_LSU:
            ospf_handle_lsu_packet((struct ospf_lsu_packet *)ospf_hdr, vif, mbuf_pool);
            break;
        case OSPF_LSACK:
            ospf_handle_lsack_packet((struct ospf_lsack_packet *)ospf_hdr, vif, mbuf_pool);
            break;
    }
}

static void ospf_handle_hello_packet(struct ospf_hello_packet *hello_pkt, struct ospf_virtual_interface *vif, struct rte_mempool *mbuf_pool) {
    vif->packets_received++;
    struct ospf_neighbor *neighbor = NULL;

    for (int i = 0; i < vif->num_neighbors; i++) {
        if (vif->neighbors[i].id == hello_pkt->header.router_id) {
            neighbor = &vif->neighbors[i];
            break;
        }
    }

    if (!neighbor) {
        if (vif->num_neighbors >= OSPF_MAX_NEIGHBORS) {
            return;
        }
        neighbor = &vif->neighbors[vif->num_neighbors++];
        neighbor->id = hello_pkt->header.router_id;
        neighbor->router_priority = hello_pkt->router_priority;
        neighbor->state = DOWN;
        neighbor->interface = vif;
        neighbor->packets_received = 1;
    } else {
        neighbor->packets_received++;
    }

    if (neighbor->state == DOWN) {
        neighbor->state = INIT;
    }

    for (int i = 0; i < (rte_be_to_cpu_16(hello_pkt->header.packet_length) - sizeof(struct ospf_hello_packet)) / 4; i++) {
        if (hello_pkt->neighbors[i] == vif->router->router_id) {
            if (neighbor->state == INIT) {
                neighbor->state = TWO_WAY;
                run_dr_bdr_election(vif);
            }
            break;
        }
    }

    if (vif->designated_router == vif->router->router_id || vif->backup_router == vif->router->router_id) {
        if (neighbor->state == TWO_WAY) {
            neighbor->state = EXSTART;
            neighbor->dd_sequence_number = rte_be_to_cpu_32(vif->router->router_id);
            neighbor->is_master = 1;
            send_dd_packet(vif, mbuf_pool, neighbor);
        }
    }
}

static void send_dd_packet(struct ospf_virtual_interface *vif, struct rte_mempool *mbuf_pool, struct ospf_neighbor *neighbor) {
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
    eth_hdr->src_addr = vif->mac_addr;
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
    ip_hdr->src_addr = vif->ip_address;
    inet_pton(AF_INET, OSPF_ALL_SPFRouters, &ip_hdr->dst_addr);

    // OSPF DD packet
    struct ospf_dd_packet *dd_pkt = (struct ospf_dd_packet *)(ip_hdr + 1);
    dd_pkt->header.version = OSPF_VERSION;
    dd_pkt->header.type = OSPF_DD;
    dd_pkt->header.packet_length = rte_cpu_to_be_16(sizeof(struct ospf_dd_packet));
    dd_pkt->header.router_id = vif->router->router_id;
    dd_pkt->header.area_id = rte_cpu_to_be_32(0);
    dd_pkt->header.checksum = 0;
    dd_pkt->header.auth_type = 0;
    dd_pkt->header.auth_data = 0;
    dd_pkt->mtu = rte_cpu_to_be_16(1500);
    dd_pkt->options = 0x02;
    dd_pkt->db_description_bits = OSPF_DD_I_BIT | OSPF_DD_M_BIT | OSPF_DD_MS_BIT;
    dd_pkt->dd_sequence_number = rte_cpu_to_be_32(neighbor->dd_sequence_number);

    // Checksums
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
    dd_pkt->header.checksum = ospf_checksum(dd_pkt, sizeof(struct ospf_dd_packet));

    // Send the packet
    const uint16_t nb_tx = rte_eth_tx_burst(vif->port_id, 0, &mbuf, 1);
    if (nb_tx > 0) {
        vif->packets_sent++;
        neighbor->packets_sent++;
    } else {
        rte_pktmbuf_free(mbuf);
    }
}

static void run_dr_bdr_election(struct ospf_virtual_interface *vif) {
    uint32_t dr = 0;
    uint32_t bdr = 0;
    uint8_t dr_priority = 0;
    uint8_t bdr_priority = 0;

    if (vif->router_priority > bdr_priority) {
        bdr = vif->router->router_id;
        bdr_priority = vif->router_priority;
    }
    for (int i = 0; i < vif->num_neighbors; i++) {
        struct ospf_neighbor *neighbor = &vif->neighbors[i];
        if (neighbor->state >= TWO_WAY) {
            if (neighbor->router_priority > bdr_priority) {
                bdr = neighbor->id;
                bdr_priority = neighbor->router_priority;
            } else if (neighbor->router_priority == bdr_priority && neighbor->id > bdr) {
                bdr = neighbor->id;
            }
        }
    }

    if (vif->router_priority > dr_priority && vif->router->router_id != bdr) {
        dr = vif->router->router_id;
        dr_priority = vif->router_priority;
    }
    for (int i = 0; i < vif->num_neighbors; i++) {
        struct ospf_neighbor *neighbor = &vif->neighbors[i];
        if (neighbor->state >= TWO_WAY && neighbor->id != bdr) {
            if (neighbor->router_priority > dr_priority) {
                dr = neighbor->id;
                dr_priority = neighbor->router_priority;
            } else if (neighbor->router_priority == dr_priority && neighbor->id > dr) {
                dr = neighbor->id;
            }
        }
    }

    vif->designated_router = dr;
    vif->backup_router = bdr;
}

static void ospf_handle_dd_packet(struct ospf_dd_packet *dd_pkt, struct ospf_virtual_interface *vif, struct rte_mempool *mbuf_pool) {
    vif->packets_received++;
    struct ospf_neighbor *neighbor = NULL;

    for (int i = 0; i < vif->num_neighbors; i++) {
        if (vif->neighbors[i].id == dd_pkt->header.router_id) {
            neighbor = &vif->neighbors[i];
            break;
        }
    }

    if (!neighbor) {
        return;
    }
    neighbor->packets_received++;

    if (neighbor->state == EXSTART) {
        if ((dd_pkt->db_description_bits & (OSPF_DD_I_BIT | OSPF_DD_M_BIT | OSPF_DD_MS_BIT)) == (OSPF_DD_I_BIT | OSPF_DD_M_BIT | OSPF_DD_MS_BIT)) {
            if (rte_be_to_cpu_32(vif->router->router_id) > rte_be_to_cpu_32(neighbor->id)) {
                neighbor->is_master = 1;
                neighbor->dd_sequence_number = rte_be_to_cpu_32(vif->router->router_id);
                send_dd_packet(vif, mbuf_pool, neighbor);
            } else {
                neighbor->is_master = 0;
                neighbor->dd_sequence_number = rte_be_to_cpu_32(dd_pkt->dd_sequence_number);
                send_dd_packet(vif, mbuf_pool, neighbor);
            }
            neighbor->state = EXCHANGE;
        }
    }

    if (neighbor->state == EXCHANGE) {
        if (neighbor->is_master) {
            if (dd_pkt->dd_sequence_number == rte_cpu_to_be_32(neighbor->dd_sequence_number)) {
                neighbor->dd_sequence_number++;
            }
        } else {
            if (dd_pkt->dd_sequence_number == rte_cpu_to_be_32(neighbor->dd_sequence_number + 1)) {
                neighbor->dd_sequence_number = rte_be_to_cpu_32(dd_pkt->dd_sequence_number);
            }
        }
    }

    if (!(dd_pkt->db_description_bits & OSPF_DD_M_BIT)) {
        neighbor->state = LOADING;
    }
}

static void ospf_handle_lsr_packet(struct ospf_lsr_packet *lsr_pkt, struct ospf_virtual_interface *vif, struct rte_mempool *mbuf_pool) {}
static void ospf_handle_lsu_packet(struct ospf_lsu_packet *lsu_pkt, struct ospf_virtual_interface *vif, struct rte_mempool *mbuf_pool) {}
static void ospf_handle_lsack_packet(struct ospf_lsack_packet *lsack_pkt, struct ospf_virtual_interface *vif, struct rte_mempool *mbuf_pool) {}

static struct ospf_virtual_interface *find_virtual_interface(uint16_t port_id, struct rte_ether_addr *mac) {
    for (int i = 0; i < g_ospf_simulator.num_router_instances; i++) {
        for (int j = 0; j < g_ospf_simulator.router_instances[i].num_virtual_interfaces; j++) {
            struct ospf_virtual_interface *vif = &g_ospf_simulator.router_instances[i].virtual_interfaces[j];
            if (vif->port_id == port_id && rte_is_same_ether_addr(&vif->mac_addr, mac)) {
                return vif;
            }
        }
    }
    return NULL;
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

static inline int port_init(uint16_t port, struct rte_mempool *mbuf_pool) {
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

    retval = rte_eth_promiscuous_enable(port);
    if (retval != 0)
        return retval;

    return 0;
}

static int load_config(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        return -1;
    }

    char line[256];
    struct ospf_router_instance *current_router = NULL;

    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }

        if (strncmp(line, "[router]", 8) == 0) {
            if (g_ospf_simulator.num_router_instances >= OSPF_MAX_ROUTER_INSTANCES) {
                break;
            }
            current_router = &g_ospf_simulator.router_instances[g_ospf_simulator.num_router_instances++];
        } else if (strncmp(line, "[interface]", 11) == 0) {
            if (current_router && current_router->num_virtual_interfaces < OSPF_MAX_VIRTUAL_INTERFACES) {
                struct ospf_virtual_interface *vif = &current_router->virtual_interfaces[current_router->num_virtual_interfaces++];
                vif->router = current_router;
            }
        } else {
            char *key = strtok(line, "=");
            char *value = strtok(NULL, "\n");

            if (key && value) {
                if (strcmp(key, "router_id") == 0) {
                    inet_pton(AF_INET, value, &current_router->router_id);
                } else if (strcmp(key, "port") == 0) {
                    current_router->virtual_interfaces[current_router->num_virtual_interfaces - 1].port_id = atoi(value);
                } else if (strcmp(key, "mac") == 0) {
                    sscanf(value, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                           &current_router->virtual_interfaces[current_router->num_virtual_interfaces - 1].mac_addr.addr_bytes[0],
                           &current_router->virtual_interfaces[current_router->num_virtual_interfaces - 1].mac_addr.addr_bytes[1],
                           &current_router->virtual_interfaces[current_router->num_virtual_interfaces - 1].mac_addr.addr_bytes[2],
                           &current_router->virtual_interfaces[current_router->num_virtual_interfaces - 1].mac_addr.addr_bytes[3],
                           &current_router->virtual_interfaces[current_router->num_virtual_interfaces - 1].mac_addr.addr_bytes[4],
                           &current_router->virtual_interfaces[current_router->num_virtual_interfaces - 1].mac_addr.addr_bytes[5]);
                } else if (strcmp(key, "ip") == 0) {
                    inet_pton(AF_INET, value, &current_router->virtual_interfaces[current_router->num_virtual_interfaces - 1].ip_address);
                } else if (strcmp(key, "netmask") == 0) {
                    inet_pton(AF_INET, value, &current_router->virtual_interfaces[current_router->num_virtual_interfaces - 1].network_mask);
                }
            }
        }
    }

    fclose(file);
    return 0;
}

static void display_stats(void) {
    printf("\033[2J\033[1;1H"); // Clear screen
    printf("OSPF Simulator Statistics\n");
    printf("=========================\n\n");

    for (int i = 0; i < g_ospf_simulator.num_router_instances; i++) {
        struct ospf_router_instance *router = &g_ospf_simulator.router_instances[i];
        struct in_addr router_id_addr = { .s_addr = router->router_id };
        printf("Router ID: %s\n", inet_ntoa(router_id_addr));

        for (int j = 0; j < router->num_virtual_interfaces; j++) {
            struct ospf_virtual_interface *vif = &router->virtual_interfaces[j];
            struct in_addr ip_addr = { .s_addr = vif->ip_address };
            printf("  Interface: %s\n", inet_ntoa(ip_addr));
            printf("    Packets Sent: %lu\n", vif->packets_sent);
            printf("    Packets Received: %lu\n", vif->packets_received);
            printf("    Neighbors:\n");

            for (int k = 0; k < vif->num_neighbors; k++) {
                struct ospf_neighbor *neighbor = &vif->neighbors[k];
                struct in_addr neighbor_id_addr = { .s_addr = neighbor->id };
                printf("      Neighbor ID: %s, State: %d, Sent: %lu, Rcvd: %lu\n",
                       inet_ntoa(neighbor_id_addr), neighbor->state, neighbor->packets_sent, neighbor->packets_received);
            }
        }
        printf("\n");
    }
}
