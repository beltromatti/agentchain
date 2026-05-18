/* AgentChain Engine — node daemon orchestration. */

#ifndef AGENTCHAIN_NODE_H
#define AGENTCHAIN_NODE_H

#include "chain.h"
#include "common.h"
#include "consensus.h"
#include "crypto.h"
#include "mempool.h"
#include "net.h"
#include "rpc.h"

typedef struct {
    char           data_dir[512];
    char           genesis_path[512];     /* optional; ignored if chain exists */
    char           listen_host[64];       /* default 0.0.0.0 */
    uint16_t       listen_port;           /* default 30303 */
    char           rpc_host[64];          /* default 127.0.0.1 */
    uint16_t       rpc_port;              /* default 30304 */
    char           external_host[128];    /* advertised in HELLO */
    char         **seed_peers;            /* NULL or array of "host:port" */
    size_t         seed_n;
    bool           validator;             /* run consensus duties */
} ac_node_config_t;

typedef struct ac_node_s ac_node_t;

ac_node_t *ac_node_new (const ac_node_config_t *cfg);
void       ac_node_free(ac_node_t *n);

/* Loads keypair from data_dir/node.key (created if missing) and starts
 * chain, mempool, net, consensus, rpc. Returns 0 on success. */
int        ac_node_start(ac_node_t *n);

/* Stops in reverse order. Blocks until threads have joined. */
void       ac_node_stop (ac_node_t *n);

/* Blocks the caller until SIGINT/SIGTERM, then returns. */
void       ac_node_wait_for_signal(ac_node_t *n);

/* -------------------------------------------------------------------------- */
/* Helpers exposed for CLI use.                                               */
/* -------------------------------------------------------------------------- */

/* Load or generate the node keypair stored at `path` (mode 0600). */
int  ac_node_keypair_load_or_create(const char *path, ac_keypair_t *out, bool *created);

/* Load a genesis configuration from a simple text file (see docs). */
int  ac_node_load_genesis(const char *path, ac_genesis_t *out);
void ac_node_free_genesis(ac_genesis_t *g);

#endif /* AGENTCHAIN_NODE_H */
