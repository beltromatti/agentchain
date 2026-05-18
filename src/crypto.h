/* AgentChain Engine — cryptographic primitives.
 *
 * Wraps libsodium and implements the deterministic Ed25519-based VRF
 * documented in PROTOCOL.md § 3.2.
 */

#ifndef AGENTCHAIN_CRYPTO_H
#define AGENTCHAIN_CRYPTO_H

#include "common.h"

/* One-time initialiser. Must be called before any other crypto function.
 * Returns 0 on success. */
int ac_crypto_init(void);

/* -------------------------------------------------------------------------- */
/* Key material.                                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t pk[AC_PUBKEY_SIZE];
    uint8_t sk[AC_PRIVKEY_SIZE];
} ac_keypair_t;

/* Generates a fresh keypair from the OS RNG. */
int ac_keypair_random(ac_keypair_t *kp);

/* Derives a keypair from a 32-byte seed (deterministic). */
int ac_keypair_from_seed(ac_keypair_t *kp, const uint8_t seed[AC_SEED_SIZE]);

/* Recovers the seed from an Ed25519 secret key (first 32 bytes by RFC 8032). */
void ac_keypair_seed(uint8_t seed_out[AC_SEED_SIZE], const ac_keypair_t *kp);

/* -------------------------------------------------------------------------- */
/* Hash and signatures.                                                       */
/* -------------------------------------------------------------------------- */

/* BLAKE2b-256 keyless hash. Concatenates [chunks[i] for i in 0..n).
 * `chunks` and `lens` parallel arrays of length `n`. */
void ac_hash_multi(ac_hash_t *out,
                   const uint8_t *const *chunks,
                   const size_t *lens,
                   size_t n);

/* Convenience: single-chunk hash. */
void ac_hash(ac_hash_t *out, const uint8_t *data, size_t len);

/* Ed25519 sign. msg/len arbitrary. */
void ac_sign(ac_sig_t *out, const uint8_t *msg, size_t len, const ac_keypair_t *kp);

/* Ed25519 verify. Returns 1 on valid, 0 on invalid. */
int ac_verify(const ac_sig_t *sig,
              const uint8_t *msg, size_t len,
              const uint8_t pk[AC_PUBKEY_SIZE]);

/* -------------------------------------------------------------------------- */
/* VRF (deterministic Ed25519-based, PROTOCOL § 3.2).                         */
/* -------------------------------------------------------------------------- */

typedef struct { uint8_t b[AC_VRF_PROOF_SIZE]; } ac_vrf_proof_t;
typedef struct { uint8_t b[AC_VRF_OUT_SIZE];   } ac_vrf_out_t;

/* Produce VRF proof and output for input alpha. */
void ac_vrf_prove(ac_vrf_proof_t *proof,
                  ac_vrf_out_t   *beta,
                  const uint8_t  *alpha, size_t alpha_len,
                  const ac_keypair_t *kp);

/* Verify proof for input alpha against public key.
 * On success writes beta and returns 1. On failure returns 0. */
int ac_vrf_verify(ac_vrf_out_t  *beta,
                  const ac_vrf_proof_t *proof,
                  const uint8_t *alpha, size_t alpha_len,
                  const uint8_t  pk[AC_PUBKEY_SIZE]);

/* -------------------------------------------------------------------------- */
/* Random.                                                                    */
/* -------------------------------------------------------------------------- */

void ac_random_bytes(uint8_t *out, size_t len);
uint64_t ac_random_u64(void);

#endif /* AGENTCHAIN_CRYPTO_H */
