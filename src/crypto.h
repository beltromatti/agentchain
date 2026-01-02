#pragma once
#ifndef CRYPTO_H
#define CRYPTO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sodium.h>

#include "types.h"

/*
 * crypto.h
 *
 * Transaction signing / verification helpers (Ed25519) + canonical TX hash for signature.
 *
 * Notes:
 * - Requires libsodium initialized via `sodium_init()` once at program start.
 * - `account` is assumed to store:
 *     - pub_key[crypto_sign_PUBLICKEYBYTES]  (32 bytes)
 *     - priv_key[crypto_sign_SECRETKEYBYTES] (64 bytes)
 * - `tx` is assumed to store:
 *     - uint8_t signature[crypto_sign_BYTES] (64 bytes)
 *     - `signer` pointer to `account`
 *
 * Error codes are negative values (0 means success/valid).
 */

/* =========================
 * Key management
 * ========================= */

/* Generates a fresh random keypair for account `a`.
 * Returns: 0 on success, <0 on error.
 */
int generate_keys(account *a);

/* Deterministically generates a keypair from a 32-byte seed.
 * Returns: 0 on success, <0 on error.
 */
int generate_keys_from_seed(account *a, const uint8_t seed[crypto_sign_SEEDBYTES]);

/* Derives Ed25519 public key from a 64-byte secret key.
 * Returns: 0 on success, <0 on error.
 */
int derive_pub_key(const uint8_t priv_key[crypto_sign_SECRETKEYBYTES],
                   uint8_t pub_key_out[crypto_sign_PUBLICKEYBYTES]);

/* =========================
 * Transaction signing / verification
 * ========================= */

/* Signs transaction `t` using `signer` (must match `t->signer`).
 * Writes detached signature into `t->signature`.
 * Returns: 0 on success, <0 on error.
 */
int sign_tx(tx *t, const account *signer);

/* Verifies detached signature of transaction `t` against `t->signer->pub_key`.
 * Returns:
 *   0  => valid
 *  <0  => invalid or error
 */
int verify_tx(const tx *t);

#ifdef __cplusplus
}
#endif

#endif /* CRYPTO_H */
