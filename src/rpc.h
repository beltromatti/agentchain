/* AgentChain Engine — JSON-RPC 2.0 over HTTP/1.1.
 *
 * A minimal embedded HTTP server. Accepts a single JSON-RPC request per
 * connection, replies, closes. Supports the methods listed in
 * TECHNICAL-IMPLEMENTATION.md § RPC.
 */

#ifndef AGENTCHAIN_RPC_H
#define AGENTCHAIN_RPC_H

#include "chain.h"
#include "common.h"
#include "mempool.h"

typedef struct ac_rpc_s ac_rpc_t;

typedef struct {
    uint16_t      port;
    char          host[64];      /* default 127.0.0.1 */
    ac_chain_t   *chain;
    ac_mempool_t *mempool;
} ac_rpc_config_t;

ac_rpc_t *ac_rpc_new (const ac_rpc_config_t *cfg);
void      ac_rpc_free(ac_rpc_t *r);

int       ac_rpc_start(ac_rpc_t *r);
void      ac_rpc_stop (ac_rpc_t *r);

#endif /* AGENTCHAIN_RPC_H */
