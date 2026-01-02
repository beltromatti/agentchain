#include <stdlib.h>
#include <string.h>

#include "tx_builder.h"
#include "crypto.h"
#include "network.h"

static int tx_build(tx** out, uint8_t function_id, uint32_t accounts_num,
                    account_list_node* accounts, tx_data* data,
                    const account* signer) {
    if (!out || !accounts || !signer) return -1;
    tx* t = calloc(1, sizeof(*t));
    if (!t) return -2;

    t->expire = network_time_now() + 3600;
    t->signer = accounts->acc;
    t->function_id = function_id;
    t->accounts_num = accounts_num;
    t->accounts = accounts;
    t->data = data;
    t->confirmed = 0;

    if (sign_tx(t, signer) < 0) {
        tx_free(t);
        return -3;
    }

    *out = t;
    return 0;
}

static int build_amount_data(uint64_t amount, tx_data** out_data) {
    tx_data* data = calloc(1, sizeof(*data));
    if (!data) return -1;
    data->data_len = sizeof(uint64_t);
    memcpy(data->data, &amount, sizeof(uint64_t));
    *out_data = data;
    return 0;
}

int tx_build_transfer(tx** out, const account* sender, const pub_key_t receiver, uint64_t amount) {
    if (!out || !sender || !receiver) return -1;

    account* sender_acc = calloc(1, sizeof(*sender_acc));
    if (!sender_acc) return -2;
    memcpy(sender_acc->pub_key, sender->pub_key, crypto_sign_PUBLICKEYBYTES);
    memcpy(sender_acc->priv_key, sender->priv_key, crypto_sign_SECRETKEYBYTES);

    account* receiver_acc = calloc(1, sizeof(*receiver_acc));
    if (!receiver_acc) {
        free(sender_acc);
        return -3;
    }
    memcpy(receiver_acc->pub_key, receiver, crypto_sign_PUBLICKEYBYTES);

    account_list_node* node1 = calloc(1, sizeof(*node1));
    account_list_node* node2 = calloc(1, sizeof(*node2));
    if (!node1 || !node2) {
        free(sender_acc);
        free(receiver_acc);
        free(node1);
        free(node2);
        return -4;
    }

    node1->acc = sender_acc;
    node1->next = node2;
    node2->acc = receiver_acc;
    node2->next = NULL;

    tx_data* data = NULL;
    if (build_amount_data(amount, &data) < 0) {
        free(sender_acc);
        free(receiver_acc);
        free(node1);
        free(node2);
        return -5;
    }

    return tx_build(out, 1, 2, node1, data, sender_acc);
}

int tx_build_mint(tx** out, const account* minter, const pub_key_t receiver, uint64_t amount) {
    if (!out || !minter || !receiver) return -1;

    account* minter_acc = calloc(1, sizeof(*minter_acc));
    if (!minter_acc) return -2;
    memcpy(minter_acc->pub_key, minter->pub_key, crypto_sign_PUBLICKEYBYTES);
    memcpy(minter_acc->priv_key, minter->priv_key, crypto_sign_SECRETKEYBYTES);

    account* receiver_acc = calloc(1, sizeof(*receiver_acc));
    if (!receiver_acc) {
        free(minter_acc);
        return -3;
    }
    memcpy(receiver_acc->pub_key, receiver, crypto_sign_PUBLICKEYBYTES);

    account_list_node* node1 = calloc(1, sizeof(*node1));
    account_list_node* node2 = calloc(1, sizeof(*node2));
    if (!node1 || !node2) {
        free(minter_acc);
        free(receiver_acc);
        free(node1);
        free(node2);
        return -4;
    }

    node1->acc = minter_acc;
    node1->next = node2;
    node2->acc = receiver_acc;
    node2->next = NULL;

    tx_data* data = NULL;
    if (build_amount_data(amount, &data) < 0) {
        free(minter_acc);
        free(receiver_acc);
        free(node1);
        free(node2);
        return -5;
    }

    return tx_build(out, 2, 2, node1, data, minter_acc);
}

void tx_free(tx* t) {
    if (!t) return;
    if (t->data) {
        free(t->data);
        t->data = NULL;
    }

    account_list_node* cur = t->accounts;
    while (cur) {
        account_list_node* next = cur->next;
        if (cur->acc) free(cur->acc);
        free(cur);
        cur = next;
    }
    t->accounts = NULL;
    t->signer = NULL;
    free(t);
}
