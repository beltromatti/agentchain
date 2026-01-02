#pragma once
#ifndef TXPOOL_H
#define TXPOOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include "types.h"

/*
 * txpool.h
 *
 * Thread-safe transaction pool (sorted by `tx->expire` ascending).
 *
 * Ownership model:
 * - After `tx_pool_push(t)`, the pool becomes the owner of `t` and will free it when removed.
 * - The pool frees:
 *     - t->data (if allocated)
 *     - the nodes of t->accounts list (account_list_node), but NOT the account objects themselves
 *     - the tx object itself
 *
 * Requirements:
 * - `types.h` must define:
 *     - struct tx
 *     - struct tx_pool
 *     - struct tx_list_node
 *     - struct account_list_node
 */

/* Global pool instance (defined in txpool.c) */
extern tx_pool TX_POOL;

/*
 * Inserts a transaction in the global pool, keeping it sorted by expire ASC.
 * The pool becomes owner of `t`.
 *
 * Returns: 0 on success, <0 on error.
 */
int tx_pool_push(tx *t);

/*
 * Removes and destroys the first `n` transactions from the global pool (earliest expire).
 *
 * Returns: number actually removed (>= 0).
 */
int tx_pool_pop_n(size_t n);

/*
 * Removes and destroys all expired transactions from the given pool `p` where tx->expire <= now.
 * Since the list is sorted by expire ASC, removal happens from head until first non-expired tx.
 *
 * Returns:
 *   >= 0 number removed
 *   < 0  error (e.g., invalid input)
 */
int remove_expired_txs(tx_pool *p);

/*
 * Clears the pool completely, destroying all pool-owned transactions.
 *
 * Returns:
 *   >= 0 number removed
 *   < 0  error (e.g., invalid input)
 */
int tx_pool_clear(tx_pool *p);

#ifdef __cplusplus
}
#endif

#endif /* TXPOOL_H */
