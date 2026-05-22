/* AgentChain Engine — peer-to-peer transport.
 *
 * Length-prefixed TCP gossip with explicit peer list. No DHT, no UDP, no NAT
 * traversal — by design, per PROTOCOL § 12. The networking module surfaces
 * a callback API; the node module wires it to mempool/consensus/chain.
 */

#ifndef AGENTCHAIN_NET_H
#define AGENTCHAIN_NET_H

#include "common.h"
#include "crypto.h"

/* Network message types (PROTOCOL § 12). */
#define AC_MSG_HELLO        0x01
#define AC_MSG_HEADERS_REQ  0x02
#define AC_MSG_HEADERS_RES  0x03
#define AC_MSG_BLOCK_REQ    0x04
#define AC_MSG_BLOCK_RES    0x05
#define AC_MSG_BLOCK_ANN    0x06
#define AC_MSG_TX_ANN       0x07
#define AC_MSG_COMMIT_VOTE  0x08
#define AC_MSG_PING         0x0B
#define AC_MSG_PONG         0x0C

#define AC_FRAME_VERSION    1
#define AC_FRAME_MAX_BYTES  (4 * 1024 * 1024)   /* 4 MB ceiling */

typedef struct ac_net_s ac_net_t;

/* Callbacks invoked from the networking threads. Implementations must be
 * thread-safe with respect to chain/mempool/consensus access patterns. */
typedef struct {
    void (*on_block_ann)   (const uint8_t *payload, size_t len, void *ctx);
    void (*on_tx_ann)      (const uint8_t *payload, size_t len, void *ctx);
    void (*on_commit_vote) (const uint8_t *payload, size_t len, void *ctx);
    void (*on_headers_req) (uint64_t from_height, uint32_t count,
                            const ac_addr_t *peer_id, void *ctx);
    void (*on_block_req)   (uint64_t height, const ac_addr_t *peer_id, void *ctx);
    void *ctx;
} ac_net_callbacks_t;

typedef struct {
    uint16_t           listen_port;
    char               listen_host[64];        /* e.g. "0.0.0.0" */
    char               external_host[128];     /* advertised in HELLO; may be empty */
    uint64_t           chain_id;
    ac_keypair_t       keypair;                /* identity used in HELLO */
    const char       **seed_peers;             /* "host:port" — peers to dial on start */
    size_t             seed_n;
    int                target_outbound;        /* default 8 */
    ac_net_callbacks_t cb;
} ac_net_config_t;

ac_net_t *ac_net_new (const ac_net_config_t *cfg);
void      ac_net_free(ac_net_t *n);

int  ac_net_start(ac_net_t *n);
void ac_net_stop (ac_net_t *n);

/* Broadcast a message to every active peer (skipping `exclude_peer_id`).
 * `exclude_peer_id` may be NULL. */
void ac_net_broadcast(ac_net_t *n, uint8_t msg_type,
                      const uint8_t *payload, size_t len,
                      const ac_addr_t *exclude_peer_id);

/* Send a directed message to a specific peer. Returns 0 on success. */
int  ac_net_send_to(ac_net_t *n, const ac_addr_t *peer_id,
                    uint8_t msg_type, const uint8_t *payload, size_t len);

/* Number of currently-connected peers. */
size_t ac_net_peer_count(ac_net_t *n);

/* This node's own identity + advertised endpoint. `host` is the external host
 * if one was configured, else the listen host. Lets callers report the full
 * network (self + peers), not just the peers this node happens to dial. */
void ac_net_self_info(ac_net_t *n, ac_addr_t *out_id,
                      char *host, size_t host_cap, uint16_t *out_port);

/* Walk over peers (id+host+port). fn returning non-zero stops iteration. */
typedef int (*ac_net_peer_fn)(const ac_addr_t *id, const char *host, uint16_t port,
                              bool inbound, void *ctx);
void ac_net_each_peer(ac_net_t *n, ac_net_peer_fn fn, void *ctx);

#endif /* AGENTCHAIN_NET_H */
