#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "blockchain.h"

#define CHAIN_STATE_DIR "data"
#define CHAIN_STATE_PATH "data/chain.state"
#define CHAIN_STATE_MAGIC 0x4E434C42u
#define CHAIN_STATE_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t chain_id;
    pub_key_t genesis_pub;
    uint64_t created_at;
} chain_state_file;

blockchain CHAIN = {
    .mtx = PTHREAD_MUTEX_INITIALIZER,
    .height = 0,
    .chain_id = 0,
    .genesis_pub = { 0 },
    .tip = NULL,
    .accounts = NULL
};

int blockchain_init(blockchain* bc) {
    if (!bc) return -1;
    bc->height = 0;
    bc->tip = NULL;
    bc->accounts = NULL;
    bc->chain_id = 0;
    memset(bc->genesis_pub, 0, sizeof(bc->genesis_pub));
    return 0;
}

static int ensure_chain_state_dir(void) {
    if (mkdir(CHAIN_STATE_DIR, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static int load_chain_state(chain_state_file* out_state) {
    if (!out_state) return -1;

    int fd = open(CHAIN_STATE_PATH, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) return -2;
        return -5;
    }

    ssize_t n = read(fd, out_state, sizeof(*out_state));
    close(fd);
    if (n != (ssize_t)sizeof(*out_state)) return -3;

    if (out_state->magic != CHAIN_STATE_MAGIC ||
        out_state->version != CHAIN_STATE_VERSION) {
        return -4;
    }

    return 0;
}

static int write_chain_state(const chain_state_file* state) {
    if (!state) return -1;
    if (ensure_chain_state_dir() < 0) return -2;

    int fd = open(CHAIN_STATE_PATH, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -3;

    ssize_t n = write(fd, state, sizeof(*state));
    if (n != (ssize_t)sizeof(*state)) {
        close(fd);
        return -4;
    }
    fsync(fd);
    close(fd);
    return 0;
}

int blockchain_bootstrap(blockchain* bc, const account* genesis) {
    if (!bc || !genesis) return -1;

    int created = 0;
    chain_state_file state;
    int rc = load_chain_state(&state);
    if (rc == 0) {
        bc->chain_id = state.chain_id;
        memcpy(bc->genesis_pub, state.genesis_pub, crypto_sign_PUBLICKEYBYTES);
        rc = blockchain_register_account(bc, genesis);
        if (rc < 0) return rc;
        return 0;
    }

    if (rc != -2) {
        return -3;
    }

    memset(&state, 0, sizeof(state));
    state.magic = CHAIN_STATE_MAGIC;
    state.version = CHAIN_STATE_VERSION;
    randombytes_buf(&state.chain_id, sizeof(state.chain_id));
    if (state.chain_id == 0) state.chain_id = 1;
    memcpy(state.genesis_pub, genesis->pub_key, crypto_sign_PUBLICKEYBYTES);
    state.created_at = (uint64_t)time(NULL);

    if (write_chain_state(&state) < 0) {
        if (load_chain_state(&state) == 0) {
            bc->chain_id = state.chain_id;
            memcpy(bc->genesis_pub, state.genesis_pub, crypto_sign_PUBLICKEYBYTES);
            rc = blockchain_register_account(bc, genesis);
            if (rc < 0) return rc;
            return 0;
        }
        return -4;
    }

    bc->chain_id = state.chain_id;
    memcpy(bc->genesis_pub, state.genesis_pub, crypto_sign_PUBLICKEYBYTES);
    created = 1;
    rc = blockchain_register_account(bc, genesis);
    if (rc < 0) return rc;
    return created;
}

account* blockchain_get_account(blockchain* bc, const pub_key_t key) {
    if (!bc || !key) return NULL;

    pthread_mutex_lock(&bc->mtx);
    account_state_node* cur = bc->accounts;
    while (cur) {
        if (memcmp(cur->acc.pub_key, key, crypto_sign_PUBLICKEYBYTES) == 0) {
            pthread_mutex_unlock(&bc->mtx);
            return &cur->acc;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&bc->mtx);
    return NULL;
}

account* blockchain_get_or_create_account(blockchain* bc, const pub_key_t key) {
    if (!bc || !key) return NULL;

    pthread_mutex_lock(&bc->mtx);
    account_state_node* cur = bc->accounts;
    while (cur) {
        if (memcmp(cur->acc.pub_key, key, crypto_sign_PUBLICKEYBYTES) == 0) {
            pthread_mutex_unlock(&bc->mtx);
            return &cur->acc;
        }
        cur = cur->next;
    }

    account_state_node* node = calloc(1, sizeof(*node));
    if (!node) {
        pthread_mutex_unlock(&bc->mtx);
        return NULL;
    }
    memcpy(node->acc.pub_key, key, crypto_sign_PUBLICKEYBYTES);
    node->acc.balance = 0;
    node->next = bc->accounts;
    bc->accounts = node;
    pthread_mutex_unlock(&bc->mtx);
    return &node->acc;
}

int blockchain_register_account(blockchain* bc, const account* acc) {
    if (!bc || !acc) return -1;

    pthread_mutex_lock(&bc->mtx);
    account_state_node* cur = bc->accounts;
    while (cur) {
        if (memcmp(cur->acc.pub_key, acc->pub_key, crypto_sign_PUBLICKEYBYTES) == 0) {
            cur->acc = *acc;
            pthread_mutex_unlock(&bc->mtx);
            return 0;
        }
        cur = cur->next;
    }

    account_state_node* node = calloc(1, sizeof(*node));
    if (!node) {
        pthread_mutex_unlock(&bc->mtx);
        return -2;
    }
    node->acc = *acc;
    node->next = bc->accounts;
    bc->accounts = node;
    pthread_mutex_unlock(&bc->mtx);
    return 0;
}

long long get_account_balance(account* acc) {
    if (!acc) return -1;
    account* stored = blockchain_get_account(&CHAIN, acc->pub_key);
    if (!stored) return 0;
    return (long long)stored->balance;
}
