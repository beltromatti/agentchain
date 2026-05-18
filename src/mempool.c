#include "mempool.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    ac_tx_t   tx;
    ac_hash_t hash;
    uint64_t  tip;          /* µCRD/gas — sort key, descending */
    uint64_t  added_slot;
} mp_entry_t;

struct ac_mempool_s {
    pthread_mutex_t mu;
    uint64_t        chain_id;
    mp_entry_t     *entries;
    size_t          n;
    size_t          cap;
};

const char *ac_mp_result_str(ac_mp_result_t r) {
    switch (r) {
    case AC_MP_ACCEPTED:           return "accepted";
    case AC_MP_DUPLICATE:          return "duplicate";
    case AC_MP_INVALID_SIG:        return "invalid signature";
    case AC_MP_WRONG_CHAIN:        return "wrong chain_id";
    case AC_MP_BAD_KIND:           return "bad kind";
    case AC_MP_EXPIRED:            return "expired";
    case AC_MP_GAS_BELOW_MIN:      return "gas below minimum";
    case AC_MP_FULL_DROP_LOW_TIP:  return "pool full and tip too low";
    case AC_MP_NONCE_GAP:          return "nonce gap";
    case AC_MP_INSUFFICIENT_FUNDS: return "insufficient funds";
    }
    return "?";
}

ac_mempool_t *ac_mempool_new(uint64_t chain_id) {
    ac_mempool_t *mp = (ac_mempool_t *)calloc(1, sizeof(*mp));
    if (!mp) return NULL;
    pthread_mutex_init(&mp->mu, NULL);
    mp->chain_id = chain_id;
    return mp;
}

void ac_mempool_free(ac_mempool_t *mp) {
    if (!mp) return;
    free(mp->entries);
    pthread_mutex_destroy(&mp->mu);
    free(mp);
}

/* Find by hash. Returns index or -1. Caller holds the lock. */
static ssize_t mp_find(const ac_mempool_t *mp, const ac_hash_t *h) {
    for (size_t i = 0; i < mp->n; ++i) {
        if (ac_hash_eq(&mp->entries[i].hash, h)) return (ssize_t)i;
    }
    return -1;
}

/* Insert in sorted position (tip descending, then added_slot ascending). */
static int mp_insert(ac_mempool_t *mp, const mp_entry_t *e) {
    if (mp->n + 1 > mp->cap) {
        size_t newcap = mp->cap ? mp->cap * 2 : 128;
        if (newcap > AC_MEMPOOL_CAPACITY) newcap = AC_MEMPOOL_CAPACITY;
        mp_entry_t *p = (mp_entry_t *)realloc(mp->entries, newcap * sizeof(*p));
        if (!p) return -1;
        mp->entries = p;
        mp->cap = newcap;
    }
    /* Linear scan for position. */
    size_t pos = mp->n;
    for (size_t i = 0; i < mp->n; ++i) {
        if (mp->entries[i].tip < e->tip) { pos = i; break; }
    }
    if (pos < mp->n) {
        memmove(&mp->entries[pos + 1], &mp->entries[pos],
                (mp->n - pos) * sizeof(*mp->entries));
    }
    mp->entries[pos] = *e;
    mp->n++;
    return 0;
}

ac_mp_result_t ac_mempool_add(ac_mempool_t *mp,
                              ac_state_t   *state,
                              const ac_tx_t *tx,
                              uint64_t current_slot) {
    if (tx->chain_id != mp->chain_id) return AC_MP_WRONG_CHAIN;
    if (tx->version != AC_TX_VERSION) return AC_MP_BAD_KIND;
    if (ac_tx_intrinsic_gas(tx->kind) < 0) return AC_MP_BAD_KIND;
    if (current_slot >= tx->valid_until) return AC_MP_EXPIRED;
    if (tx->gas_limit < ac_tx_total_gas(tx)) return AC_MP_GAS_BELOW_MIN;
    if (!ac_tx_verify(tx)) return AC_MP_INVALID_SIG;

    /* State-based pre-checks. */
    ac_account_t sender;
    ac_state_get(state, &tx->sender, &sender);
    if (tx->nonce != sender.nonce) return AC_MP_NONCE_GAP;
    /* Rough funds check: assume worst-case tip = base_fee = MIN to be permissive. */
    uint64_t max_fee = (uint64_t)tx->gas_limit * (AC_MIN_BASE_FEE + tx->tip);
    uint64_t value = 0;
    if (tx->kind == AC_TX_TRANSFER && tx->body_len >= AC_PUBKEY_SIZE + 8) {
        value = ac_rd64(tx->body + AC_PUBKEY_SIZE);
    }
    if (sender.balance < max_fee + value) return AC_MP_INSUFFICIENT_FUNDS;

    ac_hash_t h;
    ac_tx_hash(&h, tx);

    pthread_mutex_lock(&mp->mu);
    if (mp_find(mp, &h) >= 0) {
        pthread_mutex_unlock(&mp->mu);
        return AC_MP_DUPLICATE;
    }

    /* Replace by-sender-nonce: if the sender already has a tx with the same
     * nonce in the pool, replace it iff the new tip is strictly higher. */
    for (size_t i = 0; i < mp->n; ++i) {
        const ac_tx_t *etx = &mp->entries[i].tx;
        if (etx->nonce == tx->nonce && ac_addr_eq(&etx->sender, &tx->sender)) {
            if (tx->tip <= etx->tip) {
                pthread_mutex_unlock(&mp->mu);
                return AC_MP_DUPLICATE;
            }
            /* Drop existing. */
            memmove(&mp->entries[i], &mp->entries[i + 1],
                    (mp->n - i - 1) * sizeof(*mp->entries));
            mp->n--;
            break;
        }
    }

    /* Capacity handling. */
    if (mp->n >= AC_MEMPOOL_CAPACITY) {
        /* Evict the lowest-tip entry if the new one beats it. */
        if (mp->entries[mp->n - 1].tip >= tx->tip) {
            pthread_mutex_unlock(&mp->mu);
            return AC_MP_FULL_DROP_LOW_TIP;
        }
        mp->n--;
    }

    mp_entry_t e;
    e.tx = *tx;
    e.hash = h;
    e.tip = tx->tip;
    e.added_slot = current_slot;
    int rc = mp_insert(mp, &e);
    pthread_mutex_unlock(&mp->mu);
    return rc == 0 ? AC_MP_ACCEPTED : AC_MP_FULL_DROP_LOW_TIP;
}

int ac_mempool_has(const ac_mempool_t *mp, const ac_hash_t *h) {
    ac_mempool_t *m = (ac_mempool_t *)mp;
    pthread_mutex_lock(&m->mu);
    int rc = mp_find(m, h) >= 0 ? 1 : 0;
    pthread_mutex_unlock(&m->mu);
    return rc;
}

void ac_mempool_remove(ac_mempool_t *mp, const ac_hash_t *h) {
    pthread_mutex_lock(&mp->mu);
    ssize_t i = mp_find(mp, h);
    if (i >= 0) {
        memmove(&mp->entries[i], &mp->entries[i + 1],
                (mp->n - (size_t)i - 1) * sizeof(*mp->entries));
        mp->n--;
    }
    pthread_mutex_unlock(&mp->mu);
}

size_t ac_mempool_size(const ac_mempool_t *mp) {
    ac_mempool_t *m = (ac_mempool_t *)mp;
    pthread_mutex_lock(&m->mu);
    size_t n = m->n;
    pthread_mutex_unlock(&m->mu);
    return n;
}

size_t ac_mempool_prune_expired(ac_mempool_t *mp, uint64_t slot) {
    pthread_mutex_lock(&mp->mu);
    size_t dropped = 0;
    size_t w = 0;
    for (size_t r = 0; r < mp->n; ++r) {
        if (mp->entries[r].tx.valid_until <= slot) {
            dropped++;
            continue;
        }
        if (w != r) mp->entries[w] = mp->entries[r];
        w++;
    }
    mp->n = w;
    pthread_mutex_unlock(&mp->mu);
    return dropped;
}

size_t ac_mempool_snapshot(ac_mempool_t *mp, ac_tx_t *out, size_t max) {
    pthread_mutex_lock(&mp->mu);
    size_t n = mp->n < max ? mp->n : max;
    for (size_t i = 0; i < n; ++i) out[i] = mp->entries[i].tx;
    pthread_mutex_unlock(&mp->mu);
    return n;
}

void ac_mempool_each_hash(ac_mempool_t *mp, ac_mp_hash_fn fn, void *ctx) {
    pthread_mutex_lock(&mp->mu);
    for (size_t i = 0; i < mp->n; ++i) {
        if (fn(&mp->entries[i].hash, ctx) != 0) break;
    }
    pthread_mutex_unlock(&mp->mu);
}
