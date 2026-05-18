/* AgentChain Engine — consensus orchestration.
 *
 * Drives the slot timer, runs leader sortition for the local validator,
 * proposes blocks via chain + mempool, collects votes, and triggers block
 * acceptance once the commit-certificate threshold is reached.
 *
 * The consensus module is decoupled from the wire: callers register a
 * broadcaster callback through which the engine publishes proposals and
 * votes; incoming proposals and votes are pushed via the handle_* functions.
 */

#ifndef AGENTCHAIN_CONSENSUS_H
#define AGENTCHAIN_CONSENSUS_H

#include "chain.h"
#include "codec.h"
#include "common.h"
#include "mempool.h"

#include <pthread.h>

typedef struct ac_consensus_s ac_consensus_t;

/* Broadcaster callback. `type` is the network message type the consensus
 * wants emitted (see net.h):
 *   0x06 BLOCK_ANN — `payload` is an encoded block (`ac_block_encode`).
 *   0x08 COMMIT_VOTE — `payload` is a wire-format vote (see code).
 * `ctx` is the opaque pointer registered with the consensus. */
typedef void (*ac_broadcast_fn)(uint8_t type, const uint8_t *payload, size_t len, void *ctx);

typedef struct {
    ac_chain_t       *chain;
    ac_mempool_t     *mempool;
    ac_keypair_t      keypair;
    ac_broadcast_fn   broadcast;
    void             *broadcast_ctx;
    bool              validator;   /* true if this node should propose / vote  */
} ac_consensus_config_t;

ac_consensus_t *ac_consensus_new (const ac_consensus_config_t *cfg);
void            ac_consensus_free(ac_consensus_t *cs);

int  ac_consensus_start(ac_consensus_t *cs);
void ac_consensus_stop (ac_consensus_t *cs);

/* Called by the network layer when a block proposal is received. */
void ac_consensus_handle_block(ac_consensus_t *cs, const uint8_t *buf, size_t len);

/* Called by the network layer when a commit-vote message is received. */
void ac_consensus_handle_vote (ac_consensus_t *cs, const uint8_t *buf, size_t len);

/* Encoded vote message layout:
 *   u64be(height) || block_hash[32] || signer[32] || sig[64] || vrf_proof[64]   (= 200 B)
 */
#define AC_VOTE_WIRE_LEN (8 + AC_HASH_SIZE + AC_PUBKEY_SIZE + AC_SIG_SIZE + AC_VRF_PROOF_SIZE)

#endif /* AGENTCHAIN_CONSENSUS_H */
