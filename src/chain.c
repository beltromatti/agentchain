#include "chain.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define META_MAGIC       "AGCH:META:v1"
#define META_VERSION     1

#define BLOCK_FILE_NAME  "%012" PRIu64 ".blk"

/* -------------------------------------------------------------------------- */
/* Internal state.                                                            */
/* -------------------------------------------------------------------------- */

typedef struct {
    ac_hash_t  hash;
    uint64_t   height;
    uint32_t   tx_idx;
} tx_index_entry_t;

/* Per-address index over the tx history. Lets RPC clients ask
 * "every tx that touched this account" without rescanning the chain. */
typedef struct {
    ac_addr_t  addr;
    uint64_t   height;
    uint32_t   tx_idx;
    uint8_t    role; /* 0 = sender, 1 = recipient */
} addr_index_entry_t;

/* Per-validator liveness record. last_sign_height is the most recent block
 * height where this address signed the commit certificate. A validator is
 * "live" if (current_height - last_sign_height) < AC_LIVENESS_WINDOW, or if
 * it was bootstrapped at genesis and we are still inside the first window. */
typedef struct {
    ac_addr_t  addr;
    uint64_t   last_sign_height;
} live_entry_t;

#define AC_LIVENESS_WINDOW 16  /* slots; ~32 s of grace before a quiet
                                  validator is dropped from the threshold
                                  denominator. */
#define AC_MAX_LIVE        256 /* hard upper bound; far above realistic
                                  active set size for the v1.x envelope. */

struct ac_chain_s {
    pthread_mutex_t mu;

    char         data_dir[512];
    char         blocks_dir[512];
    char         meta_path[512];
    char         state_path[512];

    ac_state_t  *state;

    uint64_t     chain_id;
    uint64_t     height;            /* of tip; genesis is 0 */
    ac_hash_t    tip_hash;
    uint64_t     last_slot;
    uint64_t     last_timestamp_ms;
    uint64_t     base_fee;
    uint64_t     genesis_timestamp_ms;

    /* Cached epoch seeds. Recompute on demand. */
    uint64_t     cached_epoch;
    ac_hash_t    cached_seed;
    bool         cached_valid;

    /* tx-hash → (height, tx_index) index. Sorted by hash; binary-search
     * lookup. Rebuilt from on-disk blocks at open, updated on accept_block. */
    tx_index_entry_t *tx_index;
    size_t            tx_index_n;
    size_t            tx_index_cap;
    bool              tx_index_sorted;

    /* address → (height, tx_idx, role) index, sorted by address. Lets the
     * RPC return a per-account tx history without rescanning blocks. */
    addr_index_entry_t *addr_index;
    size_t              addr_index_n;
    size_t              addr_index_cap;
    bool                addr_index_sorted;

    /* Liveness map: validators that have signed at least one commit cert in
     * the last AC_LIVENESS_WINDOW blocks. The commit threshold is computed
     * against the sqrt-stake of THIS set rather than the full bonded set,
     * so a newly-bonded-but-still-syncing or an offline validator does not
     * stall the chain. Genesis-bootstrapped validators are pre-populated
     * with last_sign_height=0 and treated as live for the first window. */
    live_entry_t  live_map[AC_MAX_LIVE];
    size_t        live_n;
};

/* -------------------------------------------------------------------------- */
/* Meta file (chain header).                                                  */
/* -------------------------------------------------------------------------- */

static int meta_save(const ac_chain_t *c) {
    uint8_t buf[256];
    size_t pos = 0;
    memcpy(buf + pos, META_MAGIC, sizeof(META_MAGIC) - 1); pos += sizeof(META_MAGIC) - 1;
    buf[pos++] = META_VERSION;
    ac_be64(buf + pos, c->chain_id);             pos += 8;
    ac_be64(buf + pos, c->height);               pos += 8;
    memcpy(buf + pos, c->tip_hash.b, AC_HASH_SIZE); pos += AC_HASH_SIZE;
    ac_be64(buf + pos, c->last_slot);            pos += 8;
    ac_be64(buf + pos, c->last_timestamp_ms);    pos += 8;
    ac_be64(buf + pos, c->base_fee);             pos += 8;
    ac_be64(buf + pos, c->genesis_timestamp_ms); pos += 8;
    return ac_file_write_atomic(c->meta_path, buf, pos, 0600);
}

static int meta_load(ac_chain_t *c) {
    size_t len = 0;
    uint8_t *buf = ac_file_read_all(c->meta_path, &len);
    if (!buf) return -1;
    if (len < sizeof(META_MAGIC) - 1 + 1 + 8 * 6 + AC_HASH_SIZE) { free(buf); return -1; }
    if (memcmp(buf, META_MAGIC, sizeof(META_MAGIC) - 1) != 0)    { free(buf); return -1; }
    size_t pos = sizeof(META_MAGIC) - 1;
    if (buf[pos++] != META_VERSION) { free(buf); return -1; }
    c->chain_id              = ac_rd64(buf + pos); pos += 8;
    c->height                = ac_rd64(buf + pos); pos += 8;
    memcpy(c->tip_hash.b, buf + pos, AC_HASH_SIZE); pos += AC_HASH_SIZE;
    c->last_slot             = ac_rd64(buf + pos); pos += 8;
    c->last_timestamp_ms     = ac_rd64(buf + pos); pos += 8;
    c->base_fee              = ac_rd64(buf + pos); pos += 8;
    c->genesis_timestamp_ms  = ac_rd64(buf + pos); pos += 8;
    free(buf);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Block file IO.                                                             */
/* -------------------------------------------------------------------------- */

static void block_path(const ac_chain_t *c, uint64_t h, char *out, size_t cap) {
    snprintf(out, cap, "%s/" BLOCK_FILE_NAME, c->blocks_dir, h);
}

static int block_write(const ac_chain_t *c, const ac_block_t *b) {
    char path[1024];
    block_path(c, b->header.height, path, sizeof(path));
    uint8_t *buf = NULL;
    size_t buf_len = 0;
    if (ac_block_encode(&buf, &buf_len, b) < 0) return -1;
    int rc = ac_file_write_atomic(path, buf, buf_len, 0600);
    free(buf);
    return rc;
}

static int block_read(const ac_chain_t *c, uint64_t h, ac_block_t *out) {
    char path[1024];
    block_path(c, h, path, sizeof(path));
    size_t len = 0;
    uint8_t *buf = ac_file_read_all(path, &len);
    if (!buf) return -1;
    int rc = ac_block_decode(out, buf, len);
    free(buf);
    return rc < 0 ? -1 : 0;
}

int ac_chain_get_block_by_height(const ac_chain_t *c, uint64_t h, ac_block_t *out) {
    if (h > c->height) return -1;
    return block_read(c, h, out);
}

int ac_chain_get_header_by_height(const ac_chain_t *c, uint64_t h, ac_block_header_t *out) {
    ac_block_t b;
    if (block_read(c, h, &b) < 0) return -1;
    *out = b.header;
    ac_block_free(&b);
    return 0;
}

int ac_chain_get_block_hash(const ac_chain_t *c, uint64_t h, ac_hash_t *out) {
    if (h == c->height) { *out = c->tip_hash; return 0; }
    ac_block_header_t hdr;
    if (ac_chain_get_header_by_height(c, h, &hdr) < 0) return -1;
    ac_block_hash(out, &hdr);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Tx-hash index. Built at open by walking on-disk blocks; updated on accept. */
/* -------------------------------------------------------------------------- */

static int tx_index_cmp(const void *a, const void *b) {
    const tx_index_entry_t *ea = a;
    const tx_index_entry_t *eb = b;
    return memcmp(ea->hash.b, eb->hash.b, AC_HASH_SIZE);
}

static int tx_index_reserve(ac_chain_t *c, size_t need) {
    if (c->tx_index_cap >= need) return 0;
    size_t cap = c->tx_index_cap ? c->tx_index_cap * 2 : 256;
    while (cap < need) cap *= 2;
    void *p = realloc(c->tx_index, cap * sizeof(*c->tx_index));
    if (!p) return -1;
    c->tx_index = p;
    c->tx_index_cap = cap;
    return 0;
}

static int tx_index_add(ac_chain_t *c, const ac_hash_t *h, uint64_t height, uint32_t idx) {
    if (tx_index_reserve(c, c->tx_index_n + 1) < 0) return -1;
    c->tx_index[c->tx_index_n].hash   = *h;
    c->tx_index[c->tx_index_n].height = height;
    c->tx_index[c->tx_index_n].tx_idx = idx;
    c->tx_index_n++;
    c->tx_index_sorted = false;
    return 0;
}

static void tx_index_sort(ac_chain_t *c) {
    if (c->tx_index_sorted) return;
    qsort(c->tx_index, c->tx_index_n, sizeof(*c->tx_index), tx_index_cmp);
    c->tx_index_sorted = true;
}

int ac_chain_tx_find(ac_chain_t *c, const ac_hash_t *tx_hash,
                     uint64_t *out_height, uint32_t *out_tx_index) {
    if (!c->tx_index_n) return 0;
    tx_index_sort(c);
    tx_index_entry_t key;
    key.hash = *tx_hash;
    tx_index_entry_t *r = bsearch(&key, c->tx_index, c->tx_index_n,
                                   sizeof(*c->tx_index), tx_index_cmp);
    if (!r) return 0;
    if (out_height)   *out_height   = r->height;
    if (out_tx_index) *out_tx_index = r->tx_idx;
    return 1;
}

/* Walks the on-disk block range [from, c->height] and inserts every
 * transaction hash into the in-memory index. Called from ac_chain_open after
 * the chain has been opened (height + tip known). */
/* -------------------------------------------------------------------------- */
/* address-history index — addr → list of (height, tx_idx, sender|recipient). */
/* -------------------------------------------------------------------------- */

static int addr_index_cmp(const void *a, const void *b) {
    const addr_index_entry_t *ea = a;
    const addr_index_entry_t *eb = b;
    int c = memcmp(ea->addr.b, eb->addr.b, AC_PUBKEY_SIZE);
    if (c != 0) return c;
    if (ea->height < eb->height) return -1;
    if (ea->height > eb->height) return 1;
    if (ea->tx_idx  < eb->tx_idx ) return -1;
    if (ea->tx_idx  > eb->tx_idx ) return 1;
    return 0;
}

static int addr_index_reserve(ac_chain_t *c, size_t need) {
    if (c->addr_index_cap >= need) return 0;
    size_t cap = c->addr_index_cap ? c->addr_index_cap * 2 : 256;
    while (cap < need) cap *= 2;
    void *p = realloc(c->addr_index, cap * sizeof(*c->addr_index));
    if (!p) return -1;
    c->addr_index = p;
    c->addr_index_cap = cap;
    return 0;
}

static int addr_index_push(ac_chain_t *c, const ac_addr_t *addr,
                           uint64_t height, uint32_t idx, uint8_t role) {
    if (addr_index_reserve(c, c->addr_index_n + 1) < 0) return -1;
    c->addr_index[c->addr_index_n].addr   = *addr;
    c->addr_index[c->addr_index_n].height = height;
    c->addr_index[c->addr_index_n].tx_idx = idx;
    c->addr_index[c->addr_index_n].role   = role;
    c->addr_index_n++;
    c->addr_index_sorted = false;
    return 0;
}

static void addr_index_record_tx(ac_chain_t *c, const ac_tx_t *tx,
                                 uint64_t height, uint32_t idx) {
    addr_index_push(c, &tx->sender, height, idx, /*role=sender*/ 0);
    if (tx->kind == AC_TX_TRANSFER && tx->body_len >= AC_PUBKEY_SIZE) {
        ac_addr_t recipient;
        memcpy(recipient.b, tx->body, AC_PUBKEY_SIZE);
        if (memcmp(recipient.b, tx->sender.b, AC_PUBKEY_SIZE) != 0) {
            addr_index_push(c, &recipient, height, idx, /*role=recipient*/ 1);
        }
    }
}

static void addr_index_sort(ac_chain_t *c) {
    if (c->addr_index_sorted) return;
    qsort(c->addr_index, c->addr_index_n, sizeof(*c->addr_index), addr_index_cmp);
    c->addr_index_sorted = true;
}

/* Public lookup. Writes up to `cap` matching entries (most-recent-first) into
 * `out`. Returns the count. */
size_t ac_chain_addr_txs(ac_chain_t *c, const ac_addr_t *addr,
                         ac_addr_tx_entry_t *out, size_t cap) {
    if (!c->addr_index_n || cap == 0) return 0;
    addr_index_sort(c);
    /* Binary search the lower bound of `addr`. */
    ssize_t lo = 0, hi = (ssize_t)c->addr_index_n;
    while (lo < hi) {
        ssize_t mid = lo + (hi - lo) / 2;
        if (memcmp(c->addr_index[mid].addr.b, addr->b, AC_PUBKEY_SIZE) < 0) lo = mid + 1;
        else                                                                  hi = mid;
    }
    /* Walk forward until address changes. Collect, then reverse to most-recent-first. */
    size_t n = 0;
    ac_addr_tx_entry_t scratch[256];
    size_t scratch_cap = sizeof(scratch) / sizeof(scratch[0]);
    while ((size_t)lo < c->addr_index_n &&
           memcmp(c->addr_index[lo].addr.b, addr->b, AC_PUBKEY_SIZE) == 0 &&
           n < scratch_cap) {
        scratch[n].height = c->addr_index[lo].height;
        scratch[n].tx_idx = c->addr_index[lo].tx_idx;
        scratch[n].role   = c->addr_index[lo].role;
        n++;
        lo++;
    }
    /* Reverse + bound by cap. */
    size_t take = n < cap ? n : cap;
    for (size_t i = 0; i < take; ++i) {
        out[i] = scratch[n - 1 - i];
    }
    return take;
}

/* -------------------------------------------------------------------------- */
/* Liveness map.                                                              */
/* -------------------------------------------------------------------------- */

static void live_record_signer(ac_chain_t *c, const ac_addr_t *a, uint64_t h) {
    for (size_t i = 0; i < c->live_n; ++i) {
        if (memcmp(c->live_map[i].addr.b, a->b, AC_PUBKEY_SIZE) == 0) {
            if (h > c->live_map[i].last_sign_height) c->live_map[i].last_sign_height = h;
            return;
        }
    }
    if (c->live_n < AC_MAX_LIVE) {
        c->live_map[c->live_n].addr = *a;
        c->live_map[c->live_n].last_sign_height = h;
        c->live_n++;
    }
}

static bool live_is_within_window(const ac_chain_t *c, uint64_t last_sign_height) {
    /* Genesis bootstrap window: any pre-populated validator (last_sign_height == 0)
     * is treated as live until the chain has produced LIVENESS_WINDOW blocks. */
    if (last_sign_height == 0 && c->height < AC_LIVENESS_WINDOW) return true;
    if (last_sign_height == 0) return false;
    if (c->height <= last_sign_height) return true;
    return (c->height - last_sign_height) < AC_LIVENESS_WINDOW;
}

/* Total sqrt-stake of bonded validators that have signed at least one block
 * in the recent window. Falls back to the full bonded set when no signers
 * are known (very young chain). Used by try_commit + validate_commit as the
 * denominator of the 2/3 threshold. */
uint64_t ac_chain_live_sqrt_stake(ac_chain_t *c) {
    uint64_t sum = 0;
    size_t   counted = 0;
    for (size_t i = 0; i < c->live_n; ++i) {
        if (!live_is_within_window(c, c->live_map[i].last_sign_height)) continue;
        ac_account_t a;
        if (!ac_state_get(c->state, &c->live_map[i].addr, &a)) continue;
        if (a.stake < AC_MIN_STAKE_UCRD) continue;
        sum += ac_isqrt_u64(a.stake);
        counted++;
    }
    if (counted == 0) {
        /* Fallback: chain too young or every live entry expired. Use the full
         * bonded set so the chain can still progress. */
        return ac_chain_total_sqrt_stake(c);
    }
    return sum;
}

size_t ac_chain_live_count(ac_chain_t *c) {
    size_t n = 0;
    for (size_t i = 0; i < c->live_n; ++i) {
        if (!live_is_within_window(c, c->live_map[i].last_sign_height)) continue;
        ac_account_t a;
        if (!ac_state_get(c->state, &c->live_map[i].addr, &a)) continue;
        if (a.stake >= AC_MIN_STAKE_UCRD) n++;
    }
    return n;
}

/* -------------------------------------------------------------------------- */

static void tx_index_rebuild(ac_chain_t *c) {
    if (c->height == 0) return;
    for (uint64_t h = 1; h <= c->height; ++h) {
        ac_block_t b;
        if (block_read(c, h, &b) < 0) continue;
        for (uint32_t i = 0; i < b.tx_count; ++i) {
            ac_hash_t th;
            ac_tx_hash(&th, &b.txs[i]);
            tx_index_add(c, &th, h, i);
            addr_index_record_tx(c, &b.txs[i], h, i);
        }
        /* Live history: replay signers from the last LIVENESS_WINDOW blocks
         * so a node restored from disk picks up the threshold denominator
         * the rest of the network has been using. */
        if (b.header.height + AC_LIVENESS_WINDOW > c->height) {
            for (uint32_t i = 0; i < b.nsigners; ++i) {
                live_record_signer(c, &b.signers[i].signer, b.header.height);
            }
        }
        ac_block_free(&b);
    }
    tx_index_sort(c);
    addr_index_sort(c);
}

/* -------------------------------------------------------------------------- */
/* Genesis.                                                                   */
/* -------------------------------------------------------------------------- */

static void compute_genesis_seed(uint64_t chain_id, uint64_t ts, ac_hash_t *out) {
    static const char DOMAIN[] = "AGCH:GENESIS:v1";
    uint8_t buf[8 + 8];
    ac_be64(buf,     chain_id);
    ac_be64(buf + 8, ts);
    const uint8_t *chunks[2] = { (const uint8_t *)DOMAIN, buf };
    const size_t   lens[2]   = { sizeof(DOMAIN) - 1,       sizeof(buf) };
    ac_hash_multi(out, chunks, lens, 2);
}

/* System reward-pool account: deterministic, non-spendable address whose
 * balance is the unissued portion of the 60M-CRD validator pool. The first
 * four bytes spell "RWDP" so it is recognisable in tooling. */
static const ac_addr_t SYS_REWARD_POOL = { .b = {0x52,0x57,0x44,0x50,0,0,0,0, 0,0,0,0,0,0,0,0,
                                                  0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0} };

static int chain_apply_genesis(ac_chain_t *c, const ac_genesis_t *g) {
    /* Reset state and apply allocations. */
    /* Seed reward pool with the full 60 M reserved allocation per PROTOCOL § 10. */
    ac_account_t rp;
    memset(&rp, 0, sizeof(rp));
    rp.addr = SYS_REWARD_POOL;
    rp.balance = 60000000ULL * 1000000ULL; /* 60M CRD in µCRD */
    ac_state_set(c->state, &rp);

    for (size_t i = 0; i < g->accounts_n; ++i) {
        const ac_genesis_account_t *ga = &g->accounts[i];
        ac_account_t a;
        memset(&a, 0, sizeof(a));
        a.addr    = ga->addr;
        a.balance = ga->balance;
        a.stake   = ga->stake;
        ac_state_set(c->state, &a);
        /* Bootstrap the liveness map: every genesis-bonded validator is
         * treated as live for the first AC_LIVENESS_WINDOW blocks, so the
         * chain can commit even before anyone has signed yet. */
        if (a.stake >= AC_MIN_STAKE_UCRD && c->live_n < AC_MAX_LIVE) {
            c->live_map[c->live_n].addr = a.addr;
            c->live_map[c->live_n].last_sign_height = 0;
            c->live_n++;
        }
    }

    /* Build the genesis block. */
    ac_block_t b;
    memset(&b, 0, sizeof(b));
    b.header.version       = AC_BLOCK_VERSION;
    b.header.height        = 0;
    b.header.slot          = 0;
    memset(b.header.parent_hash.b, 0, AC_HASH_SIZE);
    b.header.timestamp_ms  = g->timestamp_ms;
    memset(b.header.proposer.b, 0, AC_PUBKEY_SIZE);
    memset(b.header.proposer_vrf_proof, 0, AC_VRF_PROOF_SIZE);
    ac_state_root(c->state, &b.header.state_root);
    memset(b.header.tx_root.b, 0, AC_HASH_SIZE);
    b.header.base_fee  = AC_MIN_BASE_FEE;
    b.header.gas_used  = 0;
    b.header.gas_limit = AC_BLOCK_GAS_LIMIT;
    b.header.tx_count  = 0;

    if (block_write(c, &b) < 0) return -1;

    /* Cache chain meta. */
    c->chain_id = g->chain_id;
    c->height = 0;
    ac_block_hash(&c->tip_hash, &b.header);
    c->last_slot = 0;
    c->last_timestamp_ms = g->timestamp_ms;
    c->base_fee = AC_MIN_BASE_FEE;
    c->genesis_timestamp_ms = g->timestamp_ms;

    if (meta_save(c) < 0) return -1;
    if (ac_state_save(c->state, c->state_path) < 0) return -1;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Lifecycle.                                                                 */
/* -------------------------------------------------------------------------- */

ac_chain_t *ac_chain_open(const char *data_dir, const ac_genesis_t *g) {
    ac_chain_t *c = (ac_chain_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    pthread_mutex_init(&c->mu, NULL);

    snprintf(c->data_dir,   sizeof(c->data_dir),   "%s", data_dir);
    snprintf(c->blocks_dir, sizeof(c->blocks_dir), "%s/blocks", data_dir);
    snprintf(c->meta_path,  sizeof(c->meta_path),  "%s/meta.bin",  data_dir);
    snprintf(c->state_path, sizeof(c->state_path), "%s/state.bin", data_dir);

    if (ac_mkdir_p(c->blocks_dir) != 0) { free(c); return NULL; }

    bool existing = ac_file_exists(c->meta_path);
    if (existing) {
        /* Load meta first to learn chain_id, then create the state with it. */
        if (meta_load(c) != 0) { ac_chain_close(c); return NULL; }
        c->state = ac_state_new(c->chain_id);
        if (!c->state) { ac_chain_close(c); return NULL; }
        if (ac_state_load(c->state, c->state_path) != 0) {
            LOG_E("chain", "state file missing or corrupt at %s", c->state_path);
            ac_chain_close(c);
            return NULL;
        }
    } else {
        if (!g) { LOG_E("chain", "no genesis provided and no chain on disk"); free(c); return NULL; }
        c->state = ac_state_new(g->chain_id);
        if (!c->state) { free(c); return NULL; }
        if (chain_apply_genesis(c, g) < 0) {
            LOG_E("chain", "genesis init failed");
            ac_chain_close(c);
            return NULL;
        }
        LOG_I("chain", "genesis written: chain_id=%" PRIu64 " timestamp=%" PRIu64,
              g->chain_id, g->timestamp_ms);
    }

    /* Build the in-memory tx-hash index by walking on-disk blocks. Cheap
     * on chains where most blocks contain no transactions. */
    tx_index_rebuild(c);

    LOG_I("chain", "opened: chain_id=%" PRIu64 " height=%" PRIu64
          " base_fee=%" PRIu64 " indexed_txs=%zu",
          c->chain_id, c->height, c->base_fee, c->tx_index_n);
    return c;
}

void ac_chain_close(ac_chain_t *c) {
    if (!c) return;
    if (c->state) ac_state_free(c->state);
    free(c->tx_index);
    free(c->addr_index);
    pthread_mutex_destroy(&c->mu);
    free(c);
}

/* Top accounts by selected field. Iterates the state, copies into `out`, sorts
 * descending. Returns the number actually written (≤ cap). */
typedef enum { TOP_BY_BALANCE, TOP_BY_STAKE } top_field_t;

static int top_cmp_balance(const void *a, const void *b) {
    const ac_account_t *aa = a, *ab = b;
    if (aa->balance > ab->balance) return -1;
    if (aa->balance < ab->balance) return 1;
    return memcmp(aa->addr.b, ab->addr.b, AC_PUBKEY_SIZE);
}
static int top_cmp_stake(const void *a, const void *b) {
    const ac_account_t *aa = a, *ab = b;
    if (aa->stake > ab->stake) return -1;
    if (aa->stake < ab->stake) return 1;
    return memcmp(aa->addr.b, ab->addr.b, AC_PUBKEY_SIZE);
}

static size_t chain_top_accounts(ac_chain_t *c, top_field_t f,
                                 ac_account_t *out, size_t cap, bool active_only) {
    if (cap == 0) return 0;
    size_t n = ac_state_count(c->state);
    if (n == 0) return 0;
    ac_account_t *all = (ac_account_t *)malloc(n * sizeof(*all));
    if (!all) return 0;
    size_t k = 0;
    for (size_t i = 0; i < n; ++i) {
        ac_account_t a;
        if (!ac_state_at(c->state, i, &a)) continue;
        if (active_only && a.stake < AC_MIN_STAKE_UCRD) continue;
        all[k++] = a;
    }
    qsort(all, k, sizeof(*all),
          f == TOP_BY_BALANCE ? top_cmp_balance : top_cmp_stake);
    size_t take = k < cap ? k : cap;
    for (size_t i = 0; i < take; ++i) out[i] = all[i];
    free(all);
    return take;
}

size_t ac_chain_top_accounts_by_balance(ac_chain_t *c, ac_account_t *out, size_t cap) {
    return chain_top_accounts(c, TOP_BY_BALANCE, out, cap, false);
}
size_t ac_chain_top_validators(ac_chain_t *c, ac_account_t *out, size_t cap) {
    return chain_top_accounts(c, TOP_BY_STAKE, out, cap, true);
}

void ac_chain_lock  (ac_chain_t *c) { pthread_mutex_lock(&c->mu); }
void ac_chain_unlock(ac_chain_t *c) { pthread_mutex_unlock(&c->mu); }

uint64_t   ac_chain_height       (const ac_chain_t *c) { return c->height; }
uint64_t   ac_chain_chain_id     (const ac_chain_t *c) { return c->chain_id; }
uint64_t   ac_chain_base_fee     (const ac_chain_t *c) { return c->base_fee; }
uint64_t   ac_chain_genesis_time (const ac_chain_t *c) { return c->genesis_timestamp_ms; }
const ac_hash_t *ac_chain_tip_hash(const ac_chain_t *c) { return &c->tip_hash; }
ac_state_t *ac_chain_state       (ac_chain_t *c)       { return c->state; }

uint64_t ac_chain_current_slot(const ac_chain_t *c) {
    uint64_t now = ac_now_ms();
    if (now < c->genesis_timestamp_ms) return 0;
    return (now - c->genesis_timestamp_ms) / AC_SLOT_DURATION_MS;
}

uint64_t ac_chain_slot_time_ms(const ac_chain_t *c, uint64_t slot) {
    return c->genesis_timestamp_ms + slot * AC_SLOT_DURATION_MS;
}

/* -------------------------------------------------------------------------- */
/* Epoch seeds (PROTOCOL § 6.3).                                              */
/* -------------------------------------------------------------------------- */

static void compute_epoch_seed(ac_chain_t *c, uint64_t epoch, ac_hash_t *out) {
    if (epoch == 0) {
        compute_genesis_seed(c->chain_id, c->genesis_timestamp_ms, out);
        return;
    }
    uint64_t prev_last_slot_block_height = 0;
    /* We need the header at the last block of epoch e-1. In v1 we scan back
     * through committed blocks until the slot falls in epoch (e-1). Heights are
     * not strictly aligned with slots (slots can be empty), so we search. */
    uint64_t epoch_start_slot = epoch * AC_EPOCH_SLOTS;
    uint64_t h = c->height;
    while (h > 0) {
        ac_block_header_t hdr;
        if (ac_chain_get_header_by_height(c, h, &hdr) < 0) {
            /* Shouldn't happen. Fall back to genesis. */
            compute_genesis_seed(c->chain_id, c->genesis_timestamp_ms, out);
            return;
        }
        if (hdr.slot < epoch_start_slot) {
            prev_last_slot_block_height = h;
            break;
        }
        h--;
    }
    if (prev_last_slot_block_height == 0 && c->height > 0) {
        ac_block_header_t hdr;
        if (ac_chain_get_header_by_height(c, 0, &hdr) == 0 &&
            hdr.slot < epoch_start_slot) {
            prev_last_slot_block_height = 0;
        } else {
            compute_genesis_seed(c->chain_id, c->genesis_timestamp_ms, out);
            return;
        }
    }
    ac_block_header_t prev_hdr;
    if (ac_chain_get_header_by_height(c, prev_last_slot_block_height, &prev_hdr) < 0) {
        compute_genesis_seed(c->chain_id, c->genesis_timestamp_ms, out);
        return;
    }
    /* seed[e] = blake2b("AGCH:SEED" || prev_hdr.proposer_vrf_proof) */
    static const char DOMAIN[] = "AGCH:SEED";
    const uint8_t *chunks[2] = { (const uint8_t *)DOMAIN, prev_hdr.proposer_vrf_proof };
    const size_t   lens[2]   = { sizeof(DOMAIN) - 1,      AC_VRF_PROOF_SIZE };
    ac_hash_multi(out, chunks, lens, 2);
}

void ac_chain_epoch_seed(const ac_chain_t *cc, uint64_t epoch, ac_hash_t *out) {
    ac_chain_t *c = (ac_chain_t *)cc; /* mutable cache */
    if (c->cached_valid && c->cached_epoch == epoch) {
        *out = c->cached_seed;
        return;
    }
    compute_epoch_seed(c, epoch, out);
    c->cached_epoch = epoch;
    c->cached_seed  = *out;
    c->cached_valid = true;
}

/* -------------------------------------------------------------------------- */
/* Active validators.                                                         */
/* -------------------------------------------------------------------------- */

int ac_chain_each_validator(ac_chain_t *c, ac_validator_fn fn, void *ctx) {
    size_t n = ac_state_count(c->state);
    for (size_t i = 0; i < n; ++i) {
        ac_account_t a;
        ac_state_at(c->state, i, &a);
        if (a.stake < AC_MIN_STAKE_UCRD) continue;
        uint64_t sq = ac_isqrt_u64(a.stake);
        int rc = fn(&a.addr, a.stake, sq, ctx);
        if (rc != 0) return rc;
    }
    return 0;
}

static int sum_sqrt_stake_cb(const ac_addr_t *a, uint64_t s, uint64_t sq, void *ctx) {
    (void)a; (void)s;
    *(uint64_t *)ctx += sq;
    return 0;
}
static int count_cb(const ac_addr_t *a, uint64_t s, uint64_t sq, void *ctx) {
    (void)a; (void)s; (void)sq;
    *(size_t *)ctx += 1;
    return 0;
}

uint64_t ac_chain_total_sqrt_stake(ac_chain_t *c) {
    uint64_t sum = 0;
    ac_chain_each_validator(c, sum_sqrt_stake_cb, &sum);
    return sum;
}
size_t ac_chain_active_count(ac_chain_t *c) {
    size_t n = 0;
    ac_chain_each_validator(c, count_cb, &n);
    return n;
}

/* -------------------------------------------------------------------------- */
/* Block reward and fee market.                                               */
/* -------------------------------------------------------------------------- */

uint64_t ac_chain_block_reward(uint64_t height) {
    /* PROTOCOL § 8.3. Slots per year ≈ 15,778,800. */
    const uint64_t SLOTS_PER_YEAR = 15778800ULL;
    uint64_t year = height / SLOTS_PER_YEAR;
    uint64_t annual_emission_ucrd;
    if (year < 10) annual_emission_ucrd = 2000000ULL * 1000000ULL;
    else           annual_emission_ucrd =  500000ULL * 1000000ULL;
    return annual_emission_ucrd / SLOTS_PER_YEAR;
}

uint64_t ac_chain_next_base_fee(uint64_t prev_base_fee, uint64_t gas_used, uint64_t gas_limit) {
    uint64_t target = gas_limit / 2;
    if (gas_used == target) return prev_base_fee;
    /* delta = base_fee × (gas_used - target) / target / 8  (signed) */
    int64_t diff = (int64_t)gas_used - (int64_t)target;
    /* Avoid overflow: use 128-bit-ish stepwise math. */
    int64_t delta;
    if (diff >= 0) {
        uint64_t inc = prev_base_fee * (uint64_t)diff / target / 8ULL;
        if (inc == 0) inc = 1; /* always nudge */
        delta = (int64_t)inc;
    } else {
        uint64_t dec = prev_base_fee * (uint64_t)(-diff) / target / 8ULL;
        if (dec == 0) dec = 1;
        delta = -(int64_t)dec;
    }
    int64_t next = (int64_t)prev_base_fee + delta;
    if (next < (int64_t)AC_MIN_BASE_FEE) next = AC_MIN_BASE_FEE;
    return (uint64_t)next;
}

/* -------------------------------------------------------------------------- */
/* Sortition primitives.                                                      */
/* -------------------------------------------------------------------------- */

/* Compute the leader-VRF input alpha for slot in epoch. */
static int leader_alpha(uint8_t out[64], const ac_hash_t *seed, uint64_t slot) {
    static const char DOMAIN[] = "AGCH:LEADER";
    memcpy(out, DOMAIN, sizeof(DOMAIN) - 1);
    memcpy(out + sizeof(DOMAIN) - 1, seed->b, AC_HASH_SIZE);
    ac_be64(out + sizeof(DOMAIN) - 1 + AC_HASH_SIZE, slot);
    return (int)(sizeof(DOMAIN) - 1 + AC_HASH_SIZE + 8);
}
/* Compute the committee-VRF input alpha for slot in epoch. */
static int committee_alpha(uint8_t out[64], const ac_hash_t *seed, uint64_t slot) {
    static const char DOMAIN[] = "AGCH:COMMITTEE";
    memcpy(out, DOMAIN, sizeof(DOMAIN) - 1);
    memcpy(out + sizeof(DOMAIN) - 1, seed->b, AC_HASH_SIZE);
    ac_be64(out + sizeof(DOMAIN) - 1 + AC_HASH_SIZE, slot);
    return (int)(sizeof(DOMAIN) - 1 + AC_HASH_SIZE + 8);
}

/* Compute leader priority. The "priority" is the VRF output's first 16 bytes
 * interpreted as Q.128, divided by sqrt_stake. Lower = higher priority.
 * Used by the consensus module for proposer tie-breaking. */
__attribute__((unused))
static void priority_of(uint8_t out[16], const ac_vrf_out_t *beta, uint64_t sqrt_stake) {
    /* Take first 16 bytes of beta as priority "score", then divide by sqrt_stake.
     * Implementation: priority is (score / sqrt_stake), comparing big-endian. */
    /* For simplicity, scale by 1/sqrt_stake using 128-bit arithmetic stepwise. */
    if (sqrt_stake == 0) { memset(out, 0xff, 16); return; }
    /* Treat first 8 bytes of beta as hi64. To make the comparison meaningful at
     * scale, compute hi64 / sqrt_stake and lo64 = remainder bytes. */
    uint64_t hi = ac_rd64(beta->b);
    uint64_t lo = ac_rd64(beta->b + 8);
    uint64_t q_hi = hi / sqrt_stake;
    uint64_t r_hi = hi % sqrt_stake;
    /* lo_total = r_hi * 2^64 + lo; divide by sqrt_stake.
     * Approximate by ignoring overflow: q_lo ≈ lo / sqrt_stake. We add the
     * carry from r_hi by adding r_hi * (2^64 / sqrt_stake) to q_lo, which is
     * an upper bound. */
    uint64_t q_lo = lo / sqrt_stake + (sqrt_stake ? (UINT64_MAX / sqrt_stake) * r_hi : 0);
    ac_be64(out,     q_hi);
    ac_be64(out + 8, q_lo);
}

typedef struct {
    ac_addr_t  addr;
    uint64_t   stake;
    uint64_t   sqrt_stake;
    uint8_t    priority[16];
    bool       found;
} leader_search_t;

/* Compute committee threshold for slot:
 *   draw_threshold = (sqrt_stake / total_sqrt) * COMMITTEE_TARGET
 * Returns 1 if eligible, 0 otherwise. `draw` is the first 8 bytes of the VRF
 * output interpreted as uniform [0,1). */
static int committee_eligible(uint64_t draw_u64, uint64_t sqrt_stake, uint64_t total_sqrt) {
    if (total_sqrt == 0 || sqrt_stake == 0) return 0;
    /* probability = sqrt_stake / total_sqrt * COMMITTEE_TARGET, capped at 1. */
    /* threshold_u64 = probability × UINT64_MAX. */
    /* probability_num = sqrt_stake * COMMITTEE_TARGET; probability_den = total_sqrt. */
    long double p = (long double)sqrt_stake * (long double)AC_COMMITTEE_TARGET
                  / (long double)total_sqrt;
    if (p >= 1.0L) return 1;
    long double thr = p * (long double)UINT64_MAX;
    return draw_u64 < (uint64_t)thr;
}

/* -------------------------------------------------------------------------- */
/* Block acceptance.                                                          */
/* -------------------------------------------------------------------------- */

const char *ac_accept_str(ac_accept_t r) {
    switch (r) {
    case AC_ACCEPT_OK:                     return "ok";
    case AC_ACCEPT_DUP:                    return "duplicate";
    case AC_ACCEPT_REJECT_BAD_HEADER:      return "bad header";
    case AC_ACCEPT_REJECT_BAD_PARENT:      return "bad parent";
    case AC_ACCEPT_REJECT_BAD_TIME:        return "bad timestamp";
    case AC_ACCEPT_REJECT_BAD_PROPOSER:    return "bad proposer";
    case AC_ACCEPT_REJECT_BAD_TX:          return "bad tx";
    case AC_ACCEPT_REJECT_BAD_STATE_ROOT:  return "bad state_root";
    case AC_ACCEPT_REJECT_BAD_COMMIT:      return "insufficient commit";
    case AC_ACCEPT_REJECT_BAD_GAS:         return "bad gas accounting";
    case AC_ACCEPT_REJECT_FUTURE:          return "from the future";
    case AC_ACCEPT_INTERNAL:               return "internal error";
    }
    return "?";
}

/* Validate that `proposer` is the canonical leader at `slot`. Two checks:
 *   1. The proposer's VRF proof verifies under the proposer pubkey and the
 *      epoch seed.
 *   2. No other active validator could plausibly have produced a lower
 *      priority. We approximate this by requiring the priority to be below
 *      a network-wide threshold (the lowest possible priority an attacker
 *      with the same stake could produce).
 * In v1 reference: any verifiable VRF proof from an active validator is
 * accepted as leader; the lowest-priority tie-break is resolved at fork
 * choice (PROTOCOL § 7). */
static bool validate_proposer_vrf(ac_chain_t *c, const ac_block_header_t *h) {
    /* Look up the proposer; must be an active validator. */
    ac_account_t acc;
    if (!ac_state_get(c->state, &h->proposer, &acc)) return false;
    if (acc.stake < AC_MIN_STAKE_UCRD) return false;

    ac_hash_t seed;
    ac_chain_epoch_seed(c, ac_epoch_of(h->slot), &seed);

    uint8_t alpha[64];
    int an = leader_alpha(alpha, &seed, h->slot);

    ac_vrf_proof_t proof;
    memcpy(proof.b, h->proposer_vrf_proof, AC_VRF_PROOF_SIZE);
    ac_vrf_out_t beta;
    if (!ac_vrf_verify(&beta, &proof, alpha, (size_t)an, h->proposer.b)) return false;
    return true;
}

/* File-scope helper used by accept_block when validating the commit
 * certificate against the *pre-block* validator set. The context carries
 * a small parallel array (signer address -> pre-block stake). */
struct ac_pre_lookup_ctx {
    const ac_addr_t *addrs;
    const uint64_t  *stakes;
    uint32_t         n;
};

static uint64_t ac_chain__pre_stake_lookup(const ac_addr_t *signer, void *ctx) {
    const struct ac_pre_lookup_ctx *c = (const struct ac_pre_lookup_ctx *)ctx;
    for (uint32_t i = 0; i < c->n; ++i) {
        if (ac_addr_eq(&c->addrs[i], signer)) return c->stakes[i];
    }
    return 0;
}

/* Validate the commit certificate using the *pre-block* validator set.
 * This matters because a STAKE_BOND/STAKE_UNBOND transaction in the same
 * block changes which keys are eligible to sign: if we used the post-block
 * set, a block that introduces a brand-new validator could never reach the
 * 2/3 threshold (the new validator wasn't yet in the committee), making
 * stake changes uncommittable. The fix is to pin the threshold to the
 * stake table the committee was actually sampled from.
 *
 * `pre_total_sqrt` is the sum of sqrt_stake across active validators *before*
 * any tx in this block has been applied. `pre_stake_of` maps signer address
 * to their pre-block stake (used to compute sqrt-weight per signer). */
typedef uint64_t (*signer_stake_lookup_fn)(const ac_addr_t *signer, void *ctx);

static int validate_commit(ac_chain_t *c, const ac_block_t *b, const ac_hash_t *block_hash,
                           uint64_t pre_total_sqrt,
                           signer_stake_lookup_fn pre_stake_of, void *lookup_ctx) {
    if (b->nsigners == 0) {
        return -1; /* commit required for non-genesis */
    }
    if (pre_total_sqrt == 0) return -1;

    /* Each signer must be an active validator, have a valid VRF proof for the
     * committee, and a valid commit signature on the vote message. */
    ac_hash_t seed;
    ac_chain_epoch_seed(c, ac_epoch_of(b->header.slot), &seed);

    uint8_t calpha[64];
    int can = committee_alpha(calpha, &seed, b->header.slot);

    uint8_t vmsg[64];
    int vn = ac_vote_message(vmsg, b->header.height, block_hash);

    uint64_t signed_sqrt = 0;

    for (uint32_t i = 0; i < b->nsigners; ++i) {
        uint64_t pre_stake = pre_stake_of(&b->signers[i].signer, lookup_ctx);
        if (pre_stake < AC_MIN_STAKE_UCRD) return -1;
        uint64_t sqrt_st = ac_isqrt_u64(pre_stake);

        ac_vrf_proof_t proof;
        memcpy(proof.b, b->signers[i].vrf_proof, AC_VRF_PROOF_SIZE);
        ac_vrf_out_t beta;
        if (!ac_vrf_verify(&beta, &proof, calpha, (size_t)can, b->signers[i].signer.b)) return -1;
        uint64_t draw = ac_rd64(beta.b);
        if (!committee_eligible(draw, sqrt_st, pre_total_sqrt)) return -1;

        if (!ac_verify(&b->signers[i].sig, vmsg, (size_t)vn, b->signers[i].signer.b)) return -1;

        signed_sqrt += sqrt_st;
    }

    /* Liveness condition: ≥ 2/3 of the *live* sqrt-stake — the subset of
     * bonded validators that signed at least one block in the recent
     * AC_LIVENESS_WINDOW slots. This keeps the chain progressing when a
     * freshly-bonded or temporarily-offline validator would otherwise raise
     * the denominator past what the rest of the network can satisfy.
     * Committee eligibility above still uses pre_total_sqrt so sortition
     * probabilities stay correctly calibrated against the full active set. */
    uint64_t live_sqrt = ac_chain_live_sqrt_stake(c);
    if (live_sqrt == 0) live_sqrt = pre_total_sqrt;
    if (signed_sqrt * 3 < live_sqrt * 2) return -1;

    /* Safety floor: the signers must also exceed 1/2 of the *total* bonded
     * sqrt-stake. Two disjoint signer sets cannot both exceed half the total
     * (quorum intersection), so this guarantees at most one block can finalise
     * per height even under a network partition. Without it, the live-set
     * denominator can shrink independently on each side of a split and let a
     * minority finalise conflicting blocks — the split-brain fork observed on
     * mainnet alpha at h=82033, where the seed (alone) and the belimo+droovy
     * pair each committed a different block. The live-set rule alone optimises
     * for liveness at the cost of this safety property; the floor restores it
     * while preserving the no-freeze behaviour for honest, connected quorums. */
    if (signed_sqrt * 2 <= pre_total_sqrt) return -1;
    return 0;
}

ac_accept_t ac_chain_accept_block(ac_chain_t *c, const ac_block_t *b) {
    if (b->header.version != AC_BLOCK_VERSION) return AC_ACCEPT_REJECT_BAD_HEADER;

    /* Genesis cannot be re-accepted via this path. */
    if (b->header.height == 0) return AC_ACCEPT_DUP;

    /* Already have this height? Accept iff hash matches (idempotent). */
    if (b->header.height <= c->height) {
        ac_hash_t h;
        if (ac_chain_get_block_hash(c, b->header.height, &h) == 0) {
            ac_hash_t given;
            ac_block_hash(&given, &b->header);
            return ac_hash_eq(&h, &given) ? AC_ACCEPT_DUP : AC_ACCEPT_REJECT_BAD_PARENT;
        }
        return AC_ACCEPT_REJECT_BAD_PARENT;
    }

    /* Must extend the current tip (no fork-following in v1 reference). */
    if (b->header.height != c->height + 1) return AC_ACCEPT_REJECT_BAD_PARENT;
    if (!ac_hash_eq(&b->header.parent_hash, &c->tip_hash)) return AC_ACCEPT_REJECT_BAD_PARENT;

    /* Time. */
    if (b->header.slot <= c->last_slot) return AC_ACCEPT_REJECT_BAD_TIME;
    uint64_t slot_start = ac_chain_slot_time_ms(c, b->header.slot);
    if ((int64_t)b->header.timestamp_ms < (int64_t)slot_start - 3000 ||
        (int64_t)b->header.timestamp_ms > (int64_t)slot_start + 3000) {
        return AC_ACCEPT_REJECT_BAD_TIME;
    }
    uint64_t now = ac_now_ms();
    if (b->header.timestamp_ms > now + 1500) return AC_ACCEPT_REJECT_FUTURE;

    if (b->header.gas_limit != AC_BLOCK_GAS_LIMIT) return AC_ACCEPT_REJECT_BAD_HEADER;

    /* Proposer must be an active validator with a valid VRF proof for this slot. */
    if (!validate_proposer_vrf(c, &b->header)) return AC_ACCEPT_REJECT_BAD_PROPOSER;

    /* Base fee adjustment. */
    uint64_t expected_base_fee = ac_chain_next_base_fee(c->base_fee,
                                                       /* previous block's gas_used: */ 0,
                                                       AC_BLOCK_GAS_LIMIT);
    /* For the first block after genesis we don't know "previous gas_used" since
     * genesis has none. We accept any value ≥ MIN_BASE_FEE for the very first
     * non-genesis block, and validate the EIP-1559 step thereafter. */
    if (c->height >= 1) {
        ac_block_header_t prev = {0};
        if (ac_chain_get_header_by_height(c, c->height, &prev) == 0) {
            expected_base_fee = ac_chain_next_base_fee(prev.base_fee, prev.gas_used,
                                                       prev.gas_limit);
        }
    } else {
        expected_base_fee = AC_MIN_BASE_FEE;
    }
    if (b->header.base_fee < AC_MIN_BASE_FEE) return AC_ACCEPT_REJECT_BAD_HEADER;
    /* Allow ±1 µCRD/gas tolerance to absorb rounding. */
    if (b->header.base_fee + 1 < expected_base_fee ||
        b->header.base_fee > expected_base_fee + 1) {
        return AC_ACCEPT_REJECT_BAD_HEADER;
    }

    /* Capture pre-block validator metrics for commit validation. A
     * STAKE_BOND / STAKE_UNBOND tx in this block can shift the active set;
     * the committee that signed this block was sampled against the
     * pre-block set, so the 2/3 threshold must be evaluated against the
     * same pre-block table. */
    uint64_t pre_total_sqrt = ac_chain_total_sqrt_stake(c);
    ac_addr_t pre_signer_addrs[AC_COMMITTEE_MAX];
    uint64_t  pre_signer_stake[AC_COMMITTEE_MAX];
    uint32_t  pre_signer_n = b->nsigners;
    for (uint32_t i = 0; i < b->nsigners && i < AC_COMMITTEE_MAX; ++i) {
        ac_account_t acc;
        ac_state_get(c->state, &b->signers[i].signer, &acc);
        pre_signer_addrs[i] = b->signers[i].signer;
        pre_signer_stake[i] = acc.stake;
    }

    /* Apply transactions. We work on a temporary state by serializing and
     * restoring on failure. */
    size_t snap_len = 0;
    uint8_t *snap = ac_state_serialize(c->state, &snap_len);
    if (!snap) return AC_ACCEPT_INTERNAL;

    uint64_t total_gas = 0;
    uint64_t total_tips = 0;
    bool tx_ok = true;
    for (uint32_t i = 0; i < b->tx_count; ++i) {
        if (!ac_tx_verify(&b->txs[i])) { tx_ok = false; break; }
        ac_apply_result_t r;
        int rc = ac_state_apply_tx(c->state, &b->txs[i], b->header.slot,
                                   b->header.base_fee, &r);
        if (rc < 0) { tx_ok = false; break; }
        total_gas += r.gas_used;
        total_tips += r.fee_tip;
        if (total_gas > AC_BLOCK_GAS_LIMIT) { tx_ok = false; break; }
    }
    if (!tx_ok || total_gas != b->header.gas_used) {
        ac_state_deserialize(c->state, snap, snap_len);
        free(snap);
        return tx_ok ? AC_ACCEPT_REJECT_BAD_GAS : AC_ACCEPT_REJECT_BAD_TX;
    }

    /* Compute and check tx_root. */
    ac_hash_t expected_tx_root;
    ac_block_tx_root(&expected_tx_root, b->txs, b->tx_count);
    if (!ac_hash_eq(&expected_tx_root, &b->header.tx_root)) {
        ac_state_deserialize(c->state, snap, snap_len);
        free(snap);
        return AC_ACCEPT_REJECT_BAD_TX;
    }

    /* Pay block reward + tips entirely to the leader (PROTOCOL § 8.3). */
    uint64_t reward = ac_chain_block_reward(b->header.height);
    ac_state_credit(c->state, &b->header.proposer, reward + total_tips);
    ac_state_debit (c->state, &SYS_REWARD_POOL,    reward);

    /* Compute final state root. */
    ac_hash_t my_root;
    ac_state_root(c->state, &my_root);
    if (!ac_hash_eq(&my_root, &b->header.state_root)) {
        ac_state_deserialize(c->state, snap, snap_len);
        free(snap);
        return AC_ACCEPT_REJECT_BAD_STATE_ROOT;
    }

    /* Validate the commit certificate against the pre-block validator set
     * (so a STAKE_BOND in this very block does not break self-referentially
     * the threshold the committee was meant to clear). */
    ac_hash_t block_hash;
    ac_block_hash(&block_hash, &b->header);

    struct ac_pre_lookup_ctx pre_ctx = {
        .addrs  = pre_signer_addrs,
        .stakes = pre_signer_stake,
        .n      = pre_signer_n,
    };
    if (validate_commit(c, b, &block_hash, pre_total_sqrt,
                        ac_chain__pre_stake_lookup, &pre_ctx) < 0) {
        ac_state_deserialize(c->state, snap, snap_len);
        free(snap);
        return AC_ACCEPT_REJECT_BAD_COMMIT;
    }

    free(snap);

    /* Persist block, state, meta. */
    if (block_write(c, b) < 0)                        return AC_ACCEPT_INTERNAL;
    if (ac_state_save(c->state, c->state_path) < 0)   return AC_ACCEPT_INTERNAL;

    c->height = b->header.height;
    c->tip_hash = block_hash;
    c->last_slot = b->header.slot;
    c->last_timestamp_ms = b->header.timestamp_ms;
    c->base_fee = b->header.base_fee;
    if (meta_save(c) < 0)                             return AC_ACCEPT_INTERNAL;

    /* Update liveness map: every signer of this block's commit cert is
     * recorded as alive at this height. The threshold denominator will count
     * them for the next AC_LIVENESS_WINDOW slots; if they go quiet they
     * fall off and the denominator shrinks. */
    for (uint32_t i = 0; i < b->nsigners; ++i) {
        live_record_signer(c, &b->signers[i].signer, b->header.height);
    }

    /* Update the tx-hash index + address-history index for any transactions
     * in this block. */
    for (uint32_t i = 0; i < b->tx_count; ++i) {
        addr_index_record_tx(c, &b->txs[i], b->header.height, i);
    }
    for (uint32_t i = 0; i < b->tx_count; ++i) {
        ac_hash_t th;
        ac_tx_hash(&th, &b->txs[i]);
        tx_index_add(c, &th, b->header.height, i);
    }

    /* Invalidate cached epoch seed if epoch boundary crossed. */
    c->cached_valid = false;

    return AC_ACCEPT_OK;
}

/* -------------------------------------------------------------------------- */
/* Block construction.                                                        */
/* -------------------------------------------------------------------------- */

int ac_chain_build_block(ac_chain_t *c,
                         uint64_t slot,
                         const ac_keypair_t *kp,
                         const ac_tx_t *candidate_txs,
                         uint32_t candidate_n,
                         ac_block_t *out_block) {
    memset(out_block, 0, sizeof(*out_block));

    /* Ensure proposer is active. */
    ac_addr_t proposer;
    memcpy(proposer.b, kp->pk, AC_PUBKEY_SIZE);
    ac_account_t acc;
    if (!ac_state_get(c->state, &proposer, &acc)) return -1;
    if (acc.stake < AC_MIN_STAKE_UCRD) return -1;

    /* Compute VRF proof. */
    ac_hash_t seed;
    ac_chain_epoch_seed(c, ac_epoch_of(slot), &seed);
    uint8_t alpha[64];
    int an = leader_alpha(alpha, &seed, slot);
    ac_vrf_proof_t proof;
    ac_vrf_prove(&proof, NULL, alpha, (size_t)an, kp);

    /* Compute base fee. */
    uint64_t base_fee;
    if (c->height == 0) {
        base_fee = AC_MIN_BASE_FEE;
    } else {
        ac_block_header_t prev = {0};
        if (ac_chain_get_header_by_height(c, c->height, &prev) < 0) {
            base_fee = AC_MIN_BASE_FEE;
        } else {
            base_fee = ac_chain_next_base_fee(prev.base_fee, prev.gas_used, prev.gas_limit);
        }
    }

    /* Snapshot state, apply txs, compute state_root. */
    size_t snap_len = 0;
    uint8_t *snap = ac_state_serialize(c->state, &snap_len);
    if (!snap) return -1;

    ac_tx_t *kept = (ac_tx_t *)calloc(candidate_n + 1, sizeof(ac_tx_t));
    if (!kept) { free(snap); return -1; }
    uint32_t kept_n = 0;
    uint64_t total_gas = 0;
    uint64_t total_tips = 0;

    for (uint32_t i = 0; i < candidate_n; ++i) {
        if (!ac_tx_verify(&candidate_txs[i])) continue;
        if (total_gas + ac_tx_total_gas(&candidate_txs[i]) > AC_BLOCK_GAS_LIMIT) continue;
        ac_apply_result_t r;
        int rc = ac_state_apply_tx(c->state, &candidate_txs[i], slot, base_fee, &r);
        if (rc < 0) continue;
        kept[kept_n++] = candidate_txs[i];
        total_gas += r.gas_used;
        total_tips += r.fee_tip;
    }

    /* Pay reward into local state for state_root computation. */
    uint64_t reward = ac_chain_block_reward(c->height + 1);
    uint64_t leader_share = reward * 15 / 100 + total_tips * 15 / 100;
    /* When building, the committee isn't yet known (signers added later).
     * The leader credits itself the leader share and SIMULATES the committee
     * share staying in the reward pool until ac_chain_accept_block runs after
     * commit gathering. That means the state_root the leader broadcasts is
     * NOT the post-commit one — instead we treat the block's state_root as
     * post-tx-apply-with-only-leader-share-credited; the committee share is
     * applied during accept_block by the same code path on every node.
     *
     * To keep accept_block deterministic, we anchor state_root to a fully-
     * defined point: AFTER applying txs AND crediting the leader share AND
     * crediting the committee share. Since the committee is known by the
     * time accept_block runs (it's in the block), this is consistent across
     * every node. The leader simulates the committee here using its OWN
     * intended signers; if the actual signers differ at commit time, the
     * leader rebuilds and re-broadcasts. */
    (void)leader_share;
    /* Simpler: credit the *entire* reward + tips to leader for state_root.
     * Accept_block will redistribute, so we must mirror that on every node. */
    ac_state_credit(c->state, &proposer, reward + total_tips);
    ac_state_debit (c->state, &SYS_REWARD_POOL, reward);

    ac_hash_t state_root;
    ac_state_root(c->state, &state_root);

    /* Restore state (the apply happens authoritatively during accept_block). */
    ac_state_deserialize(c->state, snap, snap_len);
    free(snap);

    /* tx_root. */
    ac_hash_t tx_root;
    ac_block_tx_root(&tx_root, kept, kept_n);

    /* Header. */
    out_block->header.version       = AC_BLOCK_VERSION;
    out_block->header.height        = c->height + 1;
    out_block->header.slot          = slot;
    out_block->header.parent_hash   = c->tip_hash;
    out_block->header.timestamp_ms  = ac_chain_slot_time_ms(c, slot);
    out_block->header.proposer      = proposer;
    memcpy(out_block->header.proposer_vrf_proof, proof.b, AC_VRF_PROOF_SIZE);
    out_block->header.state_root    = state_root;
    out_block->header.tx_root       = tx_root;
    out_block->header.base_fee      = base_fee;
    out_block->header.gas_used      = total_gas;
    out_block->header.gas_limit     = AC_BLOCK_GAS_LIMIT;
    out_block->header.tx_count      = kept_n;

    out_block->txs = kept;
    out_block->tx_count = kept_n;
    out_block->signers = NULL;
    out_block->nsigners = 0;

    return 0;
}
