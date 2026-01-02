#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <string.h>
#include "txpool.h"
#include "types.h"
#include "crypto.h"


#define USER_PRIVATE_KEY { 0x6a, 0x9f, 0x83, 0x1d, 0x2c, 0xe7, 0x54, 0x99, 0x48, 0x3b, 0xa1, 0xf0, 0x92, 0x61, 0x77, 0x3e, 0x54, 0x0f, 0x9b, 0x12, 0xd3, 0x5c, 0x89, 0x01, 0xa4, 0xe2, 0x6d, 0x7f, 0x18, 0x93, 0xbc, 0x55, 0x3c, 0x71, 0x1e, 0x0f, 0x5d, 0x6a, 0x2a, 0xb7, 0x1d, 0xa6, 0x98, 0x9f, 0x2e, 0x4f, 0x88, 0x93, 0x6b, 0xa2, 0x7c, 0x44, 0x18, 0x9d, 0xe3, 0x56, 0x8f, 0x71, 0x2b, 0x5c, 0x97, 0x41, 0x2e, 0x9a }
#define USER_PUB_KEY { 0x3c, 0x71, 0x1e, 0x0f, 0x5d, 0x6a, 0x2a, 0xb7, 0x1d, 0xa6, 0x98, 0x9f, 0x2e, 0x4f, 0x88, 0x93, 0x6b, 0xa2, 0x7c, 0x44, 0x18, 0x9d, 0xe3, 0x56, 0x8f, 0x71, 0x2b, 0x5c, 0x97, 0x41, 0x2e, 0x9a }

account USER = {
    .pub_key = USER_PUB_KEY,
    .priv_key = USER_PRIVATE_KEY,
    .balance = 0
};

long long get_account_balance(account* acc) {
    if (!acc) return -1;
    return 1000; // Example balance
}

int create_tx (tx* transaction, uint8_t function_id, uint32_t accounts_num, account_list_node* accounts,  tx_data* data) {
    transaction = malloc(sizeof(*transaction));
    if (!transaction) return -1;
    transaction->expire = (uint64_t)time(NULL) + 3600; // expires in 1 hour
    transaction->signer = accounts->acc;
    transaction->function_id = function_id;
    transaction->accounts_num = accounts_num;
    transaction->accounts = accounts;
    transaction->data = data;
    transaction->confirmed = 0;
    return 0;
}

int create_transfer_tx (tx* transaction, pub_key_t sender, pub_key_t receiver, uint64_t amount) {
    //create accounts
    long long balance = 0;
    account* sender_acc = malloc(sizeof(account));
    if (!sender_acc) return -1;
    memcpy(sender_acc->pub_key, sender, crypto_sign_PUBLICKEYBYTES);

    account* receiver_acc = malloc(sizeof(account));
    if (!receiver_acc) return -2;
    memcpy(receiver_acc->pub_key, receiver, crypto_sign_PUBLICKEYBYTES);

    // Create accounts list
    account_list_node* accounts = malloc(2 * sizeof(account_list_node));
    if (!accounts) {
        free(sender_acc);
        free(receiver_acc);
        return -3;
    }

    accounts[0].acc = sender_acc;
    accounts[0].next = &accounts[1];
    accounts[1].acc = receiver_acc;
    accounts[1].next = NULL;

    // Create tx_data
    tx_data* data = malloc(sizeof(tx_data));
    if (!data) {
        free(sender_acc);
        free(receiver_acc);
        free(accounts);
        return -4;
    }
    data->data_len = sizeof(uint64_t);
    memcpy(data->data, &amount, sizeof(uint64_t));

    // Create transaction
    if (create_tx(transaction, 1, 2, accounts, data) < 0) {
        free(sender_acc);
        free(receiver_acc);
        free(accounts);
        free(data);
        return -5;
    }
    return 0;
}

int create_mint_tx (tx* transaction, pub_key_t receiver, uint64_t amount) {
    //create account
    account* receiver_acc = malloc(sizeof(account));
    if (!receiver_acc) return -1;
    memcpy(receiver_acc->pub_key, receiver, crypto_sign_PUBLICKEYBYTES);

    // Create accounts list
    account_list_node* accounts = malloc(sizeof(account_list_node));
    if (!accounts) {
        free(receiver_acc);
        return -2;
    }

    accounts->acc = receiver_acc;
    accounts->next = NULL;

    // Create tx_data
    tx_data* data = malloc(sizeof(tx_data));
    if (!data) {
        free(receiver_acc);
        free(accounts);
        return -3;
    }
    data->data_len = sizeof(uint64_t);
    memcpy(data->data, &amount, sizeof(uint64_t));

    // Create transaction
    if (create_tx(transaction, 2, 1, accounts, data) < 0) {
        free(receiver_acc);
        free(accounts);
        free(data);
        return -4;
    }
    return 0;
}

int encode_tx (tx* transaction, uint8_t encoded_tx[], size_t* raw_tx_len) {
    if (!transaction || !encoded_tx || !raw_tx_len) return -1;
    // Placeholder for encoding logic
    *raw_tx_len = 0;
    return 0;
}
int decode_tx (tx* transaction, uint8_t encoded_tx[], size_t raw_tx_len) {
    if (!transaction || !encoded_tx || raw_tx_len == 0) return -1;
    // Placeholder for decoding logic
    return 0;
}

int share_tx_with_peer (tx* transaction, pub_key_t peer_pub_key) {
    if (!transaction) return -1;
    //qua bisogna chiamare encode_tx per ottenere encoded_tx e raw_tx_len
    // Placeholder for sharing logic
    return 0;
}

int send_tx (tx* transaction) {
    if (!transaction) return -1;
    if (verify_tx(transaction) < 0) return -2;
    tx_pool_push(transaction);
    share_tx_with_peer(transaction, NULL);
    return 0;
}

int handle_incoming_tx(uint8_t encoded_tx[], size_t raw_tx_len) {
    tx* t = malloc(sizeof(tx));
    if (!t) return -1;
    if (decode_tx(t, encoded_tx, raw_tx_len) < 0) {
        free(t);
        return -2;
    }
    if (verify_tx(t) < 0) return -3;
    tx_pool_push(t);
    return 0;
}

int main () {
    if (sodium_init() < 0) exit(1);

}