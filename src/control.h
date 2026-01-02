#pragma once
#ifndef CONTROL_H
#define CONTROL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "types.h"

int control_start(blockchain* bc);
void control_stop(void);
int control_send_tx(const uint8_t* data, size_t len);
int control_get_balance(const pub_key_t key, uint64_t* out_balance);
int control_ping(void);
uint16_t control_port(void);

#ifdef __cplusplus
}
#endif

#endif /* CONTROL_H */
