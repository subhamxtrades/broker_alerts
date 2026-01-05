#include "route_storm_ospf.h"
#include "aticara.h"
#include "misc.h"
#include "headers.h"

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include <rte_mempool.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_jhash.h>

/* Globals */
ospf_session_t ospf_sessions[RTE_MAX_ETHPORTS];

/* ---------- Helpers ---------- */

const char* ip_to_string(uint32_t ip_net)
{
    /* __thread ensures each thread has its own buffer, preventing race conditions */
    static __thread char buf[4][16];
    static __thread int idx = 0;
    char *out = buf[idx];
    idx = (idx + 1) % 4;

    uint32_t ip = ntohl(ip_net);
    snprintf(out, 16, "%u.%u.%u.%u",
        (ip >> 24) & 0xff,
        (ip >> 16) & 0xff,
        (ip >> 8)  & 0xff,
        ip & 0xff);
    return out;
}

const char* ospf_state_to_string(ospf_state_t s)
{
  static const char *names[] = {
    "DOWN","ATTEMPT","INIT","2-WAY",
    "EXSTART","EXCHANGE","LOADING","FULL"
  };
  if (s > OSPF_STATE_FULL) return "UNKNOWN";
  return names[s];
}

uint32_t string_to_ip(const char *s)
{
  struct in_addr a;
  if (inet_pton(AF_INET, s, &a) != 1) {
    printf("Invalid IP string: %s\n", s);
    return 0;
  }
  return a.s_addr; /* network order */
}

/* ---------- Hash table helper ---------- */

struct rte_hash* ospf_create_hash_table(const char *name, uint32_t entries)
{
  struct rte_hash_parameters p = {
    .name = name,
    .entries = entries,
    .key_len = sizeof(uint64_t),
    .hash_func = rte_jhash,
    .hash_func_init_val = 0,
    .socket_id = rte_socket_id()
  };
  struct rte_hash *h = rte_hash_create(&p);
  if (!h) {
    printf("Failed to create hash '%s'\n", name);
  } else {
    printf("Successfully created hash table '%s' with %u entries\n",
        name, entries);
  }
  return h;
}

/* ---------- Checksums ---------- */

uint16_t ospf_checksum(struct ospf_header *hdr, uint16_t length)
{
    uint32_t sum = 0;
    uint16_t saved = hdr->checksum;
    hdr->checksum = 0;

    uint8_t *data = (uint8_t *)hdr;
    uint16_t i = 0;

    // Process 16-bit chunks
    while (length > 1) {
        sum += ((uint16_t)data[i] << 8) | data[i+1];
        i += 2;
        length -= 2;
    }

    // Process final odd byte if any
    if (length > 0) {
        sum += (uint16_t)data[i] << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }

    hdr->checksum = saved;
    return (uint16_t)(~sum);
}

uint16_t ospf_lsa_checksum(struct ospf_lsa_header *lsa) {
    uint16_t saved_checksum = lsa->checksum;
    lsa->checksum = 0;

    int len = ntohs(lsa->length);
    uint8_t *data = (uint8_t *)lsa;

    // Start from byte 2 (options field), skipping LS Age
    uint8_t *p = data + 2;
    int data_len = len - 2;

    uint32_t c0 = 0, c1 = 0;
    int i;
    for (i = 0; i < data_len; i++) {
        c0 = c0 + p[i];
        c1 += c0;
    }
    c0 %= 255;
    c1 %= 255;

    int x = (data_len * c0 - c1) % 255;
    if (x <= 0) {
        x += 255;
    }
    int y = 510 - c0 - x;
    if (y > 255) {
        y -= 255;
    }

    uint16_t checksum = (x << 8) | y;

    lsa->checksum = saved_checksum;

    return checksum;
}

/* ---------- Cleanup / init ---------- */

void ospf_cleanup_session(uint8_t pid)
{
  if (pid >= RTE_MAX_ETHPORTS) return;

  ospf_session_t *s = &ospf_sessions[pid];

  printf("Cleaning up OSPF session for PID %u\n", pid);

  if (s->lsdb) { rte_hash_free(s->lsdb); s->lsdb = NULL; }
  if (s->rib)  { rte_hash_free(s->rib);  s->rib  = NULL; }
  if (s->fib)  { rte_hash_free(s->fib);  s->fib  = NULL; }

  memset(s, 0, sizeof(*s));
  s->pid = pid;
  s->state = OSPF_STATE_DOWN;
}

/*
 * Basic Configuration of Interface - FIXED AS POINT-TO-POINT
 * */
int ospf_initialize_test(uint8_t pid, uint32_t router_id, uint32_t area_id)
{
  ospf_cleanup_session(pid);

  ospf_session_t *s = &ospf_sessions[pid];
  memset(s, 0, sizeof(*s));
  s->pid = pid;
  s->lid = rte_lcore_id();

  /* Set Router IDs based on your topology */
  if (router_id == 0) {
    switch (pid) {
      case 0: s->router_id = string_to_ip("2.2.2.2"); break;    /* Your router PID 0 */
      case 1: s->router_id = string_to_ip("2.2.2.2"); break;    /* Your router PID 1 */
      default: s->router_id = string_to_ip("192.168.99.100"); break;
    }
  } else {
    s->router_id = router_id;  /* Use provided Router ID */
  }

  s->area_id   = area_id ? area_id : string_to_ip("0.0.0.0");
  s->state = OSPF_STATE_DOWN;

  s->config.router_id = s->router_id;
  s->config.area_id   = s->area_id;
  s->config.hello_interval = OSPF_HELLO_INTERVAL;
  s->config.dead_interval  = OSPF_DEAD_INTERVAL;
  s->config.priority = 1;
  s->config.options  = OSPF_OPTION_E;
  s->config.network_mask = string_to_ip("255.255.255.0");
  s->config.designated_router = 0;
  s->config.backup_dr = 0;

  /* Initialize interfaces based on your topology */
  s->interface_count = 1;  /* Each PID has 1 interface */

  if (pid == 0) {
    /* PID 0: Interface connected to FRR port 1 (192.168.1.1) */
    ospf_interface_t *iface = &s->interfaces[0];
    memset(iface, 0, sizeof(*iface));
    iface->ip_address = string_to_ip("192.168.1.100");
    iface->network_mask = s->config.network_mask;
    iface->area_id = s->area_id;
    iface->type = OSPF_IFTYPE_P2P;  /* FIXED: POINT-TO-POINT */
    iface->hello_interval = OSPF_HELLO_INTERVAL;
    iface->dead_interval  = OSPF_DEAD_INTERVAL;
    iface->priority = 1;
    iface->options  = OSPF_OPTION_E;
    iface->cost = 10;
    iface->state = 1;  /* up */
    iface->designated_router = 0;
    iface->backup_dr = 0;
    iface->is_passive = 0;  /* Active interface */
    printf("[OSPF PID%u] Configured interface as POINT-TO-POINT (not BROADCAST)\n", pid);
  } else if (pid == 1) {
    /* PID 1: Interface connected to FRR port 2 (192.168.2.1) */
    ospf_interface_t *iface = &s->interfaces[0];
    memset(iface, 0, sizeof(*iface));
    iface->ip_address = string_to_ip("192.168.2.100");
    iface->network_mask = s->config.network_mask;
    iface->area_id = s->area_id;
    iface->type = OSPF_IFTYPE_P2P;  /* FIXED: POINT-TO-POINT */
    iface->hello_interval = OSPF_HELLO_INTERVAL;
    iface->dead_interval  = OSPF_DEAD_INTERVAL;
    iface->priority = 1;
    iface->options  = OSPF_OPTION_E;
    iface->cost = 10;
    iface->state = 1;  /* up */
    iface->designated_router = 0;
    iface->backup_dr = 0;
    iface->is_passive = 0;  /* Active interface */
    printf("[OSPF PID%u] Configured interface as POINT-TO-POINT (not BROADCAST)\n", pid);
  }

  char lsdb_name[32], rib_name[32], fib_name[32];
  snprintf(lsdb_name, sizeof(lsdb_name), "lsdb_%u", pid);
  snprintf(rib_name, sizeof(rib_name), "rib_%u", pid);
  snprintf(fib_name, sizeof(fib_name), "fib_%u", pid);

  s->lsdb = ospf_create_hash_table(lsdb_name, 512);
  s->rib  = ospf_create_hash_table(rib_name, 1024);
  s->fib  = ospf_create_hash_table(fib_name, 1024);

  if (!s->lsdb || !s->rib || !s->fib) {
    printf("Failed to create OSPF hash tables for PID %u\n", pid);
    ospf_cleanup_session(pid);
    return -1;
  }

  printf("OSPF Test Initialized for PID %u:\n", pid);
  printf("  Router ID: %s\n", ip_to_string(s->router_id));
  printf("  Area: %s\n", ip_to_string(s->area_id));
  char ip_str[16], mask_str[16];
  snprintf(ip_str, sizeof(ip_str), "%s", ip_to_string(s->interfaces[0].ip_address));
  snprintf(mask_str, sizeof(mask_str), "%s", ip_to_string(s->config.network_mask));
  printf("  Interface: %s/%s (Point-to-Point)\n", ip_str, mask_str);
  printf("  Interface Type: POINT-TO-POINT (no DR/BDR election)\n");

  return 0;
}

/* ---------- TX: generic OSPF packet ---------- */

int ospf_send_packet(uint8_t pid, ospf_session_t *s, uint8_t type,
    void *payload, uint16_t payload_len, uint32_t dst_ip,
    uint32_t src_ip)
{
  port_info_t *info = &aticara.info[pid];
  uint8_t lid = rte_lcore_id();
  int qid = wr_get_txque(aticara.l2p, lid, pid);

  struct rte_mbuf *m = rte_pktmbuf_alloc(info->q[qid].tx_mp);
  if (!m) {
    PRINT_LOG("OSPF: failed to alloc mbuf for type %u\n", type);
    return -1;
  }

  uint16_t ospf_len = sizeof(struct ospf_header) + payload_len;
  uint16_t ip_len   = sizeof(struct iphdr) + ospf_len;
  uint16_t frame_len = sizeof(struct ethernet_hdr) + ip_len;

  rte_pktmbuf_append(m, frame_len);

  struct ethernet_hdr *eth = rte_pktmbuf_mtod(m, struct ethernet_hdr *);
  struct iphdr *ip = (struct iphdr *)(eth + 1);
  struct ospf_header *hdr = (struct ospf_header *)(ip + 1);

  /* Ethernet */
  uint8_t dst_mac[6];

  if (dst_ip == string_to_ip(OSPF_ALLSPFROUTERS_MCAST)) {
      memcpy(dst_mac, (uint8_t[]){0x01, 0x00, 0x5e, 0x00, 0x00, 0x05}, 6);
  } else {
      int found = 0;
      for (int i = 0; i < s->neighbor_count; i++) {
          if (s->neighbors[i].ip_address == dst_ip) {
              memcpy(dst_mac, s->neighbors[i].mac_addr, 6);
              found = 1;
              break;
          }
      }
      if (!found) {
          printf("[OSPF PID%u] ERROR: No MAC address found for neighbor %s\n", pid, ip_to_string(dst_ip));
          // Fallback to broadcast for initial discovery
          memset(dst_mac, 0xff, 6);
      }
  }

  uint8_t src_mac[6];
  switch (pid) {
    case 0: memcpy(src_mac, (uint8_t[]){0x00,0x11,0x22,0x33,0x44,0x01}, 6); break;
    case 1: memcpy(src_mac, (uint8_t[]){0x00,0x11,0x22,0x33,0x44,0x02}, 6); break;
    default: memcpy(src_mac, (uint8_t[]){0x00,0x11,0x22,0x33,0x44,0x99}, 6); break;
  }

  memcpy(eth->ether_dhost, dst_mac, 6);
  memcpy(eth->ether_shost, src_mac, 6);
  eth->ether_type = htons(ETHERTYPE_IP);

  /* IP header */
  memset(ip, 0, sizeof(*ip));
  ip->version = 4;
  ip->ihl = 5;
  ip->tos = 0xc0;
  ip->tot_len = htons(ip_len);
  ip->id = htons((uint16_t)(rte_rand() & 0xffff));
  ip->frag_off = htons(IP_DF);
  ip->ttl = 1;
  ip->protocol = OSPF_PROTOCOL_NUMBER;
  ip->saddr = src_ip;
  ip->daddr = dst_ip;
  ip->check = ipchksum((uint16_t *)ip, sizeof(*ip));

  /* OSPF header */
  memset(hdr, 0, sizeof(*hdr));
  hdr->version  = OSPF_VERSION;
  hdr->type     = type;
  hdr->router_id = s->router_id;   /* net order */
  hdr->area_id   = s->area_id;     /* net order */
  hdr->auth_type = 0;
  hdr->auth_data = 0;
  hdr->length    = htons(ospf_len);

  if (payload && payload_len)
    memcpy(hdr + 1, payload, payload_len);

  hdr->checksum = htons(ospf_checksum(hdr, ospf_len));

  m->pkt_len  = frame_len;
  m->data_len = frame_len;

  send_mbuf(m, pid, qid);

  if (pblast[pid].trafficCapture) {
    pblast_pcapdump(pid, (const u_char *)eth, frame_len);
  }

  const char *tname = "Unknown";
  switch (type) {
    case OSPF_TYPE_HELLO: tname = "Hello"; break;
    case OSPF_TYPE_DD:    tname = "DD";    break;
    case OSPF_TYPE_LSR:   tname = "LSR";   break;
    case OSPF_TYPE_LSU:   tname = "LSU";   break;
    case OSPF_TYPE_LSACK: tname = "LSAck"; break;
  }

  /* Store IP strings in local buffers to avoid undefined behavior */
  char rid_str[16], src_str[16], dst_str[16];
  snprintf(rid_str, sizeof(rid_str), "%s", ip_to_string(s->router_id));
  snprintf(src_str, sizeof(src_str), "%s", ip_to_string(src_ip));
  snprintf(dst_str, sizeof(dst_str), "%s", ip_to_string(dst_ip));

  printf("[OSPF PID%u] >>> SENT: %s from Router %s (%s) to %s, length=%u bytes\n",
      pid, tname, rid_str, src_str, dst_str, frame_len);

  switch (type) {
    case OSPF_TYPE_HELLO:
      s->config.hello_sent++;
      s->last_hello_sent = rte_get_tsc_cycles();
      break;
    case OSPF_TYPE_DD:
      s->config.dd_sent++;
      s->last_dd_sent = rte_get_tsc_cycles();
      break;
    case OSPF_TYPE_LSR:
      s->config.lsr_sent++;
      break;
    case OSPF_TYPE_LSU:
      s->config.lsu_sent++;
      s->last_lsa_sent = rte_get_tsc_cycles();
      break;
    case OSPF_TYPE_LSACK:
      s->config.lsack_sent++;
      break;
  }

  return 0;
}

/* ---------- TX: Hello ---------- */

int ospf_send_hello_packet(uint8_t pid, ospf_session_t *s, uint8_t iface_index)
{
  if (iface_index >= s->interface_count) return -1;

  ospf_interface_t *iface = &s->interfaces[iface_index];
  if (iface->is_passive) return 0;

  uint32_t frr_router_id = string_to_ip("1.1.1.1");

  /* Calculate actual neighbors on this interface */
  uint16_t neighbor_count = 0;
  for (int i = 0; i < s->neighbor_count; i++) {
    if (s->neighbors[i].interface_index == iface_index &&
        s->neighbors[i].state >= OSPF_STATE_INIT) {
      neighbor_count++;
    }
  }

  uint16_t hello_body_len = sizeof(struct ospf_hello) +
    neighbor_count * sizeof(uint32_t);
  uint8_t *buf = malloc(hello_body_len);
  if (!buf) return -1;

  struct ospf_hello *hello = (struct ospf_hello *)buf;
  memset(hello, 0, hello_body_len);

  hello->network_mask  = iface->network_mask;
  hello->hello_interval = htons(OSPF_HELLO_INTERVAL);
  hello->options        = OSPF_OPTION_E;
  hello->priority       = iface->priority;  /* 0 for point-to-point */
  hello->dead_interval  = htonl(OSPF_DEAD_INTERVAL);

  /* For point-to-point interfaces, set DR/BDR to 0.0.0.0 */
  hello->designated_router = 0;  /* 0.0.0.0 for point-to-point */
  hello->backup_dr         = 0;  /* 0.0.0.0 for point-to-point */

  uint32_t *nbr_ids = (uint32_t *)(hello + 1);
  int idx = 0;

  /* Add neighbors that are in INIT state or higher */
  for (int i = 0; i < s->neighbor_count; i++) {
    if (s->neighbors[i].interface_index == iface_index &&
        s->neighbors[i].state >= OSPF_STATE_INIT) {
      nbr_ids[idx++] = s->neighbors[i].router_id;
    }
  }

  /* Debug: Calculate expected packet size */
  uint16_t total_packet_size = sizeof(struct ethernet_hdr) +
    sizeof(struct iphdr) +
    sizeof(struct ospf_header) +
    hello_body_len;

  /* Store IP strings in local buffers to avoid undefined behavior */
  char iface_str[16], rid_str[16];
  snprintf(iface_str, sizeof(iface_str), "%s", ip_to_string(iface->ip_address));
  snprintf(rid_str, sizeof(rid_str), "%s", ip_to_string(s->router_id));

  printf("[OSPF PID%u] Sending Hello on point-to-point interface %s: Router %s, Neighbors: %u, Packet size=%u bytes\n",
      pid, iface_str, rid_str, neighbor_count, total_packet_size);

  if (neighbor_count > 0) {
    printf("[OSPF PID%u] Hello includes neighbor(s):", pid);
    for (int i = 0; i < neighbor_count; i++) {
      printf(" %s", ip_to_string(nbr_ids[i]));
    }
    printf("\n");
  }

  /* Per RFC 2328, Hello packets are sent to the AllSPFRouters multicast address. */
  uint32_t dst_ip = string_to_ip(OSPF_ALLSPFROUTERS_MCAST);

  int ret = ospf_send_packet(pid, s, OSPF_TYPE_HELLO,
      buf, hello_body_len, dst_ip,
      iface->ip_address);
  free(buf);

  iface->last_hello_sent = rte_get_tsc_cycles();

  return ret;
}

/* ---------- TX: DD / LSR / LSU / LSAck ---------- */

int ospf_send_dd_packet(uint8_t pid, ospf_session_t *s,
    uint32_t neighbor_rid, uint8_t flags, uint32_t dd_seq,
    bool include_lsa_headers, uint8_t iface_index)
{
  if (iface_index >= s->interface_count) return -1;
  ospf_interface_t *iface = &s->interfaces[iface_index];

  uint16_t dd_body_len = sizeof(struct ospf_dd);
  uint16_t lsa_header_len = 0;

  /* FIX: Calculate the correct total length */
  if (include_lsa_headers) {
    /* Generate LSA to get its size */
    uint8_t lsa_buf[OSPF_MAX_LSA_SIZE];
    struct ospf_lsa_header *lsa = (struct ospf_lsa_header *)lsa_buf;
    if (ospf_generate_router_lsa(s, lsa, iface_index) != 0) {
      printf("[OSPF PID%u] Failed to generate Router LSA\n", pid);
      return -1;
    }
    lsa_header_len = ntohs(lsa->length);
  }

  uint16_t total_len = dd_body_len + lsa_header_len;
  uint8_t *buf = malloc(total_len);
  if (!buf) return -1;

  struct ospf_dd *dd = (struct ospf_dd *)buf;
  memset(dd, 0, total_len);

  dd->mtu = htons(1500);

  dd->options = s->config.options;

  /* Set flags correctly */
  dd->flags = flags;

  dd->dd_sequence = htonl(dd_seq);

  /* Add LSA header if requested */
  if (include_lsa_headers && lsa_header_len > 0) {
    struct ospf_lsa_header *lsa = (struct ospf_lsa_header *)(dd + 1);
    if (ospf_generate_router_lsa(s, lsa, iface_index) != 0) {
      printf("[OSPF PID%u] Failed to generate Router LSA for DD\n", pid);
      free(buf);
      return -1;
    }
  }

  /* Unicast DD packets to the neighbor */
  ospf_neighbor_t *neighbor = NULL;
  for (int i = 0; i < s->neighbor_count; i++) {
    if (s->neighbors[i].router_id == neighbor_rid && s->neighbors[i].interface_index == iface_index) {
      neighbor = &s->neighbors[i];
      break;
    }
  }

  if (!neighbor) {
    printf("[OSPF PID%u] ERROR: Cannot send DD to unknown neighbor %s\n", pid, ip_to_string(neighbor_rid));
    free(buf);
    return -1;
  }
  uint32_t dst_ip = neighbor->ip_address;

  /* Store IP strings in local buffers to avoid undefined behavior with ip_to_string */
  char rid_str[16], dst_str[16], iface_str[16];
  snprintf(rid_str, sizeof(rid_str), "%s", ip_to_string(neighbor_rid));
  snprintf(dst_str, sizeof(dst_str), "%s", ip_to_string(dst_ip));
  snprintf(iface_str, sizeof(iface_str), "%s", ip_to_string(iface->ip_address));

  printf("[OSPF PID%u] Sending DD to %s (%s) flags 0x%02x seq %u on %s interface %s\n",
      pid, rid_str, dst_str, dd->flags, dd_seq,
      iface->type == OSPF_IFTYPE_P2P ? "P2P" : "BROADCAST",
      iface_str);
  printf("[OSPF PID%u] DD Send Flags: I=%u, M=%u, MS=%u, MTU=%u, Total len=%u\n",
      pid,
      (dd->flags & OSPF_DD_FLAG_I) ? 1 : 0,
      (dd->flags & OSPF_DD_FLAG_M) ? 1 : 0,
      (dd->flags & OSPF_DD_FLAG_MS) ? 1 : 0,
      ntohs(dd->mtu),
      total_len);

  int ret = ospf_send_packet(pid, s, OSPF_TYPE_DD, buf, total_len, dst_ip, iface->ip_address);
  free(buf);
  return ret;
}

int ospf_send_lsr_packet(uint8_t pid, ospf_session_t *s,
    uint32_t neighbor_rid, uint32_t ls_type,
    uint32_t link_state_id, uint32_t adv_router, uint8_t iface_index)
{
  if (iface_index >= s->interface_count) return -1;
  ospf_interface_t *iface = &s->interfaces[iface_index];

  struct ospf_lsr lsr;
  memset(&lsr, 0, sizeof(lsr));
  lsr.ls_type           = htonl(ls_type);
  lsr.link_state_id     = link_state_id;     /* already net order */
  lsr.advertising_router = adv_router;      /* net order */

  /* Unicast LSR packets to the neighbor */
  ospf_neighbor_t *neighbor = NULL;
  for (int i = 0; i < s->neighbor_count; i++) {
    if (s->neighbors[i].router_id == neighbor_rid && s->neighbors[i].interface_index == iface_index) {
      neighbor = &s->neighbors[i];
      break;
    }
  }

  if (!neighbor) {
    printf("[OSPF PID%u] ERROR: Cannot send LSR to unknown neighbor %s\n", pid, ip_to_string(neighbor_rid));
    return -1;
  }
  uint32_t dst_ip = neighbor->ip_address;

  /* Store IP strings in local buffers to avoid undefined behavior */
  char rid_str[16], dst_str[16], iface_str[16];
  snprintf(rid_str, sizeof(rid_str), "%s", ip_to_string(neighbor_rid));
  snprintf(dst_str, sizeof(dst_str), "%s", ip_to_string(dst_ip));
  snprintf(iface_str, sizeof(iface_str), "%s", ip_to_string(iface->ip_address));

  printf("[OSPF PID%u] Sending LSR to %s (%s) for LSA type %u on point-to-point interface %s\n",
      pid, rid_str, dst_str, ls_type, iface_str);

  return ospf_send_packet(pid, s, OSPF_TYPE_LSR, &lsr, sizeof(lsr), dst_ip, iface->ip_address);
}

int ospf_generate_router_lsa(ospf_session_t *s, struct ospf_lsa_header *lsa, uint8_t iface_index)
{
    if (s->lsa_seq_num == 0) s->lsa_seq_num = 0x80000001;

    ospf_interface_t *iface = &s->interfaces[iface_index];

    /* A router LSA should have one link for each neighbor in state 2-Way or higher */
    uint16_t num_links = 0;
    for (int i = 0; i < s->neighbor_count; i++) {
        if (s->neighbors[i].state >= OSPF_STATE_TWO_WAY) {
            num_links++;
        }
    }

    uint16_t lsa_length = sizeof(struct ospf_lsa_header) + 4 + (num_links * sizeof(struct ospf_router_lsa_link));
    memset(lsa, 0, lsa_length);

    struct ospf_lsa_header *hdr = (struct ospf_lsa_header *)lsa;
    hdr->age = htons(0);
    hdr->options = OSPF_OPTION_E;
    hdr->type = LSA_TYPE_ROUTER;
    hdr->link_state_id = s->router_id;
    hdr->advertising_router = s->router_id;
    hdr->sequence_number = htonl(s->lsa_seq_num++);
    hdr->length = htons(lsa_length);

    uint16_t *lsa_body = (uint16_t *)(hdr + 1);
    lsa_body[0] = htons(OSPF_LSA_ROUTER_FLAG_E); /* This router is an ASBR */
    lsa_body[1] = htons(num_links);

    if (num_links > 0) {
        struct ospf_router_lsa_link *link = (struct ospf_router_lsa_link *)(lsa_body + 2);
        int current_link = 0;
        for (int i = 0; i < s->neighbor_count; i++) {
            if (s->neighbors[i].state >= OSPF_STATE_TWO_WAY) {
                link[current_link].link_id = s->neighbors[i].router_id;
                link[current_link].link_data = iface->ip_address;
                link[current_link].type = 1; // P2P
                link[current_link].num_tos = 0;
                link[current_link].metric = htons(iface->cost);
                current_link++;
            }
        }
    }

    hdr->checksum = ospf_lsa_checksum(hdr);

    return 0;
}

int ospf_generate_network_lsa(ospf_session_t *s, struct ospf_lsa_header *lsa, uint8_t iface_index)
{
    if (s->lsa_seq_num == 0) s->lsa_seq_num = 0x80000001;

    ospf_interface_t *iface = &s->interfaces[iface_index];
    uint16_t num_routers = 0;
    for (int i = 0; i < s->neighbor_count; i++) {
        if (s->neighbors[i].state >= OSPF_STATE_TWO_WAY) {
            num_routers++;
        }
    }
    num_routers++; // Add ourselves

    uint16_t lsa_length = sizeof(struct ospf_lsa_header) + 4 + (num_routers * 4);
    memset(lsa, 0, lsa_length);

    lsa->age = htons(0);
    lsa->options = OSPF_OPTION_E;
    lsa->type = LSA_TYPE_NETWORK;
    lsa->link_state_id = iface->ip_address;
    lsa->advertising_router = s->router_id;
    lsa->sequence_number = htonl(s->lsa_seq_num++);
    lsa->length = htons(lsa_length);

    uint32_t *lsa_body = (uint32_t *)(lsa + 1);
    lsa_body[0] = iface->network_mask;
    int current_router = 1;
    lsa_body[current_router++] = s->router_id;

    for (int i = 0; i < s->neighbor_count; i++) {
        if (s->neighbors[i].state >= OSPF_STATE_TWO_WAY) {
            lsa_body[current_router++] = s->neighbors[i].router_id;
        }
    }

    lsa->checksum = 0;
    lsa->checksum = ospf_lsa_checksum(lsa);

    printf("[OSPF PID%u] Generated Network LSA with %u routers\n", s->pid, num_routers);
    return 0;
}

int ospf_send_lsu_packet(uint8_t pid, ospf_session_t *s,
    uint32_t neighbor_rid, uint8_t lsa_type,
    uint8_t lsa_count, struct ospf_lsa_header **lsas, uint8_t iface_index)
{
  if (iface_index >= s->interface_count) return -1;
  ospf_interface_t *iface = &s->interfaces[iface_index];

  /* Calculate total size */
  uint16_t total_len = sizeof(struct ospf_lsu);
  for (int i = 0; i < lsa_count; i++) {
    if (lsas[i]) {
      total_len += ntohs(lsas[i]->length);
    }
  }

  uint8_t *buf = malloc(total_len);
  if (!buf) return -1;

  struct ospf_lsu *lsu = (struct ospf_lsu *)buf;
  memset(lsu, 0, sizeof(*lsu));
  lsu->num_lsas = htonl(lsa_count);

  uint8_t *ptr = (uint8_t *)(lsu + 1);
  for (int i = 0; i < lsa_count; i++) {
    if (lsas[i]) {
      uint16_t len = ntohs(lsas[i]->length);
      memcpy(ptr, lsas[i], len);
      ptr += len;
    }
  }

  /* Unicast LSU packets to the neighbor */
  ospf_neighbor_t *neighbor = NULL;
  for (int i = 0; i < s->neighbor_count; i++) {
    if (s->neighbors[i].router_id == neighbor_rid && s->neighbors[i].interface_index == iface_index) {
      neighbor = &s->neighbors[i];
      break;
    }
  }

  if (!neighbor) {
    printf("[OSPF PID%u] ERROR: Cannot send LSU to unknown neighbor %s\n", pid, ip_to_string(neighbor_rid));
    free(buf);
    return -1;
  }
  uint32_t dst_ip = neighbor->ip_address;

  /* Store IP strings in local buffers to avoid undefined behavior */
  char rid_str[16], dst_str[16], iface_str[16];
  snprintf(rid_str, sizeof(rid_str), "%s", ip_to_string(neighbor_rid));
  snprintf(dst_str, sizeof(dst_str), "%s", ip_to_string(dst_ip));
  snprintf(iface_str, sizeof(iface_str), "%s", ip_to_string(iface->ip_address));

  printf("[OSPF PID%u] Sending LSU with %u LSA(s) to %s (%s) on point-to-point interface %s\n",
      pid, lsa_count, rid_str, dst_str, iface_str);

  int ret = ospf_send_packet(pid, s, OSPF_TYPE_LSU, buf, total_len, dst_ip, iface->ip_address);
  free(buf);
  return ret;
}

int ospf_send_lsack_packet(uint8_t pid, ospf_session_t *s,
    uint32_t neighbor_rid, struct ospf_lsa_header **lsas,
    uint8_t lsa_count, uint8_t iface_index)
{
    if (iface_index >= s->interface_count) return -1;
    ospf_interface_t *iface = &s->interfaces[iface_index];

    uint16_t payload_len = lsa_count * sizeof(struct ospf_lsa_header);
    uint8_t *buf = malloc(payload_len);
    if (!buf) return -1;

    for (int i = 0; i < lsa_count; i++) {
        memcpy(buf + (i * sizeof(struct ospf_lsa_header)), lsas[i], sizeof(struct ospf_lsa_header));
    }

    ospf_neighbor_t *neighbor = NULL;
    for (int i = 0; i < s->neighbor_count; i++) {
        if (s->neighbors[i].router_id == neighbor_rid && s->neighbors[i].interface_index == iface_index) {
            neighbor = &s->neighbors[i];
            break;
        }
    }

    if (!neighbor) {
        printf("[OSPF PID%u] ERROR: Cannot send LSAck to unknown neighbor %s\n", pid, ip_to_string(neighbor_rid));
        free(buf);
        return -1;
    }
    uint32_t dst_ip = neighbor->ip_address;

    printf("[OSPF PID%u] Sending LSAck to %s for %u LSAs\n", pid, ip_to_string(neighbor_rid), lsa_count);

    int ret = ospf_send_packet(pid, s, OSPF_TYPE_LSACK, buf, payload_len, dst_ip, iface->ip_address);
    free(buf);
    return ret;
}

/* Helper to check if DD packet contains LSA headers */
bool dd_contains_lsa_headers(struct ospf_header *hdr, struct ospf_dd *dd)
{
  uint16_t ospf_len = ntohs(hdr->length);
  uint16_t header_len = sizeof(struct ospf_header) + sizeof(struct ospf_dd);

  /* If OSPF length is greater than header + DD, it contains LSA headers */
  return (ospf_len > header_len);
}

/* ---------- Neighbor state helper ---------- */

void ospf_update_neighbor_state(ospf_session_t *s,
    uint32_t neighbor_rid,
    uint8_t interface_index,
    ospf_state_t new_state)
{
  for (int i = 0; i < s->neighbor_count; i++) {
    if (s->neighbors[i].router_id == neighbor_rid &&
        s->neighbors[i].interface_index == interface_index) {
      ospf_state_t old = s->neighbors[i].state;
      if (old == new_state) return;
      s->neighbors[i].state = new_state;

    if (new_state <= OSPF_STATE_EXSTART) {
        s->neighbors[i].initial_dd_sent = 0;
        s->neighbors[i].dd_sequence = (rte_rand() & 0xffffff);
    }

      char rid_str[16];
      snprintf(rid_str, sizeof(rid_str), "%s", ip_to_string(neighbor_rid));
      printf("[OSPF PID%u] Neighbor %s (interface %u) state: %s -> %s\n",
          s->pid,
          rid_str,
          interface_index,
          ospf_state_to_string(old),
          ospf_state_to_string(new_state));

      if (new_state == OSPF_STATE_FULL) {
        s->config.neighbors_full++;
        char rid_str[16];
        snprintf(rid_str, sizeof(rid_str), "%s", ip_to_string(neighbor_rid));
        printf("[OSPF PID%u] *** Adjacency with %s established (FULL) ***\n",
            s->pid, rid_str);
      }
      return;
    }
  }
}

/* ---------- Step-by-step exchange control ---------- */

void ospf_start_exchange(ospf_session_t *s, uint8_t iface_index)
{
  ospf_interface_t *iface = &s->interfaces[iface_index];
  iface->exchange_state.step = 0;
  iface->exchange_state.waiting_for = 0;
  iface->exchange_state.wait_until = 0;
  iface->exchange_state.retry_count = 0;

  printf("[OSPF PID%u] Exchange system ready on point-to-point interface %s\n",
      s->pid, ip_to_string(iface->ip_address));
}

int ospf_process_exchange_steps(ospf_session_t *s, uint8_t pid)
{
  uint64_t now = rte_get_tsc_cycles();

  for (int if_idx = 0; if_idx < s->interface_count; if_idx++) {
    ospf_interface_t *iface = &s->interfaces[if_idx];

    if (iface->exchange_state.step == 0) {
      continue;
    }

    if (iface->exchange_state.wait_until > now) {
      continue;
    }

    /* Process timeout */
    switch (iface->exchange_state.step) {
      case 6: /* Waiting to send LSR */
        printf("[OSPF PID%u] Step 6: Sending LSR\n", pid);
        ospf_send_lsr_packet(pid, s, string_to_ip("1.1.1.1"),
            LSA_TYPE_ROUTER, s->router_id, s->router_id, if_idx);
        iface->exchange_state.step = 7;
        iface->exchange_state.wait_until = now + (2 * rte_get_tsc_hz());
        break;

      case 7: /* Waiting to send LSU */
        printf("[OSPF PID%u] Step 7: Sending LSU\n", pid);
        uint8_t lsa_buf[OSPF_MAX_LSA_SIZE];
        struct ospf_lsa_header *lsa = (struct ospf_lsa_header *)lsa_buf;
        ospf_generate_router_lsa(s, lsa, if_idx);
        struct ospf_lsa_header *lsa_ptr = lsa;
        ospf_send_lsu_packet(pid, s, string_to_ip("1.1.1.1"),
            LSA_TYPE_ROUTER, 1, &lsa_ptr, if_idx);
        iface->exchange_state.step = 8;
        iface->exchange_state.wait_until = now + (1 * rte_get_tsc_hz());
        break;

      case 8: /* Waiting to send LSAck */
        printf("[OSPF PID%u] Step 8: Sending LSAck\n", pid);
        iface->exchange_state.step = 0;
        break;

      default:
        iface->exchange_state.step = 0;
        break;
    }
  }

  return 0;
}

/* ---------- RX: Hello ---------- */

int ospf_handle_hello_packet(struct ethernet_hdr *eth_hdr,
    struct ospf_header *hdr,
    struct ospf_hello *hello,
    uint8_t pid,
    ospf_session_t *s,
    uint32_t src_ip)
{
  uint32_t remote_rid  = hdr->router_id; /* net order */
  uint32_t remote_area = hdr->area_id;   /* net order */

  uint16_t hello_int = ntohs(hello->hello_interval);
  uint32_t dead_int  = ntohl(hello->dead_interval);
  uint32_t dr_ip     = hello->designated_router;
  uint32_t bdr_ip    = hello->backup_dr;

  /* Determine which interface received this Hello */
  uint8_t iface_index = 0;
  for (int i = 0; i < s->interface_count; i++) {
    uint32_t network = s->interfaces[i].ip_address & s->interfaces[i].network_mask;
    uint32_t src_network = src_ip & s->interfaces[i].network_mask;
    if (network == src_network) {
      iface_index = i;
      break;
    }
  }

  ospf_interface_t *iface = &s->interfaces[iface_index];

  printf("[OSPF PID%u] <<< RECEIVED: Hello from Router %s (1.1.1.1) on point-to-point interface %u, packet size analysis:\n",
      pid, ip_to_string(remote_rid), iface_index);

  /* Calculate packet size for debugging */
  uint16_t ospf_len = ntohs(hdr->length);
  uint16_t total_packet_size = sizeof(struct ethernet_hdr) +
    sizeof(struct iphdr) +
    ospf_len;
  printf("[OSPF PID%u] OSPF length: %u bytes, Total packet: ~%u bytes\n",
      pid, ospf_len, total_packet_size);

  if (remote_area != s->area_id) {
    char local_area_str[16], remote_area_str[16];
    snprintf(local_area_str, sizeof(local_area_str), "%s", ip_to_string(s->area_id));
    snprintf(remote_area_str, sizeof(remote_area_str), "%s", ip_to_string(remote_area));
    printf("[OSPF PID%u] Area mismatch: local %s, remote %s\n",
        pid, local_area_str, remote_area_str);
    return -1;
  }

  /* FRR uses 1.1.1.1 */
  if (remote_rid != string_to_ip("1.1.1.1")) {
    printf("[OSPF PID%u] ERROR: Expected neighbor RID 1.1.1.1, got %s\n",
        pid, ip_to_string(remote_rid));
    return -1;
  }

  /* Update interface DR/BDR info */
  iface->designated_router = dr_ip;
  iface->backup_dr = bdr_ip;

  iface->last_hello_received = rte_get_tsc_cycles();

  uint16_t total_len = ntohs(hdr->length);
  uint16_t base = sizeof(struct ospf_header) + sizeof(struct ospf_hello);

  bool found_ourselves = false;
  int neighbor_count_in_hello = 0;

  if (total_len > base) {
    neighbor_count_in_hello = (total_len - base) / sizeof(uint32_t);
    uint32_t *nbr_ids = (uint32_t *)(hello + 1);

    printf("[OSPF PID%u] Hello contains %u neighbor(s): ", pid, neighbor_count_in_hello);
    for (int i = 0; i < neighbor_count_in_hello; i++) {
      uint32_t nid = nbr_ids[i]; /* net order */
      printf("%s ", ip_to_string(nid));
      if (nid == s->router_id) {
        found_ourselves = true;
      }
    }
    printf("\n");

    if (found_ourselves) {
      printf("[OSPF PID%u] *** Found ourselves in FRR's Hello (2-WAY) ***\n", pid);
    }
  } else {
    printf("[OSPF PID%u] Hello contains 0 neighbors\n", pid);
  }

  /* Find or create neighbor entry */
  int idx = -1;
  for (int i = 0; i < s->neighbor_count; i++) {
    if (s->neighbors[i].router_id == remote_rid &&
        s->neighbors[i].interface_index == iface_index) {
      idx = i; break;
    }
  }

  if (idx == -1) {
    if (s->neighbor_count >= OSPF_MAX_NEIGHBORS) {
      printf("[OSPF PID%u] Neighbor table full\n", pid);
      return -1;
    }
    idx = s->neighbor_count++;
    ospf_neighbor_t *n = &s->neighbors[idx];
    memset(n, 0, sizeof(*n));
    n->router_id = remote_rid;
    n->ip_address = src_ip;
    memcpy(n->mac_addr, eth_hdr->ether_shost, 6);
    n->interface_index = iface_index;
    n->state = OSPF_STATE_DOWN;
    n->priority = hello->priority;
    n->dead_interval = dead_int;
    n->dd_sequence = (rte_rand() & 0xffffff);
    n->last_hello_received = rte_get_tsc_cycles();
    n->creation_time = rte_get_tsc_cycles();

    /* Initialize exchange state */
    n->exchange_state.retry_count = 0;
    n->exchange_state.step = 0;
    n->exchange_state.waiting_for = 0;
    n->exchange_state.wait_until = 0;

    printf("[OSPF PID%u] New OSPF neighbor discovered: FRR (1.1.1.1) on point-to-point interface %u\n",
        pid, iface_index);

    ospf_update_neighbor_state(s, remote_rid, iface_index, OSPF_STATE_INIT);
  } else {
    ospf_neighbor_t *n = &s->neighbors[idx];
    n->last_hello_received = rte_get_tsc_cycles();
  }

  ospf_neighbor_t *nbr = &s->neighbors[idx];

  if (found_ourselves) {
    /* FRR listed us in its Hello */
    if (nbr->state < OSPF_STATE_TWO_WAY) {
      ospf_update_neighbor_state(s, remote_rid, iface_index, OSPF_STATE_TWO_WAY);

      /* Send Hello response WITH FRR in neighbor list */
      ospf_send_hello_packet(pid, s, iface_index);

      /* For P2P, the state machine can proceed. The main loop will handle the transition to ExStart. */
      printf("[OSPF PID%u] Reached 2-WAY. Main loop will now handle ExStart transition.\n", pid);
    } else if (nbr->state >= OSPF_STATE_EXSTART && nbr->state <= OSPF_STATE_EXCHANGE) {
      /* During DD exchange, FRR might temporarily not include us in Hello */
      /* This is normal - just send Hello to maintain */
      printf("[OSPF PID%u] In DD exchange (state=%s), FRR still includes us, maintaining state\n",
          pid, ospf_state_to_string(nbr->state));
      ospf_send_hello_packet(pid, s, iface_index);
    } else {
      /* Already in higher state, just maintain */
      ospf_send_hello_packet(pid, s, iface_index);
    }
  } else {
    /* FRR didn't list us in Hello */
    if (nbr->state == OSPF_STATE_INIT) {
      printf("[OSPF PID%u] FRR's Hello doesn't contain us yet (staying in INIT)\n", pid);
      ospf_send_hello_packet(pid, s, iface_index);
    } else if (nbr->state >= OSPF_STATE_EXSTART && nbr->state <= OSPF_STATE_EXCHANGE) {
      /* CRITICAL FIX: During DD exchange, FRR might stop listing us temporarily */
      /* This is NORMAL - don't drop state! */
      printf("[OSPF PID%u] Note: FRR not listing us in Hello during DD exchange (state=%s)\n",
          pid, ospf_state_to_string(nbr->state));
      printf("[OSPF PID%u] This is normal OSPF behavior, maintaining state\n", pid);
      ospf_send_hello_packet(pid, s, iface_index);
    } else if (nbr->state >= OSPF_STATE_LOADING) {
      /* In LOADING or FULL, FRR should list us */
      printf("[OSPF PID%u] WARNING: FRR dropped us from Hello in state %s\n",
          pid, ospf_state_to_string(nbr->state));
      printf("[OSPF PID%u] Moving back to INIT\n", pid);
      ospf_update_neighbor_state(s, remote_rid, iface_index, OSPF_STATE_INIT);
      ospf_send_hello_packet(pid, s, iface_index);
    }
  }

  return 0;
}

/* ---------- RX: DD ---------- */

int ospf_handle_dd_packet(struct ethernet_hdr *eth_hdr,
    struct ospf_header *hdr,
    struct ospf_dd *dd,
    uint8_t pid,
    ospf_session_t *s,
    uint32_t src_ip)
{
  uint32_t remote_rid = hdr->router_id;  /* Should be 1.1.1.1 */
  uint32_t dd_seq = ntohl(dd->dd_sequence);
  uint8_t flags = dd->flags;

  /* Determine interface */
  uint8_t iface_index = 0;
  for (int i = 0; i < s->interface_count; i++) {
    uint32_t network = s->interfaces[i].ip_address & s->interfaces[i].network_mask;
    uint32_t src_network = src_ip & s->interfaces[i].network_mask;
    if (network == src_network) {
      iface_index = i;
      break;
    }
  }

  printf("[OSPF PID%u] <<< RECEIVED: DD from FRR (1.1.1.1) seq %u flags 0x%02x on interface %u\n",
      pid, dd_seq, flags, iface_index);
  printf("[OSPF PID%u] DD Flags: I=%u, M=%u, MS=%u\n",
      pid,
      (flags & OSPF_DD_FLAG_I) ? 1 : 0,
      (flags & OSPF_DD_FLAG_M) ? 1 : 0,
      (flags & OSPF_DD_FLAG_MS) ? 1 : 0);

  /* Check if DD contains LSA headers */
  bool has_lsa_headers = dd_contains_lsa_headers(hdr, dd);
  printf("[OSPF PID%u] DD contains LSA headers: %s\n", pid, has_lsa_headers ? "YES" : "NO");

  ospf_neighbor_t *nbr = NULL;
  for (int i = 0; i < s->neighbor_count; i++) {
    if (s->neighbors[i].router_id == remote_rid &&
        s->neighbors[i].interface_index == iface_index) {
      nbr = &s->neighbors[i];
      break;
    }
  }
  if (!nbr) {
    printf("[OSPF PID%u] DD from unknown FRR neighbor\n", pid);
    return -1;
  }

  /* Per RFC 2328, DD packets are ignored in states < ExStart */
  if (nbr->state < OSPF_STATE_EXSTART) {
    char rid_str[16];
    snprintf(rid_str, sizeof(rid_str), "%s", ip_to_string(remote_rid));
    printf("[OSPF PID%u] Received DD from %s in state %s, ignoring packet.\n",
        pid, rid_str, ospf_state_to_string(nbr->state));
    return 0;
  }

  nbr->last_dd_received = rte_get_tsc_cycles();

  /* RFC 2328 Section 10.8: Receiving Database Description Packets */
  switch (nbr->state) {
    case OSPF_STATE_DOWN:
    case OSPF_STATE_ATTEMPT:
    case OSPF_STATE_TWO_WAY:
        /* DD packets are not expected in these states. Ignore. */
        printf("[OSPF PID%u] Received DD packet from %s in unexpected state %s. Ignoring.\n",
               pid, ip_to_string(remote_rid), ospf_state_to_string(nbr->state));
        break;

    case OSPF_STATE_INIT:
        /* This is an error, should have transitioned to 2-Way first. Resetting. */
        printf("[OSPF PID%u] Received DD packet from %s in INIT state. This is an error. Resetting neighbor.\n",
               pid, ip_to_string(remote_rid));
        ospf_update_neighbor_state(s, remote_rid, iface_index, OSPF_STATE_DOWN);
        break;

    case OSPF_STATE_EXSTART:
        if ((flags & OSPF_DD_FLAG_I) && (flags & OSPF_DD_FLAG_M) && (flags & OSPF_DD_FLAG_MS) &&
            !has_lsa_headers && (ntohl(remote_rid) > ntohl(s->router_id))) {
            /* Peer is master, we must be slave. */
            printf("[OSPF PID%u] [EXSTART] Peer %s is MASTER. We are SLAVE.\n", pid, ip_to_string(remote_rid));
            nbr->is_master = 0;
            nbr->dd_sequence = dd_seq;
            ospf_update_neighbor_state(s, remote_rid, iface_index, OSPF_STATE_EXCHANGE);
            /* As slave, our first packet echoes the master's seq num, with our own DD summary */
            ospf_send_dd_packet(pid, s, remote_rid, OSPF_DD_FLAG_M, nbr->dd_sequence, true, iface_index);

        } else if (!(flags & OSPF_DD_FLAG_I) && !(flags & OSPF_DD_FLAG_MS) &&
                   (dd_seq == nbr->dd_sequence) && (ntohl(s->router_id) > ntohl(remote_rid))) {
            /* We are master, and slave has acknowledged our mastership. */
            printf("[OSPF PID%u] [EXSTART] Peer %s is SLAVE. Moving to EXCHANGE.\n", pid, ip_to_string(remote_rid));
            ospf_update_neighbor_state(s, remote_rid, iface_index, OSPF_STATE_EXCHANGE);
            /* Start sending our DD summary */
            nbr->dd_sequence++;
            ospf_send_dd_packet(pid, s, remote_rid, OSPF_DD_FLAG_M | OSPF_DD_FLAG_MS, nbr->dd_sequence, true, iface_index);

        } else if ((flags & OSPF_DD_FLAG_I) && (ntohl(s->router_id) > ntohl(remote_rid))) {
             /* Mastership conflict. Our RID is higher, so we re-send our initial DD packet to assert master. */
             printf("[OSPF PID%u] [EXSTART] Mastership conflict with %s. Re-asserting master role.\n",
                    pid, ip_to_string(remote_rid));
             ospf_send_dd_packet(pid, s, nbr->router_id, OSPF_DD_FLAG_I | OSPF_DD_FLAG_M | OSPF_DD_FLAG_MS, nbr->dd_sequence, false, nbr->interface_index);
        }
        break;

    case OSPF_STATE_EXCHANGE:
        /* Packet Validation: I-bit must be 0, MS-bit must be opposite of ours, and sequence numbers must match expectations. */
        if ((flags & OSPF_DD_FLAG_I) ||
            ((flags & OSPF_DD_FLAG_MS) == nbr->is_master) ||
            (nbr->is_master && (dd_seq != nbr->dd_sequence)) ||
            (!nbr->is_master && (dd_seq != nbr->dd_sequence + 1))) {
            printf("[OSPF PID%u] [EXCHANGE] DD packet from %s failed validation. Resetting.\n", pid, ip_to_string(remote_rid));
            ospf_update_neighbor_state(s, remote_rid, iface_index, OSPF_STATE_EXSTART);
            return -1;
        }

        if (has_lsa_headers) {
            struct ospf_lsa_header *lsa = (struct ospf_lsa_header *)(dd + 1);
            uint16_t ospf_len = ntohs(hdr->length);
            uint16_t remaining_len = ospf_len - sizeof(struct ospf_header) - sizeof(struct ospf_dd);

            while (remaining_len >= sizeof(struct ospf_lsa_header)) {
                if (nbr->ls_request_count < OSPF_MAX_LSAS_PER_UPDATE) {
                    memcpy(&nbr->ls_request_list[nbr->ls_request_count], lsa, sizeof(struct ospf_lsa_header));
                    nbr->ls_request_count++;
                }
                uint16_t lsa_len = ntohs(lsa->length);
                remaining_len -= lsa_len;
                lsa = (struct ospf_lsa_header *)((uint8_t *)lsa + lsa_len);
            }
        }

        if (nbr->is_master) {
             if (dd_seq == nbr->dd_sequence) {
                /* Slave acknowledged our last DD packet. We can send the next one. */
                bool we_have_more = false; /* Simplified: We assume all LSAs fit in one packet */

                if (we_have_more) {
                    nbr->dd_sequence++;
                    // ospf_send_dd_packet(pid, s, ...);
                } else if (flags & OSPF_DD_FLAG_M) {
                    /* Slave has more LSAs. We need to poll for them. */
                    nbr->dd_sequence++;
                    ospf_send_dd_packet(pid, s, remote_rid, OSPF_DD_FLAG_MS, nbr->dd_sequence, false, iface_index);
                } else {
                    /* Neither has more. Exchange is done. */
                    printf("[OSPF PID%u] [EXCHANGE] Master: Exchange complete with %s.\n", pid, ip_to_string(remote_rid));
                    ospf_update_neighbor_state(s, remote_rid, iface_index, OSPF_STATE_LOADING);
                }
             }
        } else { // We are slave
            if (dd_seq == nbr->dd_sequence + 1) {
                nbr->dd_sequence = dd_seq;
                /* Master has sent next DD packet. */
                bool we_have_more = false; /* Simplified */

                if (!(flags & OSPF_DD_FLAG_M) && !we_have_more) {
                    printf("[OSPF PID%u] [EXCHANGE] Slave: Exchange complete with %s.\n", pid, ip_to_string(remote_rid));
                    ospf_update_neighbor_state(s, remote_rid, iface_index, OSPF_STATE_LOADING);
                }
                /* Acknowledge and send our next DD packet (which is empty in our simple case) */
                ospf_send_dd_packet(pid, s, remote_rid, we_have_more ? OSPF_DD_FLAG_M : 0, nbr->dd_sequence, false, iface_index);
            }
        }
        break;

    case OSPF_STATE_LOADING:
    case OSPF_STATE_FULL:
        /* In these states, we should only process duplicate DD packets. */
        if (dd_seq == nbr->dd_sequence && !nbr->is_master) {
             /* Slave re-acknowledging master's last packet */
             ospf_send_dd_packet(pid, s, remote_rid, 0, nbr->dd_sequence, false, iface_index);
        }
        break;
  }

  return 0;
}

/* ---------- RX: LSR / LSU / LSAck ---------- */

int ospf_handle_lsr_packet(struct ethernet_hdr *eth_hdr,
    struct ospf_header *hdr,
    struct ospf_lsr *lsr,
    uint8_t pid,
    ospf_session_t *s,
    uint32_t src_ip)
{
  uint32_t remote_rid = hdr->router_id;  /* Should be 1.1.1.1 */
  uint32_t ls_type = ntohl(lsr->ls_type);

  /* Determine interface */
  uint8_t iface_index = 0;
  for (int i = 0; i < s->interface_count; i++) {
    uint32_t network = s->interfaces[i].ip_address & s->interfaces[i].network_mask;
    uint32_t src_network = src_ip & s->interfaces[i].network_mask;
    if (network == src_network) {
      iface_index = i;
      break;
    }
  }

  printf("[OSPF PID%u] <<< RECEIVED: LSR from FRR (1.1.1.1) type %u on interface %u\n",
      pid, ls_type, iface_index);

  /* Send LSU response immediately */
  printf("[OSPF PID%u] Sending LSU response immediately\n", pid);
  uint8_t lsa_buf[OSPF_MAX_LSA_SIZE];
  struct ospf_lsa_header *lsa = (struct ospf_lsa_header *)lsa_buf;
  ospf_generate_router_lsa(s, lsa, iface_index);
  struct ospf_lsa_header *lsa_ptr = lsa;
  ospf_send_lsu_packet(pid, s, remote_rid, LSA_TYPE_ROUTER, 1, &lsa_ptr, iface_index);

  return 0;
}

int ospf_handle_lsu_packet(struct ethernet_hdr *eth_hdr,
    struct ospf_header *hdr,
    struct ospf_lsu *lsu,
    uint8_t pid,
    ospf_session_t *s,
    uint32_t src_ip)
{
  uint32_t remote_rid = hdr->router_id;  /* Should be 1.1.1.1 */
  uint32_t num = ntohl(lsu->num_lsas);

  /* Determine interface */
  uint8_t iface_index = 0;
  for (int i = 0; i < s->interface_count; i++) {
    uint32_t network = s->interfaces[i].ip_address & s->interfaces[i].network_mask;
    uint32_t src_network = src_ip & s->interfaces[i].network_mask;
    if (network == src_network) {
      iface_index = i;
      break;
    }
  }

  printf("[OSPF PID%u] <<< RECEIVED: LSU from FRR (1.1.1.1) with %u LSA(s) on interface %u\n",
      pid, num, iface_index);

  struct ospf_lsa_header *lsa = (struct ospf_lsa_header *)(lsu + 1);
  struct ospf_lsa_header *lsas_to_ack[OSPF_MAX_LSAS_PER_UPDATE];
  uint8_t lsa_count = 0;

  for (uint32_t i = 0; i < num && i < OSPF_MAX_LSAS_PER_UPDATE; i++) {
    uint64_t key = ((uint64_t)lsa->type << 32) | lsa->link_state_id;
    lsas_to_ack[lsa_count++] = lsa;

    /* Add to LSDB */
    void *existing_data;
    if (rte_hash_lookup_data(s->lsdb, &key, &existing_data) < 0) {
      rte_hash_add_key_data(s->lsdb, &key, lsa);
      char lsid_str[16], adv_str[16];
      snprintf(lsid_str, sizeof(lsid_str), "%s", ip_to_string(lsa->link_state_id));
      snprintf(adv_str, sizeof(adv_str), "%s", ip_to_string(lsa->advertising_router));
      printf("[OSPF PID%u] LSDB add: type %u, LSID %s, Adv %s\n",
          pid, lsa->type, lsid_str, adv_str);
    }

    /* Move to next LSA */
    lsa = (struct ospf_lsa_header *)((uint8_t *)lsa + ntohs(lsa->length));
  }

  /* Send LSAck immediately */
  printf("[OSPF PID%u] Sending LSAck immediately\n", pid);
  ospf_send_lsack_packet(pid, s, remote_rid, lsas_to_ack, lsa_count, iface_index);

  /* Move to FULL state */
  for (int i = 0; i < s->neighbor_count; i++) {
    if (s->neighbors[i].router_id == remote_rid &&
        s->neighbors[i].interface_index == iface_index &&
        s->neighbors[i].state == OSPF_STATE_LOADING) {
      printf("[OSPF PID%u] Moving to FULL state with FRR\n", pid);
      ospf_update_neighbor_state(s, remote_rid, iface_index, OSPF_STATE_FULL);
      break;
    }
  }

  return 0;
}

int ospf_handle_lsack_packet(struct ethernet_hdr *eth_hdr,
    struct ospf_header *hdr,
    uint8_t pid,
    ospf_session_t *s,
    uint32_t src_ip)
{
    uint32_t remote_rid = hdr->router_id;

    /* Determine interface */
    uint8_t iface_index = 0;
    for (int i = 0; i < s->interface_count; i++) {
        uint32_t network = s->interfaces[i].ip_address & s->interfaces[i].network_mask;
        uint32_t src_network = src_ip & s->interfaces[i].network_mask;
        if (network == src_network) {
            iface_index = i;
            break;
        }
    }

    printf("[OSPF PID%u] <<< RECEIVED: LSAck from FRR (%s) on interface %u\n",
           pid, ip_to_string(remote_rid), iface_index);

    struct ospf_lsa_header *lsa_hdr = (struct ospf_lsa_header *)(hdr + 1);
    uint16_t ospf_len = ntohs(hdr->length);
    uint16_t headers_len = ospf_len - sizeof(struct ospf_header);

    if (headers_len % sizeof(struct ospf_lsa_header) != 0) {
        printf("[OSPF PID%u] LSAck packet has invalid length\n", pid);
        return -1;
    }
    int num_lsas = headers_len / sizeof(struct ospf_lsa_header);

    printf("[OSPF PID%u] LSAck contains %d LSA header(s):\n", pid, num_lsas);

    for (int i = 0; i < num_lsas; i++) {
        char lsid_str[16], adv_str[16];
        snprintf(lsid_str, sizeof(lsid_str), "%s", ip_to_string(lsa_hdr[i].link_state_id));
        snprintf(adv_str, sizeof(adv_str), "%s", ip_to_string(lsa_hdr[i].advertising_router));

        printf("  - Type: %u, LSID: %s, AdvRtr: %s, Seq: 0x%x\n",
               lsa_hdr[i].type, lsid_str, adv_str,
               ntohl(lsa_hdr[i].sequence_number));
    }

    /* In a full implementation, we would use this info to stop retransmitting these LSAs. */
    /* For this simulator, logging is sufficient to confirm the exchange is happening. */

    return 0;
}

/* ---------- Neighbor timeouts / retransmit ---------- */

#define OSPF_DD_RETRANSMIT_INTERVAL 5 /* seconds */

void ospf_process_neighbor_timeouts(ospf_session_t *s, uint8_t pid)
{
  uint64_t now = rte_get_tsc_cycles();
  uint64_t dead_cycles = (uint64_t)s->config.dead_interval * rte_get_tsc_hz();
  uint64_t session_timeout_cycles = (uint64_t)OSPF_SESSION_TIMEOUT * rte_get_tsc_hz();

  for (int i = 0; i < s->neighbor_count; i++) {
    ospf_neighbor_t *n = &s->neighbors[i];

    /* Dead timer check */
    if (n->state != OSPF_STATE_DOWN && now - n->last_hello_received > dead_cycles) {
      char rid_str[16];
      snprintf(rid_str, sizeof(rid_str), "%s", ip_to_string(n->router_id));
      printf("[OSPF PID%u] Neighbor %s dead timer expired on interface %u\n",
          pid, rid_str, n->interface_index);
      ospf_update_neighbor_state(s, n->router_id, n->interface_index, OSPF_STATE_DOWN);

      s->dirty_lsa = 1;

      for (int j = i; j < s->neighbor_count - 1; j++)
        s->neighbors[j] = s->neighbors[j+1];
      s->neighbor_count--;
      i--;
      continue;
    }

    if (n->state < OSPF_STATE_FULL && now - n->creation_time > session_timeout_cycles) {
        char rid_str[16];
        snprintf(rid_str, sizeof(rid_str), "%s", ip_to_string(n->router_id));
        printf("[OSPF PID%u] Neighbor %s failed to reach FULL state within timeout, resetting session\n",
            pid, rid_str);

        for (int j = i; j < s->neighbor_count - 1; j++)
            s->neighbors[j] = s->neighbors[j+1];
        s->neighbor_count--;
        i--;
        continue;
    }

    /* State-specific timeout handling */
    switch (n->state) {
      case OSPF_STATE_EXSTART:
        {
          uint64_t retransmit_cycles = (uint64_t)OSPF_DD_RETRANSMIT_INTERVAL * rte_get_tsc_hz();
          if (n->is_master && now - n->last_dd_sent > retransmit_cycles) {
            printf("[OSPF PID%u] EXSTART timeout with FRR on interface %u, retransmitting DD\n",
                pid, n->interface_index);
            ospf_send_dd_packet(pid, s, n->router_id, OSPF_DD_FLAG_I | OSPF_DD_FLAG_M | OSPF_DD_FLAG_MS, n->dd_sequence, false, n->interface_index);
            n->last_dd_sent = now;
          }
        }
        break;

      case OSPF_STATE_EXCHANGE:
        /* EXCHANGE timeout - check if waiting for FRR's DD with LSA headers */
        if (n->exchange_state.waiting_for == OSPF_TYPE_DD &&
            now > n->exchange_state.wait_until) {
          printf("[OSPF PID%u] EXCHANGE timeout waiting for FRR's DD with LSA headers\n", pid);

          n->exchange_state.retry_count++;
          if (n->exchange_state.retry_count >= 3) {
            printf("[OSPF PID%u] Too many retries, restarting adjacency\n", pid);
            ospf_update_neighbor_state(s, n->router_id, n->interface_index, OSPF_STATE_EXSTART);
            n->exchange_state.retry_count = 0;
            n->exchange_state.waiting_for = 0;
            n->exchange_state.step = 0;
          } else {
            /* Resend our DD with LSA headers */
            printf("[OSPF PID%u] Retry %u: Resending DD with LSA headers\n",
                pid, n->exchange_state.retry_count);
            ospf_send_dd_packet(pid, s, n->router_id,
                OSPF_DD_FLAG_M, n->dd_sequence, true, n->interface_index);
            n->exchange_state.wait_until = now + (10 * rte_get_tsc_hz());
          }
        }
        break;

      case OSPF_STATE_LOADING:
        /* LOADING timeout - check for LSR/LSU exchange */
        if (n->exchange_state.waiting_for == OSPF_TYPE_LSU &&
            now > n->exchange_state.wait_until) {
          printf("[OSPF PID%u] LOADING timeout waiting for LSU\n", pid);

          /* Resend LSR */
          printf("[OSPF PID%u] Resending LSR\n", pid);
          ospf_send_lsr_packet(pid, s, n->router_id,
              LSA_TYPE_ROUTER, n->router_id, n->router_id, n->interface_index);
          n->exchange_state.wait_until = now + (5 * rte_get_tsc_hz());
        }
        break;

      default:
        /* Other states don't need special timeout handling */
        break;
    }
  }
}

/* ---------- Main test loop ---------- */

int ospf_test_main_loop(uint8_t pid, int userId, uint8_t pairPid)
{
  (void)userId;
  (void)pairPid;

  uint8_t lid = rte_lcore_id();
  uint16_t nb_rx;
  struct rte_mbuf *pkts[32];

  uint32_t router_id;
  uint32_t area_id = string_to_ip("0.0.0.0");
  uint64_t hello_cycles;
  uint64_t last_periodic_hello = 0;
  uint64_t last_stats_display = 0;

  printf("\n=== Starting OSPF Test on PID %u, Core %u ===\n", pid, lid);

  /* Set Router IDs based on your topology */
  switch (pid) {
    case 0:
      router_id = string_to_ip("2.2.2.2");  /* Your router RID for PID 0 */
      break;
    case 1:
      router_id = string_to_ip("2.2.2.2");  /* Your router RID for PID 1 */
      break;
    default:
      router_id = string_to_ip("192.168.99.99");
      break;
  }

  if (ospf_initialize_test(pid, router_id, area_id) != 0) {
    printf("Failed to initialize OSPF test on PID %u\n", pid);
    return -1;
  }

  ospf_session_t *s = &ospf_sessions[pid];

  uint8_t mac[6];
  switch (pid) {
    case 0: memcpy(mac, (uint8_t[]){0x00,0x11,0x22,0x33,0x44,0x01}, 6); break;
    case 1: memcpy(mac, (uint8_t[]){0x00,0x11,0x22,0x33,0x44,0x02}, 6); break;
    default: memcpy(mac, (uint8_t[]){0x00,0x11,0x22,0x33,0x44,0x99}, 6); break;
  }

  printf("OSPF Configuration (PID %u):\n", pid);
  printf("  Router ID: %s\n", ip_to_string(s->router_id));
  printf("  Area: %s\n", ip_to_string(s->area_id));
  printf("  Interface: %s/%s (Point-to-Point)\n",
      ip_to_string(s->interfaces[0].ip_address),
      ip_to_string(s->config.network_mask));
  printf("  Interface Type: %s\n", s->interfaces[0].type == OSPF_IFTYPE_P2P ? "POINT-TO-POINT" : "BROADCAST");
  printf("  MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
      mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

  hello_cycles = (uint64_t)s->config.hello_interval * rte_get_tsc_hz();

  /* CRITICAL FIX: Send initial Hello immediately upon startup */
  printf("[OSPF PID%u] Sending INITIAL Hello to discover FRR\n", pid);
  ospf_send_hello_packet(pid, s, 0);
  last_periodic_hello = rte_get_tsc_cycles();

  /* Start step-by-step exchange */
  ospf_start_exchange(s, 0);

  while (pblast[pid].trafficStatus) {
    uint64_t now = rte_get_tsc_cycles();

    /* Process step-by-step exchange */
    ospf_process_exchange_steps(s, pid);

    /* Send periodic Hellos every hello_interval */
    if (now - last_periodic_hello > hello_cycles) {
      ospf_send_hello_packet(pid, s, 0);
      last_periodic_hello = now;
    }

    /* Process neighbor timeouts */
    ospf_process_neighbor_timeouts(s, pid);

    /* Check for neighbors in 2-Way state to start the exchange */
    for (int i = 0; i < s->neighbor_count; i++) {
        ospf_neighbor_t *n = &s->neighbors[i];
        if (n->state == OSPF_STATE_TWO_WAY) {
            printf("[OSPF PID%u] Neighbor %s is 2-WAY, proceeding to EXSTART.\n", pid, ip_to_string(n->router_id));
            ospf_update_neighbor_state(s, n->router_id, n->interface_index, OSPF_STATE_EXSTART);

            if (ntohl(s->router_id) > ntohl(n->router_id)) {
                n->is_master = 1;
                char local_rid_str[16], remote_rid_str[16];
                snprintf(local_rid_str, sizeof(local_rid_str), "%s", ip_to_string(s->router_id));
                snprintf(remote_rid_str, sizeof(remote_rid_str), "%s", ip_to_string(n->router_id));
                printf("[OSPF PID%u] We are MASTER for %s (RID %s > %s)\n",
                       pid, remote_rid_str, local_rid_str, remote_rid_str);
            } else {
                n->is_master = 0;
                char local_rid_str[16], remote_rid_str[16];
                snprintf(local_rid_str, sizeof(local_rid_str), "%s", ip_to_string(s->router_id));
                snprintf(remote_rid_str, sizeof(remote_rid_str), "%s", ip_to_string(n->router_id));
                printf("[OSPF PID%u] We are SLAVE for %s (RID %s < %s)\n",
                    pid, remote_rid_str, local_rid_str, remote_rid_str);
            }
        }
    }

    /* Check for neighbors needing initial DD packet */
    for (int i = 0; i < s->neighbor_count; i++) {
        ospf_neighbor_t *n = &s->neighbors[i];
        if (n->state == OSPF_STATE_EXSTART && !n->initial_dd_sent && n->is_master) {
            printf("[OSPF PID%u] Sending initial DD as master from main loop\n", pid);
            ospf_send_dd_packet(pid, s, n->router_id, OSPF_DD_FLAG_I | OSPF_DD_FLAG_M | OSPF_DD_FLAG_MS, n->dd_sequence, false, n->interface_index);
            n->last_dd_sent = rte_get_tsc_cycles();
            n->initial_dd_sent = 1;
        }
    }

    /* Check for neighbors in LOADING state to send LSRs */
    for (int i = 0; i < s->neighbor_count; i++) {
        ospf_neighbor_t *n = &s->neighbors[i];
        if (n->state == OSPF_STATE_LOADING && n->ls_request_count > 0) {
            printf("[OSPF PID%u] Neighbor %s is LOADING, sending LSRs.\n", pid, ip_to_string(n->router_id));
            for (int j = 0; j < n->ls_request_count; j++) {
                struct ospf_lsa_header *lsa_h = &n->ls_request_list[j];
                ospf_send_lsr_packet(pid, s, n->router_id, lsa_h->type, lsa_h->link_state_id, lsa_h->advertising_router, n->interface_index);
            }
            n->ls_request_count = 0; /* Clear the request list */
        }
    }

    if (now - last_stats_display > 10 * rte_get_tsc_hz()) {
        ospf_display_stats(pid);
        last_stats_display = now;
    }

    if (s->sim_routes_count > 0 && !s->sim_routes_advertised) {
        for (uint32_t i = 0; i < s->sim_routes_count; i++) {
            uint8_t lsa_buf[OSPF_MAX_LSA_SIZE] = {0};
            struct ospf_lsa_header *lsa = (struct ospf_lsa_header *)lsa_buf;
            uint32_t sim_route = s->sim_routes_start + i;

            if (ospf_generate_external_lsa(s, lsa, sim_route) == 0) {
                for (int j = 0; j < s->neighbor_count; j++) {
                    ospf_neighbor_t *n = &s->neighbors[j];

                    if (n->state >= OSPF_STATE_EXCHANGE) {
                        struct ospf_lsa_header *lsa_list[] = {lsa};
                        ospf_send_lsu_packet(pid, s, n->router_id, lsa->type, 1, lsa_list, n->interface_index);
                    }
                }
            }
        }
        s->sim_routes_advertised = 1;
    }

    if(s->dirty_lsa) {
        uint8_t lsa_buf[OSPF_MAX_LSA_SIZE] = {0};
        struct ospf_lsa_header *lsa = (struct ospf_lsa_header *)lsa_buf;

        if (ospf_generate_router_lsa(s, lsa, 0) == 0) {
            for (int i = 0; i < s->neighbor_count; i++) {
                ospf_neighbor_t *n = &s->neighbors[i];

                if (n->state >= OSPF_STATE_EXCHANGE) {
                    struct ospf_lsa_header *lsa_list[] = {lsa};
                    ospf_send_lsu_packet(pid, s, n->router_id, lsa->type, 1, lsa_list, n->interface_index);
                }
            }
        }
        s->dirty_lsa = 0;
    }

    /* Receive and process packets */
    nb_rx = rte_eth_rx_burst(pid, 0, pkts, 32);
    for (uint16_t i = 0; i < nb_rx; i++) {
      struct ethernet_hdr *eth = rte_pktmbuf_mtod(pkts[i], struct ethernet_hdr *);
      struct iphdr *ip = (struct iphdr *)(eth + 1);

      /* Only process OSPF packets */
      if (eth->ether_type == htons(ETHERTYPE_IP) &&
          ip->protocol == OSPF_PROTOCOL_NUMBER) {
        ospf_process_packet(pkts[i], pid, s);
      }
      rte_pktmbuf_free(pkts[i]);
    }

    /* Run SPF when we have FULL neighbors */
    if (s->config.neighbors_full > 0) {
      static uint64_t last_spf_run = 0;
      if (now - last_spf_run > (5 * rte_get_tsc_hz())) {
        printf("[OSPF PID%u] Running SPF calculation\n", pid);
        last_spf_run = now;
      }
    }

    /* CRITICAL: Reduce delay to respond faster to FRR */
    rte_delay_us(1000); /* 1ms delay */
  }

  printf("\n=== OSPF Test Stopped on PID %u ===\n", pid);
  return 0;
}

/* ---------- RX: top-level dispatcher ---------- */

int ospf_process_packet(struct rte_mbuf *pkt, uint8_t pid, ospf_session_t *s)
{
  struct ethernet_hdr *eth = rte_pktmbuf_mtod(pkt, struct ethernet_hdr *);

  if (pblast[pid].trafficCapture) {
    pblast_pcapdump(pid, (const u_char *)eth, pkt->data_len);
  }

  if (eth->ether_type != htons(ETHERTYPE_IP))
    return -1;

  struct iphdr *ip = (struct iphdr *)(eth + 1);
  if (ip->protocol != OSPF_PROTOCOL_NUMBER)
    return -1;

  struct ospf_header *hdr = (struct ospf_header *)(ip + 1);

  if (hdr->version != OSPF_VERSION) {
    printf("[OSPF PID%u] Invalid OSPF version %u\n", pid, hdr->version);
    return -1;
  }

  uint16_t ospf_len = ntohs(hdr->length);
  uint16_t rec = ntohs(hdr->checksum);
  uint16_t calc = ospf_checksum(hdr, ospf_len);

  if (rec != calc) {
    printf("[OSPF PID%u] OSPF checksum error: received %04x, calculated %04x\n",
        pid, rec, calc);
    return -1;
  }

  switch (hdr->type) {
    case OSPF_TYPE_HELLO:
      s->config.hello_received++;
      return ospf_handle_hello_packet(eth,
          hdr, (struct ospf_hello *)(hdr + 1), pid, s, ip->saddr);
    case OSPF_TYPE_DD:
      s->config.dd_received++;
      return ospf_handle_dd_packet(eth,
          hdr, (struct ospf_dd *)(hdr + 1), pid, s, ip->saddr);
    case OSPF_TYPE_LSR:
      s->config.lsr_received++;
      return ospf_handle_lsr_packet(eth,
          hdr, (struct ospf_lsr *)(hdr + 1), pid, s, ip->saddr);
    case OSPF_TYPE_LSU:
      s->config.lsu_received++;
      return ospf_handle_lsu_packet(eth,
          hdr, (struct ospf_lsu *)(hdr + 1), pid, s, ip->saddr);
    case OSPF_TYPE_LSACK:
      s->config.lsack_received++;
      return ospf_handle_lsack_packet(eth, hdr, pid, s, ip->saddr);
    default:
      printf("[OSPF PID%u] Unknown OSPF type %u\n",
          pid, hdr->type);
      return -1;
  }
}

int ospf_add_simulated_routes(ospf_session_t *s, uint32_t count, uint32_t start_ip) {
    if (!s) return -1;

    s->sim_routes_count = count;
    s->sim_routes_start = start_ip;

    printf("[OSPF PID%u] Simulating %u routes starting from %s\n",
           s->pid, count, ip_to_string(start_ip));

    return 0;
}

int ospf_generate_external_lsa(ospf_session_t *s, struct ospf_lsa_header *lsa, uint32_t link_state_id) {
    if (s->lsa_seq_num == 0) s->lsa_seq_num = 0x80000001;

    uint16_t lsa_length = sizeof(struct ospf_lsa_header) + sizeof(struct ospf_external_lsa);

    struct ospf_lsa_header *hdr = (struct ospf_lsa_header *)lsa;
    hdr->age = htons(0);
    hdr->options = OSPF_OPTION_E;
    hdr->type = LSA_TYPE_EXTERNAL;
    hdr->link_state_id = link_state_id;
    hdr->advertising_router = s->router_id;
    hdr->sequence_number = htonl(s->lsa_seq_num++);
    hdr->length = htons(lsa_length);

    struct ospf_external_lsa *ext_lsa = (struct ospf_external_lsa *)(hdr + 1);
    ext_lsa->network_mask = htonl(0xFFFFFFFF);
    ext_lsa->metric = htonl(10);
    ext_lsa->forwarding_address = 0;
    ext_lsa->external_route_tag = 0;

    hdr->checksum = ospf_lsa_checksum(hdr);

    return 0;
}
void ospf_run_spf(ospf_session_t *s) {
    if (!s) return;

    s->spf_run_count++;

    printf("[OSPF PID%u] SPF calculation #%lu\n", s->pid, s->spf_run_count);
}
void ospf_display_stats(uint8_t pid) {
    if (pid >= RTE_MAX_ETHPORTS) return;

    ospf_session_t *s = &ospf_sessions[pid];
    printf("\n--- OSPF Stats for PID %u ---\n", pid);
    char rid_str[16];
    snprintf(rid_str, sizeof(rid_str), "%s", ip_to_string(s->router_id));
    printf("Router ID: %s\n", rid_str);
    printf("Neighbors:\n");
    for (int i = 0; i < s->neighbor_count; i++) {
        ospf_neighbor_t *n = &s->neighbors[i];
        char n_rid_str[16];
        snprintf(n_rid_str, sizeof(n_rid_str), "%s", ip_to_string(n->router_id));
        printf("  - %s: %s\n", n_rid_str, ospf_state_to_string(n->state));
    }
    printf("Packet Stats (TX/RX):\n");
    printf("  Hello: %lu/%lu\n", s->config.hello_sent, s->config.hello_received);
    printf("  DD:    %lu/%lu\n", s->config.dd_sent, s->config.dd_received);
    printf("  LSR:   %lu/%lu\n", s->config.lsr_sent, s->config.lsr_received);
    printf("  LSU:   %lu/%lu\n", s->config.lsu_sent, s->config.lsu_received);
    printf("  LSAck: %lu/%lu\n", s->config.lsack_sent, s->config.lsack_received);
    printf("DB Counts:\n");
    printf("  LSDB: %d\n", rte_hash_count(s->lsdb));
    printf("  RIB:  %d\n", rte_hash_count(s->rib));
    printf("  FIB:  %d\n", rte_hash_count(s->fib));
    printf("-------------------------\n");
}