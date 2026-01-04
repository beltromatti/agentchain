#pragma once
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <pthread.h>
#include <sodium.h>

#define TX_DATA_MAX_SIZE 128

typedef uint8_t pub_key_t[crypto_sign_PUBLICKEYBYTES]; // 32 bytes
typedef uint8_t priv_key_t[crypto_sign_SECRETKEYBYTES]; // 64 bytes (secret key Ed25519)
typedef uint8_t signature_t[crypto_sign_BYTES]; // 64 bytes (Ed25519 detached signature)

typedef int (*peer_send_fn)(const uint8_t* data, size_t len, void* user_ctx);

typedef struct {
    pub_key_t pub_key;
    priv_key_t priv_key; 
    uint64_t balance;
} account;

typedef struct account_list_node {
    account* acc;
    struct account_list_node* next;
} account_list_node;

typedef struct {
    uint8_t data[TX_DATA_MAX_SIZE];
    uint32_t data_len;
} tx_data;

typedef struct {
    signature_t signature;
    uint64_t expire;
    account* signer;
    uint8_t function_id;
    uint32_t accounts_num;
    account_list_node* accounts;
    tx_data* data;
    uint8_t confirmed;
} tx;

typedef struct tx_list_node {
    tx* transaction;
    struct tx_list_node* next;
} tx_list_node;

typedef struct block {
    uint64_t id;
    uint64_t slot;
    uint64_t prev_id;
    uint64_t chain_id;
    pub_key_t proposer;
    signature_t signature;
    struct block* prev_block;
    uint32_t tx_num;
    tx_list_node* txs;
} block;

typedef struct account_state_node {
    account acc;
    struct account_state_node* next;
} account_state_node;

typedef struct {
    pthread_mutex_t mtx;
    uint64_t height;
    uint64_t chain_id;
    pub_key_t genesis_pub;
    block* tip;
    account_state_node* accounts;
    uint8_t synced;
} blockchain;

typedef struct {
    pthread_mutex_t mtx;
    uint32_t tx_num;
    tx_list_node* txs;
} tx_pool;

typedef struct vote_entry {
    pub_key_t voter;
    struct vote_entry* next;
} vote_entry;

typedef struct pending_block {
    block* blk;
    uint64_t id;
    uint64_t slot;
    pub_key_t proposer;
    size_t vote_count;
    vote_entry* votes;
    struct pending_block* next;
} pending_block;

typedef struct temp_balance {
    pub_key_t pub;
    uint64_t balance;
    struct temp_balance* next;
} temp_balance;

typedef struct peer_state {
    uint64_t id;
    pub_key_t pub_key;
    int online;
    int has_time;
    int64_t time_offset;
    uint64_t last_seen;
    peer_send_fn send_cb;
    void* user_ctx;
    uint32_t ip_be;
    uint16_t port;
    struct peer_state* next;
} peer_state;

typedef struct {
    uint8_t hash[32];
    uint64_t last_seen;
} seen_tx_entry;

typedef struct {
    uint64_t id;
    peer_send_fn send_cb;
    void* user_ctx;
    uint32_t ip_be;
    uint16_t port;
} peer_send_target;
