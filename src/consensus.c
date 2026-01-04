#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#include "consensus.h"
#include "crypto.h"
#include "network.h"
#include "txpool.h"
#include "utils.h"
#include "log.h"

#define CONSENSUS_BLOCK_VERSION 1
#define CONSENSUS_VOTE_VERSION 1

#define CONSENSUS_SLOT_SECS 5
#define CONSENSUS_MAX_TX 64
#define CONSENSUS_MAX_BLOCK_BYTES 2000
#define CONSENSUS_MAX_VOTES 256
#define CONSENSUS_PROB_DENOM 1000000ULL
#define CONSENSUS_MAX_FUTURE_SLOTS 1
#define CONSENSUS_MAX_PAST_SLOTS 8

static pthread_mutex_t CONS_MTX = PTHREAD_MUTEX_INITIALIZER;
static blockchain* CONS_CHAIN = NULL;
static account CONS_VALIDATOR;
static int HAS_VALIDATOR = 0;
static pending_block* PENDING = NULL;
static pthread_t CONS_THREAD;
static volatile int CONS_RUNNING = 0;
static uint64_t LAST_PRODUCED_SLOT = 0;

static void votes_free(vote_entry* v) {
    while (v) {
        vote_entry* next = v->next;
        v->next = NULL;
        free(v);
        v = next;
    }
}

static void block_destroy(block* b);

static void pending_free(pending_block* p) {
    if (!p) return;
    votes_free(p->votes);
    p->votes = NULL;
    p->blk = NULL;
    free(p);
}

static void pending_free_with_block(pending_block* p) {
    if (!p) return;
    if (p->blk) {
        block_destroy(p->blk);
        p->blk = NULL;
    }
    pending_free(p);
}

static void pending_prune_locked(uint64_t current_slot) {
    pending_block* prev = NULL;
    pending_block* cur = PENDING;
    while (cur) {
        if (cur->slot + CONSENSUS_MAX_PAST_SLOTS < current_slot) {
            pending_block* drop = cur;
            if (prev) {
                prev->next = cur->next;
            } else {
                PENDING = cur->next;
            }
            cur = cur->next;
            pending_free_with_block(drop);
            continue;
        }
        prev = cur;
        cur = cur->next;
    }
}

static pending_block* pending_find_locked(uint64_t id) {
    pending_block* cur = PENDING;
    while (cur) {
        if (cur->id == id) return cur;
        cur = cur->next;
    }
    return NULL;
}

static pending_block* pending_find_slot_locked(uint64_t slot) {
    pending_block* cur = PENDING;
    while (cur) {
        if (cur->slot == slot) return cur;
        cur = cur->next;
    }
    return NULL;
}

static void pending_remove_locked(uint64_t id) {
    pending_block* prev = NULL;
    pending_block* cur = PENDING;
    while (cur) {
        if (cur->id == id) {
            if (prev) prev->next = cur->next;
            else PENDING = cur->next;
            pending_free(cur);
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

static int vote_add_locked(pending_block* pb, const pub_key_t voter) {
    if (!pb || !voter) return -1;
    if (pb->vote_count >= CONSENSUS_MAX_VOTES) return 0;
    vote_entry* cur = pb->votes;
    while (cur) {
        if (memcmp(cur->voter, voter, crypto_sign_PUBLICKEYBYTES) == 0) {
            return 0;
        }
        cur = cur->next;
    }

    vote_entry* v = calloc(1, sizeof(*v));
    if (!v) return -2;
    memcpy(v->voter, voter, crypto_sign_PUBLICKEYBYTES);
    v->next = pb->votes;
    pb->votes = v;
    pb->vote_count++;
    return 1;
}

static size_t consensus_required_votes(void) {
    size_t peers = network_peer_count_online();
    size_t total = peers + 1;
    size_t needed = (total / 2) + 1;
    if (needed < 1) needed = 1;
    return needed;
}

static void tx_free_account_nodes(account_list_node* head) {
    while (head) {
        account_list_node* next = head->next;
        head->acc = NULL;
        head->next = NULL;
        free(head);
        head = next;
    }
}

static void tx_destroy_pool_owned(tx* t) {
    if (!t) return;
    if (t->data) {
        free(t->data);
        t->data = NULL;
    }
    if (t->accounts) {
        tx_free_account_nodes(t->accounts);
        t->accounts = NULL;
    }
    t->signer = NULL;
    free(t);
}

static void tx_list_destroy(tx_list_node* head) {
    while (head) {
        tx_list_node* next = head->next;
        tx* t = head->transaction;
        head->transaction = NULL;
        head->next = NULL;
        free(head);
        tx_destroy_pool_owned(t);
        head = next;
    }
}

static void block_destroy(block* b) {
    if (!b) return;
    tx_list_destroy(b->txs);
    b->txs = NULL;
    b->prev_block = NULL;
    free(b);
}

static void tx_pool_remove_expired_locked(uint64_t now) {
    while (TX_POOL.txs) {
        tx_list_node* node = TX_POOL.txs;
        tx* t = node->transaction;
        if (!t) {
            TX_POOL.txs = node->next;
            node->next = NULL;
            free(node);
            TX_POOL.tx_num--;
            continue;
        }
        if (t->expire > now) break;
        TX_POOL.txs = node->next;
        node->next = NULL;
        node->transaction = NULL;
        free(node);
        TX_POOL.tx_num--;
        tx_destroy_pool_owned(t);
    }
}

static tx_list_node* tx_pool_detach_for_block(uint32_t max_txs, uint32_t* out_num) {
    if (out_num) *out_num = 0;
    if (max_txs == 0) return NULL;

    uint64_t now = network_time_now();
    pthread_mutex_lock(&TX_POOL.mtx);
    tx_pool_remove_expired_locked(now);

    tx_list_node* head = NULL;
    tx_list_node* tail = NULL;
    uint32_t count = 0;
    size_t block_size = 1 + 8 + 8 + 8 + crypto_sign_PUBLICKEYBYTES + 4 + crypto_sign_BYTES;

    while (TX_POOL.txs && count < max_txs) {
        tx_list_node* node = TX_POOL.txs;
        tx* t = node->transaction;

        if (!t || t->expire <= now || verify_tx(t) < 0) {
            TX_POOL.txs = node->next;
            node->next = NULL;
            node->transaction = NULL;
            free(node);
            TX_POOL.tx_num--;
            tx_destroy_pool_owned(t);
            continue;
        }

        size_t tx_len = 0;
        if (encode_tx(t, NULL, &tx_len) < 0) {
            TX_POOL.txs = node->next;
            node->next = NULL;
            node->transaction = NULL;
            free(node);
            TX_POOL.tx_num--;
            tx_destroy_pool_owned(t);
            continue;
        }

        if (block_size + 4 + tx_len > CONSENSUS_MAX_BLOCK_BYTES) {
            break;
        }

        TX_POOL.txs = node->next;
        node->next = NULL;
        TX_POOL.tx_num--;

        if (!head) {
            head = node;
        } else {
            tail->next = node;
        }
        tail = node;
        count++;
        block_size += 4 + tx_len;
    }

    pthread_mutex_unlock(&TX_POOL.mtx);
    if (out_num) *out_num = count;
    return head;
}

static void tx_pool_remove_by_sig(const signature_t sig) {
    pthread_mutex_lock(&TX_POOL.mtx);
    tx_list_node* prev = NULL;
    tx_list_node* cur = TX_POOL.txs;
    while (cur) {
        tx* t = cur->transaction;
        if (t && memcmp(t->signature, sig, crypto_sign_BYTES) == 0) {
            tx_list_node* victim = cur;
            if (prev) {
                prev->next = cur->next;
            } else {
                TX_POOL.txs = cur->next;
            }
            cur = cur->next;
            victim->next = NULL;
            victim->transaction = NULL;
            free(victim);
            TX_POOL.tx_num--;
            tx_destroy_pool_owned(t);
            continue;
        }
        prev = cur;
        cur = cur->next;
    }
    pthread_mutex_unlock(&TX_POOL.mtx);
}

static int tx_wire_hash(const tx* t, uint8_t out[32]) {
    if (!t || !out) return -1;
    size_t len = 0;
    if (encode_tx((tx*)t, NULL, &len) < 0) return -2;
    uint8_t* buf = malloc(len);
    if (!buf) return -3;
    size_t cap = len;
    if (encode_tx((tx*)t, buf, &cap) < 0) {
        free(buf);
        return -4;
    }
    crypto_generichash(out, 32, buf, cap, NULL, 0);
    free(buf);
    return 0;
}

static int block_hash(const block* b, uint8_t out[32]) {
    if (!b || !out) return -1;
    crypto_generichash_state st;
    if (crypto_generichash_init(&st, NULL, 0, 32) != 0) return -2;

    uint8_t b8[8];
    store_u64_le(b8, b->chain_id);
    crypto_generichash_update(&st, b8, sizeof b8);
    store_u64_le(b8, b->slot);
    crypto_generichash_update(&st, b8, sizeof b8);
    store_u64_le(b8, b->prev_id);
    crypto_generichash_update(&st, b8, sizeof b8);

    crypto_generichash_update(&st, b->proposer, crypto_sign_PUBLICKEYBYTES);

    tx_list_node* cur = b->txs;
    while (cur) {
        uint8_t h[32];
        if (!cur->transaction || tx_wire_hash(cur->transaction, h) < 0) {
            sodium_memzero(&st, sizeof st);
            return -3;
        }
        crypto_generichash_update(&st, h, sizeof h);
        cur = cur->next;
    }

    crypto_generichash_final(&st, out, 32);
    sodium_memzero(&st, sizeof st);
    return 0;
}

static uint64_t block_id_from_hash(const uint8_t hash[32]) {
    uint64_t id = load_u64_le(hash);
    return (id == 0) ? 1 : id;
}

static int validator_eligible(const pub_key_t pub_key, uint64_t balance,
                              uint64_t slot, uint64_t prev_id, uint64_t chain_id) {
    if (!pub_key || balance == 0) return 0;
    uint8_t buf[8 + 8 + 8 + crypto_sign_PUBLICKEYBYTES];
    size_t off = 0;
    store_u64_le(&buf[off], chain_id);
    off += 8;
    store_u64_le(&buf[off], prev_id);
    off += 8;
    store_u64_le(&buf[off], slot);
    off += 8;
    memcpy(&buf[off], pub_key, crypto_sign_PUBLICKEYBYTES);
    off += crypto_sign_PUBLICKEYBYTES;

    uint8_t hash[32];
    crypto_generichash(hash, sizeof(hash), buf, off, NULL, 0);
    uint64_t h = load_u64_le(hash);

    uint64_t scale = UINT64_MAX / CONSENSUS_PROB_DENOM;
    if (scale == 0) scale = 1;
    uint64_t threshold = (balance >= CONSENSUS_PROB_DENOM) ? UINT64_MAX : (balance * scale);
    return h <= threshold;
}

static int is_bootstrap_genesis_locked(blockchain* bc, const pub_key_t pub_key) {
    if (!bc || !pub_key) return 0;
    if (bc->tip != NULL) return 0;
    return memcmp(pub_key, bc->genesis_pub, crypto_sign_PUBLICKEYBYTES) == 0;
}

static uint64_t current_slot(void) {
    return network_time_now() / CONSENSUS_SLOT_SECS;
}

static int block_encode(const block* b, uint8_t* out, size_t* out_len) {
    if (!b || !out_len) return -1;

    size_t total = 1 + 8 + 8 + 8 + crypto_sign_PUBLICKEYBYTES + 4;
    uint32_t count = 0;
    tx_list_node* cur = b->txs;
    while (cur) {
        size_t tx_len = 0;
        if (!cur->transaction || encode_tx(cur->transaction, NULL, &tx_len) < 0) return -2;
        total += 4 + tx_len;
        count++;
        cur = cur->next;
    }
    total += crypto_sign_BYTES;

    if (!out) {
        *out_len = total;
        return 0;
    }
    if (*out_len < total) {
        *out_len = total;
        return -3;
    }

    size_t off = 0;
    out[off++] = CONSENSUS_BLOCK_VERSION;
    store_u64_le(&out[off], b->chain_id);
    off += 8;
    store_u64_le(&out[off], b->slot);
    off += 8;
    store_u64_le(&out[off], b->prev_id);
    off += 8;
    memcpy(&out[off], b->proposer, crypto_sign_PUBLICKEYBYTES);
    off += crypto_sign_PUBLICKEYBYTES;
    store_u32_le(&out[off], count);
    off += 4;

    cur = b->txs;
    while (cur) {
        size_t tx_len = 0;
        encode_tx(cur->transaction, NULL, &tx_len);
        store_u32_le(&out[off], (uint32_t)tx_len);
        off += 4;
        size_t cap = tx_len;
        if (encode_tx(cur->transaction, &out[off], &cap) < 0) return -4;
        off += tx_len;
        cur = cur->next;
    }

    memcpy(&out[off], b->signature, crypto_sign_BYTES);
    off += crypto_sign_BYTES;

    *out_len = off;
    return 0;
}

static int block_decode(const uint8_t* data, size_t len, block** out_block) {
    if (!data || !out_block || len < 1 + crypto_sign_BYTES) return -1;
    size_t off = 0;
    if (data[off++] != CONSENSUS_BLOCK_VERSION) return -2;

    if (off + 8 + 8 + 8 + crypto_sign_PUBLICKEYBYTES + 4 > len) return -3;
    block* b = calloc(1, sizeof(*b));
    if (!b) return -4;

    b->chain_id = load_u64_le(&data[off]);
    off += 8;
    b->slot = load_u64_le(&data[off]);
    off += 8;
    b->prev_id = load_u64_le(&data[off]);
    off += 8;
    memcpy(b->proposer, &data[off], crypto_sign_PUBLICKEYBYTES);
    off += crypto_sign_PUBLICKEYBYTES;
    b->tx_num = load_u32_le(&data[off]);
    off += 4;

    tx_list_node* head = NULL;
    tx_list_node* tail = NULL;

    for (uint32_t i = 0; i < b->tx_num; i++) {
        if (off + 4 > len) {
            tx_list_destroy(head);
            free(b);
            return -5;
        }
        uint32_t tx_len = load_u32_le(&data[off]);
        off += 4;
        if (tx_len == 0 || off + tx_len > len) {
            tx_list_destroy(head);
            free(b);
            return -6;
        }
        tx* t = calloc(1, sizeof(*t));
        if (!t) {
            tx_list_destroy(head);
            free(b);
            return -7;
        }
        if (decode_tx(t, (uint8_t*)&data[off], tx_len) < 0) {
            free(t);
            tx_list_destroy(head);
            free(b);
            return -8;
        }
        off += tx_len;

        tx_list_node* node = calloc(1, sizeof(*node));
        if (!node) {
            free(t);
            tx_list_destroy(head);
            free(b);
            return -9;
        }
        node->transaction = t;
        if (!head) head = node;
        else tail->next = node;
        tail = node;
    }

    if (off + crypto_sign_BYTES > len) {
        tx_list_destroy(head);
        free(b);
        return -10;
    }
    memcpy(b->signature, &data[off], crypto_sign_BYTES);
    off += crypto_sign_BYTES;

    if (off != len) {
        tx_list_destroy(head);
        free(b);
        return -11;
    }

    b->txs = head;
    *out_block = b;
    return 0;
}

static int block_sign(block* b, const account* validator) {
    if (!b || !validator) return -1;
    uint8_t hash[32];
    if (block_hash(b, hash) < 0) return -2;
    if (crypto_sign_detached(b->signature, NULL, hash, sizeof hash, validator->priv_key) != 0) {
        sodium_memzero(hash, sizeof hash);
        return -3;
    }
    b->id = block_id_from_hash(hash);
    sodium_memzero(hash, sizeof hash);
    return 0;
}

static int block_verify(const block* b) {
    if (!b) return -1;
    uint8_t hash[32];
    if (block_hash(b, hash) < 0) return -2;
    if (crypto_sign_verify_detached(b->signature, hash, sizeof hash, b->proposer) != 0) {
        sodium_memzero(hash, sizeof hash);
        return -3;
    }
    uint64_t id = block_id_from_hash(hash);
    sodium_memzero(hash, sizeof hash);
    return (id == b->id) ? 0 : -4;
}

static uint64_t bc_get_balance_locked(blockchain* bc, const pub_key_t key) {
    account_state_node* cur = bc->accounts;
    while (cur) {
        if (memcmp(cur->acc.pub_key, key, crypto_sign_PUBLICKEYBYTES) == 0) {
            return cur->acc.balance;
        }
        cur = cur->next;
    }
    return 0;
}

static account* bc_get_or_create_locked(blockchain* bc, const pub_key_t key) {
    account_state_node* cur = bc->accounts;
    while (cur) {
        if (memcmp(cur->acc.pub_key, key, crypto_sign_PUBLICKEYBYTES) == 0) {
            return &cur->acc;
        }
        cur = cur->next;
    }
    account_state_node* node = calloc(1, sizeof(*node));
    if (!node) return NULL;
    memcpy(node->acc.pub_key, key, crypto_sign_PUBLICKEYBYTES);
    node->acc.balance = 0;
    node->next = bc->accounts;
    bc->accounts = node;
    return &node->acc;
}

static uint64_t temp_get_balance(temp_balance** head, blockchain* bc, const pub_key_t key) {
    temp_balance* cur = *head;
    while (cur) {
        if (memcmp(cur->pub, key, crypto_sign_PUBLICKEYBYTES) == 0) return cur->balance;
        cur = cur->next;
    }
    temp_balance* node = calloc(1, sizeof(*node));
    if (!node) return 0;
    memcpy(node->pub, key, crypto_sign_PUBLICKEYBYTES);
    node->balance = bc_get_balance_locked(bc, key);
    node->next = *head;
    *head = node;
    return node->balance;
}

static int temp_set_balance(temp_balance** head, const pub_key_t key, uint64_t balance) {
    temp_balance* cur = *head;
    while (cur) {
        if (memcmp(cur->pub, key, crypto_sign_PUBLICKEYBYTES) == 0) {
            cur->balance = balance;
            return 0;
        }
        cur = cur->next;
    }
    temp_balance* node = calloc(1, sizeof(*node));
    if (!node) return -1;
    memcpy(node->pub, key, crypto_sign_PUBLICKEYBYTES);
    node->balance = balance;
    node->next = *head;
    *head = node;
    return 0;
}

static void temp_balance_free(temp_balance* head) {
    while (head) {
        temp_balance* next = head->next;
        free(head);
        head = next;
    }
}

static int validate_block_txs(blockchain* bc, const block* b) {
    if (!bc || !b) return -1;
    uint64_t now = network_time_now();

    pthread_mutex_lock(&bc->mtx);
    temp_balance* temp = NULL;
    tx_list_node* cur = b->txs;
    while (cur) {
        tx* t = cur->transaction;
        if (!t || t->expire <= now || verify_tx(t) < 0) {
            temp_balance_free(temp);
            pthread_mutex_unlock(&bc->mtx);
            return -2;
        }

        if (t->function_id == 1) {
            if (!t->accounts || !t->accounts->next || !t->data || t->data->data_len < sizeof(uint64_t)) {
                temp_balance_free(temp);
                pthread_mutex_unlock(&bc->mtx);
                return -3;
            }
            uint64_t amount = 0;
            memcpy(&amount, t->data->data, sizeof(uint64_t));

            account* sender = t->accounts->acc;
            account* receiver = t->accounts->next->acc;
            if (!sender || !receiver) {
                temp_balance_free(temp);
                pthread_mutex_unlock(&bc->mtx);
                return -4;
            }
            uint64_t sender_bal = temp_get_balance(&temp, bc, sender->pub_key);
            if (sender_bal < amount) {
                temp_balance_free(temp);
                pthread_mutex_unlock(&bc->mtx);
                return -5;
            }
            temp_set_balance(&temp, sender->pub_key, sender_bal - amount);
            uint64_t recv_bal = temp_get_balance(&temp, bc, receiver->pub_key);
            temp_set_balance(&temp, receiver->pub_key, recv_bal + amount);
        } else if (t->function_id == 2) {
            if (!t->signer ||
                memcmp(t->signer->pub_key, bc->genesis_pub, crypto_sign_PUBLICKEYBYTES) != 0) {
                temp_balance_free(temp);
                pthread_mutex_unlock(&bc->mtx);
                return -6;
            }
            if (!t->accounts || !t->accounts->next || !t->data ||
                t->data->data_len < sizeof(uint64_t)) {
                temp_balance_free(temp);
                pthread_mutex_unlock(&bc->mtx);
                return -7;
            }
            uint64_t amount = 0;
            memcpy(&amount, t->data->data, sizeof(uint64_t));
            account* receiver = t->accounts->next->acc;
            if (!receiver) {
                temp_balance_free(temp);
                pthread_mutex_unlock(&bc->mtx);
                return -8;
            }
            uint64_t recv_bal = temp_get_balance(&temp, bc, receiver->pub_key);
            temp_set_balance(&temp, receiver->pub_key, recv_bal + amount);
        }
        cur = cur->next;
    }
    temp_balance_free(temp);
    pthread_mutex_unlock(&bc->mtx);
    return 0;
}

static int apply_block(blockchain* bc, block* b) {
    if (!bc || !b) return -1;

    pthread_mutex_lock(&bc->mtx);
    b->prev_block = bc->tip;
    bc->tip = b;
    bc->height++;

    tx_list_node* cur = b->txs;
    while (cur) {
        tx* t = cur->transaction;
        if (!t) {
            cur = cur->next;
            continue;
        }

        if (t->function_id == 1) {
            if (t->accounts && t->accounts->next && t->data && t->data->data_len >= sizeof(uint64_t)) {
                uint64_t amount = 0;
                memcpy(&amount, t->data->data, sizeof(uint64_t));
                account* sender = bc_get_or_create_locked(bc, t->accounts->acc->pub_key);
                account* receiver = bc_get_or_create_locked(bc, t->accounts->next->acc->pub_key);
                if (sender && receiver && sender->balance >= amount) {
                    sender->balance -= amount;
                    receiver->balance += amount;
                }
            }
        } else if (t->function_id == 2) {
            if (t->signer &&
                memcmp(t->signer->pub_key, bc->genesis_pub, crypto_sign_PUBLICKEYBYTES) == 0 &&
                t->accounts && t->accounts->next && t->data &&
                t->data->data_len >= sizeof(uint64_t)) {
                uint64_t amount = 0;
                memcpy(&amount, t->data->data, sizeof(uint64_t));
                account* receiver = bc_get_or_create_locked(bc, t->accounts->next->acc->pub_key);
                if (receiver) receiver->balance += amount;
            }
        }
        t->confirmed = 1;
        cur = cur->next;
    }

    pthread_mutex_unlock(&bc->mtx);
    return 0;
}

static int consensus_add_pending(block* b) {
    if (!b) return -1;
    pending_block* pb = calloc(1, sizeof(*pb));
    if (!pb) return -2;
    pb->blk = b;
    pb->id = b->id;
    pb->slot = b->slot;
    memcpy(pb->proposer, b->proposer, crypto_sign_PUBLICKEYBYTES);
    pb->next = PENDING;
    PENDING = pb;
    return 0;
}

static int consensus_broadcast_block(const block* b) {
    size_t len = 0;
    if (block_encode(b, NULL, &len) < 0) return -1;
    uint8_t* buf = malloc(len);
    if (!buf) return -2;
    size_t cap = len;
    if (block_encode(b, buf, &cap) < 0) {
        free(buf);
        return -3;
    }
    int rc = network_broadcast_block(buf, cap);
    free(buf);
    return rc;
}

static int vote_encode(uint64_t chain_id, uint64_t block_id, uint64_t slot,
                       const pub_key_t proposer, const account* voter,
                       uint8_t* out, size_t* out_len) {
    if (!out_len || !voter) return -1;
    size_t total = 1 + 8 + 8 + 8 + crypto_sign_PUBLICKEYBYTES +
        crypto_sign_PUBLICKEYBYTES + crypto_sign_BYTES;
    if (!out) {
        *out_len = total;
        return 0;
    }
    if (*out_len < total) {
        *out_len = total;
        return -2;
    }

    size_t off = 0;
    out[off++] = CONSENSUS_VOTE_VERSION;
    store_u64_le(&out[off], chain_id);
    off += 8;
    store_u64_le(&out[off], block_id);
    off += 8;
    store_u64_le(&out[off], slot);
    off += 8;
    memcpy(&out[off], proposer, crypto_sign_PUBLICKEYBYTES);
    off += crypto_sign_PUBLICKEYBYTES;
    memcpy(&out[off], voter->pub_key, crypto_sign_PUBLICKEYBYTES);
    off += crypto_sign_PUBLICKEYBYTES;

    if (crypto_sign_detached(&out[off], NULL, out, off, voter->priv_key) != 0) {
        return -3;
    }
    off += crypto_sign_BYTES;
    *out_len = off;
    return 0;
}

static int consensus_broadcast_vote(const block* b) {
    if (!b || !HAS_VALIDATOR) return -1;
    size_t len = 0;
    if (vote_encode(b->chain_id, b->id, b->slot, b->proposer, &CONS_VALIDATOR, NULL, &len) < 0) {
        return -2;
    }
    uint8_t* buf = malloc(len);
    if (!buf) return -3;
    size_t cap = len;
    if (vote_encode(b->chain_id, b->id, b->slot, b->proposer, &CONS_VALIDATOR, buf, &cap) < 0) {
        free(buf);
        return -4;
    }
    int rc = network_broadcast_vote(buf, cap);
    free(buf);
    return rc;
}

static int consensus_try_commit(pending_block* pb) {
    if (!pb || !pb->blk) return -1;
    size_t needed = consensus_required_votes();
    if (pb->vote_count < needed) return 0;

    if (validate_block_txs(CONS_CHAIN, pb->blk) < 0) return -2;
    if (apply_block(CONS_CHAIN, pb->blk) < 0) return -3;

    tx_list_node* cur = pb->blk->txs;
    while (cur) {
        if (cur->transaction) {
            tx_pool_remove_by_sig(cur->transaction->signature);
        }
        cur = cur->next;
    }

    {
        uint64_t height = 0;
        pthread_mutex_lock(&CONS_CHAIN->mtx);
        height = CONS_CHAIN->height;
        pthread_mutex_unlock(&CONS_CHAIN->mtx);
        log_info("block committed id=%llu height=%llu txs=%u",
                 (unsigned long long)pb->blk->id,
                 (unsigned long long)height,
                 pb->blk->tx_num);
    }

    return 1;
}

static int consensus_accept_block(block* b) {
    if (!b || !CONS_CHAIN) return -1;

    uint64_t now_slot = current_slot();
    if (b->slot + CONSENSUS_MAX_PAST_SLOTS < now_slot) return -2;
    if (b->slot > now_slot + CONSENSUS_MAX_FUTURE_SLOTS) return -3;

    if (b->chain_id != CONS_CHAIN->chain_id || b->chain_id == 0) return -4;

    uint64_t prev_id = 0;
    pthread_mutex_lock(&CONS_CHAIN->mtx);
    if (CONS_CHAIN->tip) prev_id = CONS_CHAIN->tip->id;
    pthread_mutex_unlock(&CONS_CHAIN->mtx);

    if (b->prev_id != prev_id) return -5;

    uint64_t balance = 0;
    pthread_mutex_lock(&CONS_CHAIN->mtx);
    balance = bc_get_balance_locked(CONS_CHAIN, b->proposer);
    int bootstrap_genesis = (b->prev_id == 0) ? is_bootstrap_genesis_locked(CONS_CHAIN, b->proposer) : 0;
    pthread_mutex_unlock(&CONS_CHAIN->mtx);

    if (!bootstrap_genesis &&
        !validator_eligible(b->proposer, balance, b->slot, b->prev_id, b->chain_id)) {
        return -6;
    }

    if (block_verify(b) < 0) return -7;
    if (validate_block_txs(CONS_CHAIN, b) < 0) return -8;

    return 0;
}

static void consensus_handle_pending_block(block* b) {
    uint64_t slot = b->slot;

    pthread_mutex_lock(&CONS_MTX);
    pending_prune_locked(current_slot());

    pending_block* existing = pending_find_slot_locked(slot);
    if (existing && existing->id == b->id) {
        pthread_mutex_unlock(&CONS_MTX);
        block_destroy(b);
        return;
    }

    if (existing && b->id >= existing->id) {
        pthread_mutex_unlock(&CONS_MTX);
        block_destroy(b);
        return;
    }

    if (existing && b->id < existing->id) {
        pending_block* prev = NULL;
        pending_block* cur = PENDING;
        while (cur) {
            if (cur == existing) {
                if (prev) prev->next = cur->next;
                else PENDING = cur->next;
                pending_free_with_block(cur);
                break;
            }
            prev = cur;
            cur = cur->next;
        }
    }

    consensus_add_pending(b);
    pending_block* pb = pending_find_locked(b->id);
    if (pb && HAS_VALIDATOR) {
        vote_add_locked(pb, CONS_VALIDATOR.pub_key);
    }
    if (pb) {
        int rc = consensus_try_commit(pb);
        if (rc > 0) {
            pending_remove_locked(pb->id);
        }
    }
    pthread_mutex_unlock(&CONS_MTX);

    consensus_broadcast_vote(b);
}

static void consensus_produce_block(void) {
    if (!HAS_VALIDATOR || !CONS_CHAIN) return;

    uint64_t slot = current_slot();
    if (LAST_PRODUCED_SLOT == slot) return;

    pthread_mutex_lock(&CONS_CHAIN->mtx);
    uint64_t prev_id = CONS_CHAIN->tip ? CONS_CHAIN->tip->id : 0;
    uint64_t chain_id = CONS_CHAIN->chain_id;
    uint64_t balance = bc_get_balance_locked(CONS_CHAIN, CONS_VALIDATOR.pub_key);
    int bootstrap_genesis = (prev_id == 0) ? is_bootstrap_genesis_locked(CONS_CHAIN, CONS_VALIDATOR.pub_key) : 0;
    pthread_mutex_unlock(&CONS_CHAIN->mtx);

    if (!bootstrap_genesis &&
        !validator_eligible(CONS_VALIDATOR.pub_key, balance, slot, prev_id, chain_id)) {
        return;
    }

    pthread_mutex_lock(&CONS_MTX);
    if (pending_find_slot_locked(slot)) {
        pthread_mutex_unlock(&CONS_MTX);
        return;
    }
    pthread_mutex_unlock(&CONS_MTX);

    uint32_t tx_num = 0;
    tx_list_node* txs = tx_pool_detach_for_block(CONSENSUS_MAX_TX, &tx_num);
    if (!txs) return;

    block* b = calloc(1, sizeof(*b));
    if (!b) {
        tx_list_destroy(txs);
        return;
    }

    b->chain_id = chain_id;
    b->slot = slot;
    b->prev_id = prev_id;
    memcpy(b->proposer, CONS_VALIDATOR.pub_key, crypto_sign_PUBLICKEYBYTES);
    b->txs = txs;
    b->tx_num = tx_num;

    if (block_sign(b, &CONS_VALIDATOR) < 0) {
        block_destroy(b);
        return;
    }

    LAST_PRODUCED_SLOT = slot;
    log_info("block proposed id=%llu slot=%llu txs=%u",
             (unsigned long long)b->id,
             (unsigned long long)b->slot,
             b->tx_num);
    consensus_handle_pending_block(b);
    consensus_broadcast_block(b);
}

static void* consensus_thread(void* arg) {
    (void)arg;
    while (CONS_RUNNING) {
        consensus_produce_block();
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 200 * 1000000L;
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int consensus_init(blockchain* bc) {
    if (!bc) return -1;
    CONS_CHAIN = bc;
    CONS_RUNNING = 1;
    if (pthread_create(&CONS_THREAD, NULL, consensus_thread, NULL) != 0) {
        CONS_RUNNING = 0;
        return -2;
    }
    return 0;
}

void consensus_shutdown(void) {
    if (CONS_RUNNING) {
        CONS_RUNNING = 0;
        pthread_join(CONS_THREAD, NULL);
    }
}

int consensus_set_validator(const account* validator) {
    if (!validator) return -1;
    memcpy(&CONS_VALIDATOR, validator, sizeof(CONS_VALIDATOR));
    HAS_VALIDATOR = 1;
    return 0;
}

int consensus_handle_block(const uint8_t* data, size_t len) {
    block* b = NULL;
    if (block_decode(data, len, &b) < 0) return -1;
    uint8_t hash[32];
    if (block_hash(b, hash) < 0) {
        block_destroy(b);
        return -2;
    }
    b->id = block_id_from_hash(hash);
    sodium_memzero(hash, sizeof hash);

    if (consensus_accept_block(b) < 0) {
        block_destroy(b);
        return -3;
    }

    consensus_handle_pending_block(b);

    return 0;
}

int consensus_handle_vote(const uint8_t* data, size_t len) {
    if (!data || len < 1 + 8 + 8 + 8 + crypto_sign_PUBLICKEYBYTES * 2 + crypto_sign_BYTES) return -1;
    size_t off = 0;
    if (data[off++] != CONSENSUS_VOTE_VERSION) return -2;

    uint64_t chain_id = load_u64_le(&data[off]);
    off += 8;
    uint64_t block_id = load_u64_le(&data[off]);
    off += 8;
    uint64_t slot = load_u64_le(&data[off]);
    off += 8;
    const uint8_t* proposer = &data[off];
    off += crypto_sign_PUBLICKEYBYTES;
    const uint8_t* voter = &data[off];
    off += crypto_sign_PUBLICKEYBYTES;

    const uint8_t* sig = &data[off];
    size_t sign_len = off;
    if (off + crypto_sign_BYTES != len) return -3;
    if (crypto_sign_verify_detached(sig, data, sign_len, voter) != 0) return -4;

    if (!CONS_CHAIN || chain_id != CONS_CHAIN->chain_id) return -5;

    pthread_mutex_lock(&CONS_CHAIN->mtx);
    uint64_t voter_balance = bc_get_balance_locked(CONS_CHAIN, voter);
    pthread_mutex_unlock(&CONS_CHAIN->mtx);
    if (voter_balance == 0) return -6;

    pthread_mutex_lock(&CONS_MTX);
    pending_block* pb = pending_find_locked(block_id);
    if (!pb || pb->slot != slot ||
        memcmp(pb->proposer, proposer, crypto_sign_PUBLICKEYBYTES) != 0) {
        pthread_mutex_unlock(&CONS_MTX);
        return -7;
    }

    vote_add_locked(pb, voter);
    int rc = consensus_try_commit(pb);
    block* committed = NULL;
    if (rc > 0) {
        committed = pb->blk;
        pending_remove_locked(pb->id);
    }
    pthread_mutex_unlock(&CONS_MTX);

    if (committed) {
        consensus_broadcast_vote(committed);
    }

    return 0;
}
