#pragma once
#ifndef CONSENSUS_H
#define CONSENSUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "types.h"

int consensus_init(blockchain* bc);
void consensus_shutdown(void);
int consensus_set_validator(const account* validator);

int consensus_handle_block(const uint8_t* data, size_t len);
int consensus_handle_vote(const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CONSENSUS_H */
