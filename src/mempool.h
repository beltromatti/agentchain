/* AgentChain Engine — transaction mempool.
 *
 * Stores pending transactions ordered by tip-per-gas (descending). Provides
 * dedup, expiry pruning, capacity-bounded eviction, and bounded-snapshot
 * extraction for block building.
 */

#ifndef AGENTCHAIN_MEMPOOL_H
#define AGENTCHAIN_MEMPOOL_H

#include "codec.h"
#include "common.h"
#include "state.h"

#include <pthread.h>

#define AC_MEMPOOL_CAPACITY 8192

typedef struct ac_mempool_s ac_mempool_t;

ac_mempool_t *ac_mempool_new(uint64_t chain_id);
void          ac_mempool_free(ac_mempool_t *mp);

typedef enum {
    AC_MP_ACCEPTED = 0,
    AC_MP_DUPLICATE,
    AC_MP_INVALID_SIG,
    AC_MP_WRONG_CHAIN,
    AC_MP_BAD_KIND,
    AC_MP_EXPIRED,
    AC_MP_GAS_BELOW_MIN,
    AC_MP_FULL_DROP_LOW_TIP,
    AC_MP_NONCE_GAP,
    AC_MP_INSUFFICIENT_FUNDS,
} ac_mp_result_t;

const char *ac_mp_result_str(ac_mp_result_t r);

/* Adds the tx (deep copy) to the pool. `state` is consulted for nonce and
 * balance pre-checks. Returns the disposition. */
ac_mp_result_t ac_mempool_add(ac_mempool_t *mp,
                              ac_state_t   *state,
                              const ac_tx_t *tx,
                              uint64_t current_slot);

/* Returns 1 if the pool contains a tx with the given hash. */
int  ac_mempool_has(const ac_mempool_t *mp, const ac_hash_t *hash);

/* Removes a tx by hash. */
void ac_mempool_remove(ac_mempool_t *mp, const ac_hash_t *hash);

/* Number of txs currently in the pool. */
size_t ac_mempool_size(const ac_mempool_t *mp);

/* Drops txs with valid_until <= slot. Returns count dropped. */
size_t ac_mempool_prune_expired(ac_mempool_t *mp, uint64_t slot);

/* Extracts up to `max` txs in inclusion order (highest tip first). Writes them
 * to `out` and returns the number written. The pool retains them — only block
 * acceptance removes them. */
size_t ac_mempool_snapshot(ac_mempool_t *mp, ac_tx_t *out, size_t max);

/* Iterate every tx hash (for gossip filtering, status RPC, etc.). */
typedef int (*ac_mp_hash_fn)(const ac_hash_t *h, void *ctx);
void ac_mempool_each_hash(ac_mempool_t *mp, ac_mp_hash_fn fn, void *ctx);

#endif /* AGENTCHAIN_MEMPOOL_H */
