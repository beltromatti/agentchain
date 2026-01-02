#pragma once
#ifndef TX_BUILDER_H
#define TX_BUILDER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "types.h"

int tx_build_transfer(tx** out, const account* sender, const pub_key_t receiver, uint64_t amount);
int tx_build_mint(tx** out, const account* minter, const pub_key_t receiver, uint64_t amount);
void tx_free(tx* t);

#ifdef __cplusplus
}
#endif

#endif /* TX_BUILDER_H */
