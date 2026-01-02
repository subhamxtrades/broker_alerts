#ifndef ATICARA_H
#define ATICARA_H

#include <rte_mempool.h>
#include <rte_mbuf.h>

// Dummy structure for queue info
typedef struct {
    struct rte_mempool *tx_mp;
} queue_info_t;

// Dummy structure for port info
typedef struct {
    queue_info_t q[1]; // Assuming at least one queue
} port_info_t;

// Dummy structure for pblast info
typedef struct {
    int trafficCapture;
    int trafficStatus;
} pblast_info_t;

// Dummy global structures
struct {
    port_info_t info[32]; // Assuming max ports
    void *l2p;
} aticara;

pblast_info_t pblast[32];

// Dummy function prototypes
static inline int wr_get_txque(void *l2p, uint8_t lid, uint8_t pid) {
    (void)l2p; (void)lid; (void)pid;
    return 0;
}

static inline void send_mbuf(struct rte_mbuf *m, uint8_t pid, int qid) {
    (void)pid; (void)qid;
    rte_pktmbuf_free(m);
}

static inline void pblast_pcapdump(uint8_t pid, const unsigned char *pkt, unsigned int len) {
    (void)pid; (void)pkt; (void)len;
}

// Dummy macro for logging
#define PRINT_LOG(fmt, ...) printf(fmt, ##__VA_ARGS__)

#endif // ATICARA_H