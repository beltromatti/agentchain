/* AgentChain Engine — global state.
 *
 * The state is an in-memory account map plus a names registry, persisted
 * to disk atomically after every committed block. The state root committed
 * in block headers is the cryptographic summary of this structure
 * (PROTOCOL § 11.1).
 *
 * Thread safety: callers must hold the per-state mutex; the chain module
 * is the sole concurrent user.
 */

#ifndef AGENTCHAIN_STATE_H
#define AGENTCHAIN_STATE_H

#include "codec.h"
#include "common.h"

#include <pthread.h>

/* -------------------------------------------------------------------------- */
/* Account.                                                                   */
/* -------------------------------------------------------------------------- */

typedef struct {
    ac_addr_t addr;
    uint64_t  balance;
    uint64_t  nonce;
    uint64_t  stake;
    uint64_t  unbond_at;
} ac_account_t;

/* -------------------------------------------------------------------------- */
/* State container — opaque to other modules.                                 */
/* -------------------------------------------------------------------------- */

typedef struct ac_state_s ac_state_t;

ac_state_t *ac_state_new(uint64_t chain_id);
void        ac_state_free(ac_state_t *s);

uint64_t    ac_state_chain_id(const ac_state_t *s);

/* Lookup. Returns 1 if found and writes into *out (zeroed if not found). */
int  ac_state_get(const ac_state_t *s, const ac_addr_t *addr, ac_account_t *out);

/* Write the account (insert or replace). Empty accounts (all zero non-addr) are
 * removed, per PROTOCOL § 4.1. */
void ac_state_set(ac_state_t *s, const ac_account_t *acc);

/* Returns 1 if the name is currently registered and writes its address into
 * *out_addr. Returns 0 otherwise. */
int  ac_state_name_lookup(const ac_state_t *s, const uint8_t *name, size_t name_len,
                          ac_addr_t *out_addr);
/* Registers a name → addr binding. Returns 0 on success, -1 if name is taken
 * or invalid. */
int  ac_state_name_register(ac_state_t *s, const uint8_t *name, size_t name_len,
                            const ac_addr_t *addr);

/* Compute the state root per PROTOCOL § 11.1. */
void ac_state_root(const ac_state_t *s, ac_hash_t *out);

/* Iterate accounts in lexicographic address order. Returns count. */
size_t      ac_state_count(const ac_state_t *s);
/* Read the i-th account in lex order. Returns 0 if out of range. */
int  ac_state_at(const ac_state_t *s, size_t i, ac_account_t *out);

/* -------------------------------------------------------------------------- */
/* Persistence: serialize / restore full state to/from a byte buffer.         */
/* -------------------------------------------------------------------------- */

/* Returns allocated buffer (caller frees) or NULL on error. */
uint8_t *ac_state_serialize(const ac_state_t *s, size_t *out_len);
/* Replaces the contents of s with the decoded state. Returns 0 on success. */
int      ac_state_deserialize(ac_state_t *s, const uint8_t *buf, size_t len);

/* Convenience: read/write the state file at `path`. */
int      ac_state_load(ac_state_t *s, const char *path);
int      ac_state_save(const ac_state_t *s, const char *path);

/* -------------------------------------------------------------------------- */
/* Apply: state transition for one transaction.                               */
/* -------------------------------------------------------------------------- */

typedef struct {
    bool      ok;             /* true if all checks passed AND body executed cleanly */
    uint64_t  gas_used;       /* gas to charge (always charged, even if body failed) */
    uint64_t  fee_burned;     /* base_fee × gas_used */
    uint64_t  fee_tip;        /* tip × gas_used (to validators) */
    char      err[64];        /* zero-terminated error message; empty on success */
} ac_apply_result_t;

/* Apply a single tx, mutating state. `slot` is the current slot for valid_until
 * checks. `base_fee` is the block's base fee. The reward pool address is the
 * address used to credit the slashing reporter. */
int ac_state_apply_tx(ac_state_t *s,
                      const ac_tx_t *tx,
                      uint64_t slot,
                      uint64_t base_fee,
                      ac_apply_result_t *result);

/* Credit the validator reward pool (system mint at block production). */
void ac_state_credit(ac_state_t *s, const ac_addr_t *addr, uint64_t amount);
/* Debit (no underflow protection — caller checks). */
void ac_state_debit (ac_state_t *s, const ac_addr_t *addr, uint64_t amount);

#endif /* AGENTCHAIN_STATE_H */
