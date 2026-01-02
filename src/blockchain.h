#pragma once
#ifndef BLOCKCHAIN_H
#define BLOCKCHAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

extern blockchain CHAIN;

int blockchain_init(blockchain* bc);
int blockchain_bootstrap(blockchain* bc, const account* genesis);
int blockchain_register_account(blockchain* bc, const account* acc);
account* blockchain_get_account(blockchain* bc, const pub_key_t key);
account* blockchain_get_or_create_account(blockchain* bc, const pub_key_t key);
long long get_account_balance(account* acc);

#ifdef __cplusplus
}
#endif

#endif /* BLOCKCHAIN_H */
