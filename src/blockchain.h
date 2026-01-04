#pragma once
#ifndef BLOCKCHAIN_H
#define BLOCKCHAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

extern blockchain CHAIN;

int blockchain_init(blockchain* bc);
int blockchain_load_chain_state(blockchain* bc);
int blockchain_create_chain_state(blockchain* bc, const account* genesis);
int blockchain_accept_remote_chain_state(blockchain* bc, uint64_t chain_id, const pub_key_t genesis_pub);
int blockchain_bootstrap(blockchain* bc, const account* genesis);
int blockchain_register_account(blockchain* bc, const account* acc);
account* blockchain_get_account(blockchain* bc, const pub_key_t key);
account* blockchain_get_or_create_account(blockchain* bc, const pub_key_t key);
long long get_account_balance(account* acc);
int blockchain_encode_snapshot(blockchain* bc, uint8_t** out, size_t* out_len);
int blockchain_apply_snapshot(blockchain* bc, const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* BLOCKCHAIN_H */
