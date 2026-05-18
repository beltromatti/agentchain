#include "crypto.h"

#include <sodium.h>
#include <string.h>

/* libsodium gives us:
 *   crypto_sign_PUBLICKEYBYTES = 32
 *   crypto_sign_SECRETKEYBYTES = 64 (seed||pk concatenation)
 *   crypto_sign_SEEDBYTES      = 32
 *   crypto_sign_BYTES          = 64 (signature)
 *   crypto_generichash         = BLAKE2b
 */

/* Compile-time agreement with our public constants. */
_Static_assert(AC_PUBKEY_SIZE   == crypto_sign_PUBLICKEYBYTES, "pubkey size");
_Static_assert(AC_PRIVKEY_SIZE  == crypto_sign_SECRETKEYBYTES, "privkey size");
_Static_assert(AC_SEED_SIZE     == crypto_sign_SEEDBYTES,      "seed size");
_Static_assert(AC_SIG_SIZE      == crypto_sign_BYTES,          "sig size");

int ac_crypto_init(void) {
    if (sodium_init() < 0) return -1;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Keypairs.                                                                  */
/* -------------------------------------------------------------------------- */

int ac_keypair_random(ac_keypair_t *kp) {
    uint8_t seed[AC_SEED_SIZE];
    randombytes_buf(seed, sizeof(seed));
    int rc = crypto_sign_seed_keypair(kp->pk, kp->sk, seed);
    ac_secure_zero(seed, sizeof(seed));
    return rc;
}

int ac_keypair_from_seed(ac_keypair_t *kp, const uint8_t seed[AC_SEED_SIZE]) {
    return crypto_sign_seed_keypair(kp->pk, kp->sk, seed);
}

void ac_keypair_seed(uint8_t seed_out[AC_SEED_SIZE], const ac_keypair_t *kp) {
    /* RFC 8032: the first 32 bytes of the secret key in libsodium's layout
     * are precisely the seed. */
    memcpy(seed_out, kp->sk, AC_SEED_SIZE);
}

/* -------------------------------------------------------------------------- */
/* Hash and sign.                                                             */
/* -------------------------------------------------------------------------- */

void ac_hash_multi(ac_hash_t *out,
                   const uint8_t *const *chunks,
                   const size_t *lens,
                   size_t n) {
    crypto_generichash_state st;
    crypto_generichash_init(&st, NULL, 0, AC_HASH_SIZE);
    for (size_t i = 0; i < n; ++i) {
        if (lens[i] > 0 && chunks[i] != NULL) {
            crypto_generichash_update(&st, chunks[i], lens[i]);
        }
    }
    crypto_generichash_final(&st, out->b, AC_HASH_SIZE);
}

void ac_hash(ac_hash_t *out, const uint8_t *data, size_t len) {
    crypto_generichash(out->b, AC_HASH_SIZE, data, len, NULL, 0);
}

void ac_sign(ac_sig_t *out, const uint8_t *msg, size_t len, const ac_keypair_t *kp) {
    unsigned long long siglen = 0;
    crypto_sign_detached(out->b, &siglen, msg, len, kp->sk);
}

int ac_verify(const ac_sig_t *sig,
              const uint8_t *msg, size_t len,
              const uint8_t pk[AC_PUBKEY_SIZE]) {
    return crypto_sign_verify_detached(sig->b, msg, len, pk) == 0 ? 1 : 0;
}

/* -------------------------------------------------------------------------- */
/* VRF — deterministic Ed25519 signature with domain tag.                     */
/*                                                                            */
/*   alpha_tagged = "AGCH:VRF:v1" || alpha                                    */
/*   proof        = Ed25519_sign(sk, alpha_tagged)                            */
/*   beta         = BLAKE2b-256("AGCH:VRF-OUT:v1" || proof)                   */
/* -------------------------------------------------------------------------- */

static const char VRF_DOMAIN[]     = "AGCH:VRF:v1";
static const char VRF_OUT_DOMAIN[] = "AGCH:VRF-OUT:v1";

static void vrf_compute_beta(ac_vrf_out_t *beta, const uint8_t proof[AC_VRF_PROOF_SIZE]) {
    const uint8_t *chunks[2] = { (const uint8_t *)VRF_OUT_DOMAIN, proof };
    const size_t   lens[2]   = { sizeof(VRF_OUT_DOMAIN) - 1,       AC_VRF_PROOF_SIZE };
    ac_hash_t h;
    ac_hash_multi(&h, chunks, lens, 2);
    memcpy(beta->b, h.b, AC_VRF_OUT_SIZE);
}

void ac_vrf_prove(ac_vrf_proof_t *proof,
                  ac_vrf_out_t   *beta,
                  const uint8_t  *alpha, size_t alpha_len,
                  const ac_keypair_t *kp) {
    /* Build alpha_tagged in a temporary buffer (stack — VRF inputs are tiny). */
    uint8_t buf[1024];
    size_t  buf_len = sizeof(VRF_DOMAIN) - 1 + alpha_len;

    uint8_t *p = buf;
    bool heap = false;
    if (buf_len > sizeof(buf)) {
        p = (uint8_t *)malloc(buf_len);
        heap = true;
    }
    memcpy(p, VRF_DOMAIN, sizeof(VRF_DOMAIN) - 1);
    if (alpha_len > 0) memcpy(p + sizeof(VRF_DOMAIN) - 1, alpha, alpha_len);

    ac_sig_t sig;
    ac_sign(&sig, p, buf_len, kp);
    memcpy(proof->b, sig.b, AC_VRF_PROOF_SIZE);
    if (beta) vrf_compute_beta(beta, proof->b);

    if (heap) free(p);
}

int ac_vrf_verify(ac_vrf_out_t  *beta,
                  const ac_vrf_proof_t *proof,
                  const uint8_t *alpha, size_t alpha_len,
                  const uint8_t  pk[AC_PUBKEY_SIZE]) {
    uint8_t buf[1024];
    size_t  buf_len = sizeof(VRF_DOMAIN) - 1 + alpha_len;

    uint8_t *p = buf;
    bool heap = false;
    if (buf_len > sizeof(buf)) {
        p = (uint8_t *)malloc(buf_len);
        heap = true;
    }
    memcpy(p, VRF_DOMAIN, sizeof(VRF_DOMAIN) - 1);
    if (alpha_len > 0) memcpy(p + sizeof(VRF_DOMAIN) - 1, alpha, alpha_len);

    ac_sig_t sig;
    memcpy(sig.b, proof->b, AC_VRF_PROOF_SIZE);
    int ok = ac_verify(&sig, p, buf_len, pk);

    if (heap) free(p);

    if (!ok) return 0;
    if (beta) vrf_compute_beta(beta, proof->b);
    return 1;
}

/* -------------------------------------------------------------------------- */
/* RNG.                                                                       */
/* -------------------------------------------------------------------------- */

void ac_random_bytes(uint8_t *out, size_t len) {
    randombytes_buf(out, len);
}

uint64_t ac_random_u64(void) {
    uint8_t b[8];
    randombytes_buf(b, sizeof(b));
    return ac_rd64(b);
}
