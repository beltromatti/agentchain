#pragma once
#ifndef NETWORK_H
#define NETWORK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "types.h"

int network_init(void);
void network_shutdown(void);
int network_set_identity(const pub_key_t pub_key, const priv_key_t priv_key);
int network_peer_add_addr(const char* ip, uint16_t port);
uint16_t network_listen_port(void);
size_t network_peer_count_online(void);
size_t network_peer_snapshot_pubkeys(pub_key_t** out_pub_keys);

int network_peer_add(uint64_t peer_id, const pub_key_t peer_pub_key, peer_send_fn send_cb, void* user_ctx);
int network_peer_remove(uint64_t peer_id);
int network_peer_set_online(uint64_t peer_id, int online);
int network_peer_update_time(uint64_t peer_id, uint64_t peer_time_unix);

uint64_t network_time_now(void);

int encode_tx(tx* transaction, uint8_t encoded_tx[], size_t* raw_tx_len);
int decode_tx(tx* transaction, uint8_t encoded_tx[], size_t raw_tx_len);

int share_tx_with_peer(tx* transaction, pub_key_t peer_pub_key);
int handle_incoming_tx(uint8_t encoded_tx[], size_t raw_tx_len);

int network_broadcast_block(const uint8_t* data, size_t len);
int network_broadcast_vote(const uint8_t* data, size_t len);

int network_request_chain_state(void);
int network_request_snapshot(void);
int network_get_remote_chain_state(uint64_t* out_chain_id, pub_key_t out_genesis_pub,
                                  uint64_t* out_height, uint64_t* out_tip_id);
int network_get_remote_snapshot(uint8_t* out, size_t* inout_len);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_H */
