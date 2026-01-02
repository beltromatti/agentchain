#pragma once
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <sodium.h>

#define TX_DATA_MAX_SIZE 128

typedef uint8_t pub_key_t[crypto_sign_PUBLICKEYBYTES]; // 32 bytes
typedef uint8_t priv_key_t[crypto_sign_SECRETKEYBYTES]; // 64 bytes (secret key Ed25519)
typedef uint8_t signature_t[crypto_sign_BYTES]; // 64 bytes (Ed25519 detached signature)

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

typedef struct {
    uint64_t id;
    signature_t signature;
    struct block* prev_block;
    uint32_t tx_num;
    tx_list_node* txs;
} block;

typedef struct {
    pthread_mutex_t mtx;
    uint32_t tx_num;
    tx_list_node* txs;
} tx_pool;