/* AgentChain Engine — chain storage, validation, and fork choice.
 *
 * The chain owns the canonical sequence of committed blocks, the global
 * state mutated by them, and the per-block metadata required by consensus
 * (epoch seeds, current base fee, supply tracking).
 *
 * Thread safety: a single coarse mutex (ac_chain_lock / ac_chain_unlock)
 * serialises all access. Callers are network/consensus/rpc threads.
 */

#ifndef AGENTCHAIN_CHAIN_H
#define AGENTCHAIN_CHAIN_H

#include "codec.h"
#include "common.h"
#include "state.h"

#include <pthread.h>

typedef struct ac_chain_s ac_chain_t;

/* -------------------------------------------------------------------------- */
/* Genesis configuration.                                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
    ac_addr_t  addr;
    uint64_t   balance;
    uint64_t   stake;
} ac_genesis_account_t;

typedef struct {
    uint64_t              chain_id;
    uint64_t              timestamp_ms;       /* deterministic */
    ac_genesis_account_t *accounts;
    size_t                accounts_n;
} ac_genesis_t;

/* Open or create the chain rooted at `data_dir`. Creates the directory tree.
 * If no chain exists yet, requires `genesis` (else NULL is accepted but the
 * call fails with rc=-1). The returned chain owns the state lifecycle. */
ac_chain_t *ac_chain_open(const char *data_dir, const ac_genesis_t *genesis);
void        ac_chain_close(ac_chain_t *c);

/* Coarse locking. */
void ac_chain_lock  (ac_chain_t *c);
void ac_chain_unlock(ac_chain_t *c);

/* Chain accessors (lock-free safe under single-writer). */
uint64_t   ac_chain_height       (const ac_chain_t *c);
uint64_t   ac_chain_chain_id     (const ac_chain_t *c);
uint64_t   ac_chain_base_fee     (const ac_chain_t *c);
uint64_t   ac_chain_genesis_time (const ac_chain_t *c);
const ac_hash_t *ac_chain_tip_hash(const ac_chain_t *c);
/* Wall slot for the current real time. */
uint64_t   ac_chain_current_slot (const ac_chain_t *c);
/* slot start time in unix-ms. */
uint64_t   ac_chain_slot_time_ms (const ac_chain_t *c, uint64_t slot);
/* Epoch number of a slot. */
static inline uint64_t ac_epoch_of(uint64_t slot) { return slot / AC_EPOCH_SLOTS; }
/* The epoch seed used for sortition in `epoch` (PROTOCOL § 6.3). */
void       ac_chain_epoch_seed   (const ac_chain_t *c, uint64_t epoch, ac_hash_t *out);

/* Access committed blocks (returns 0 if not present). */
int        ac_chain_get_block_by_height(const ac_chain_t *c, uint64_t height, ac_block_t *out);
int        ac_chain_get_header_by_height(const ac_chain_t *c, uint64_t height, ac_block_header_t *out);
int        ac_chain_get_block_hash      (const ac_chain_t *c, uint64_t height, ac_hash_t *out);

/* State accessor (read-only); use ac_chain_lock around it. */
ac_state_t *ac_chain_state(ac_chain_t *c);

/* Reward pool & validator selection helpers. */

/* Returns total sqrt-stake across active validators. */
uint64_t   ac_chain_total_sqrt_stake(ac_chain_t *c);
/* Returns the number of active validators. */
size_t     ac_chain_active_count    (ac_chain_t *c);

/* Iterate active validators (lock held by caller). Calls fn(addr, stake, sqrt_stake, ctx)
 * for each. Stop iterating by returning non-zero. */
typedef int (*ac_validator_fn)(const ac_addr_t *addr, uint64_t stake, uint64_t sqrt_stake, void *ctx);
int        ac_chain_each_validator(ac_chain_t *c, ac_validator_fn fn, void *ctx);

/* -------------------------------------------------------------------------- */
/* Block append.                                                              */
/* -------------------------------------------------------------------------- */

typedef enum {
    AC_ACCEPT_OK = 0,
    AC_ACCEPT_DUP,
    AC_ACCEPT_REJECT_BAD_HEADER,
    AC_ACCEPT_REJECT_BAD_PARENT,
    AC_ACCEPT_REJECT_BAD_TIME,
    AC_ACCEPT_REJECT_BAD_PROPOSER,
    AC_ACCEPT_REJECT_BAD_TX,
    AC_ACCEPT_REJECT_BAD_STATE_ROOT,
    AC_ACCEPT_REJECT_BAD_COMMIT,
    AC_ACCEPT_REJECT_BAD_GAS,
    AC_ACCEPT_REJECT_FUTURE,
    AC_ACCEPT_INTERNAL,
} ac_accept_t;

const char *ac_accept_str(ac_accept_t r);

/* Apply a fully-formed block (header + txs + commit). Performs full validation
 * including consensus checks (proposer VRF + committee). Caller must hold the
 * chain lock. */
ac_accept_t ac_chain_accept_block(ac_chain_t *c, const ac_block_t *b);

/* Build a candidate block at `slot` from the given mempool snapshot.
 * The proposer's keypair is `kp`. The block is returned via `*out_block`
 * (uninitialised on failure). The caller owns the txs/signers arrays and
 * must call ac_block_free. Returns 0 on success.
 *
 * This function does NOT include the commit certificate (signers=NULL,
 * nsigners=0). The caller will gather votes via the consensus module and
 * later upgrade the block. */
int ac_chain_build_block(ac_chain_t *c,
                         uint64_t slot,
                         const ac_keypair_t *kp,
                         const ac_tx_t *candidate_txs,
                         uint32_t candidate_n,
                         ac_block_t *out_block);

/* Helper: compute the base fee for the next block (EIP-1559 rule). */
uint64_t   ac_chain_next_base_fee(uint64_t prev_base_fee, uint64_t gas_used, uint64_t gas_limit);

/* Helper: compute the block reward for a given height (PROTOCOL § 8.3). */
uint64_t   ac_chain_block_reward(uint64_t height);

#endif /* AGENTCHAIN_CHAIN_H */
