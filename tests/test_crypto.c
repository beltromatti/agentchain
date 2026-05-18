/* Sanity tests for cryptographic primitives. */
#include "crypto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_keypair_roundtrip(void) {
    ac_keypair_t a;
    ac_keypair_t b;
    assert(ac_keypair_random(&a) == 0);
    /* Re-derive from seed produces same pubkey. */
    uint8_t seed[AC_SEED_SIZE];
    ac_keypair_seed(seed, &a);
    assert(ac_keypair_from_seed(&b, seed) == 0);
    assert(memcmp(a.pk, b.pk, AC_PUBKEY_SIZE) == 0);
    (void)b;
}

static void test_sign_verify(void) {
    ac_keypair_t kp;
    assert(ac_keypair_random(&kp) == 0);
    const uint8_t msg[] = "hello agentchain";
    ac_sig_t sig;
    ac_sign(&sig, msg, sizeof(msg) - 1, &kp);
    assert(ac_verify(&sig, msg, sizeof(msg) - 1, kp.pk) == 1);
    /* Tamper. */
    sig.b[0] ^= 1;
    assert(ac_verify(&sig, msg, sizeof(msg) - 1, kp.pk) == 0);
}

static void test_vrf_roundtrip(void) {
    ac_keypair_t kp;
    assert(ac_keypair_random(&kp) == 0);
    const uint8_t alpha[] = "AGCH:LEADER" "\x00\x00\x00\x00\x00\x00\x00\x05";
    ac_vrf_proof_t proof;
    ac_vrf_out_t   beta1;
    ac_vrf_out_t   beta2;
    ac_vrf_prove (&proof, &beta1, alpha, sizeof(alpha) - 1, &kp);
    assert(ac_vrf_verify(&beta2, &proof, alpha, sizeof(alpha) - 1, kp.pk) == 1);
    assert(memcmp(beta1.b, beta2.b, AC_VRF_OUT_SIZE) == 0);

    /* Determinism: repeated proves give identical outputs. */
    ac_vrf_proof_t proof2;
    ac_vrf_out_t   beta3;
    ac_vrf_prove(&proof2, &beta3, alpha, sizeof(alpha) - 1, &kp);
    assert(memcmp(proof.b, proof2.b, AC_VRF_PROOF_SIZE) == 0);
    assert(memcmp(beta1.b, beta3.b, AC_VRF_OUT_SIZE) == 0);
}

static void test_hash_chunks(void) {
    ac_hash_t h1, h2;
    const uint8_t a[] = "agentchain";
    const uint8_t b[] = "rules";
    const uint8_t both[] = "agentchainrules";
    const uint8_t *chunks[2] = {a, b};
    const size_t   lens[2]   = {sizeof(a) - 1, sizeof(b) - 1};
    ac_hash_multi(&h1, chunks, lens, 2);
    ac_hash(&h2, both, sizeof(both) - 1);
    assert(memcmp(h1.b, h2.b, AC_HASH_SIZE) == 0);
}

int main(void) {
    if (ac_crypto_init() != 0) { fprintf(stderr, "crypto init failed\n"); return 1; }
    test_keypair_roundtrip();
    test_sign_verify();
    test_vrf_roundtrip();
    test_hash_chunks();
    printf("test_crypto OK\n");
    return 0;
}
