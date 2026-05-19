/* Round-trip tests for Tx/Block canonical encoders. */
#include "codec.h"
#include "crypto.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_transfer_tx(void) {
    ac_keypair_t kp;
    assert(ac_keypair_random(&kp) == 0);

    ac_tx_t tx;
    memset(&tx, 0, sizeof(tx));
    tx.version     = AC_TX_VERSION;
    tx.chain_id    = 2025;
    tx.kind        = AC_TX_TRANSFER;
    memcpy(tx.sender.b, kp.pk, AC_PUBKEY_SIZE);
    tx.nonce       = 7;
    tx.gas_limit   = 200;
    tx.tip         = 3;
    tx.valid_until = 99999;

    ac_body_transfer_t b;
    for (int i = 0; i < AC_PUBKEY_SIZE; ++i) b.recipient.b[i] = (uint8_t)(0xA0 + i);
    b.amount = 12345;
    int bn = ac_body_transfer_encode(tx.body, AC_TX_BODY_MAX, &b);
    assert(bn == AC_PUBKEY_SIZE + 8);
    tx.body_len = (uint32_t)bn;

    const char memo[] = "hello world";
    memcpy(tx.memo, memo, sizeof(memo) - 1);
    tx.memo_len = sizeof(memo) - 1;

    assert(ac_tx_sign(&tx, &kp) == 0);
    assert(ac_tx_verify(&tx) == 1);

    /* Encode/decode round-trip. */
    uint8_t buf[AC_TX_MAX_BYTES];
    int n = ac_tx_encode(buf, sizeof(buf), &tx);
    assert(n > 0);
    ac_tx_t tx2;
    int n2 = ac_tx_decode(&tx2, buf, (size_t)n);
    assert(n2 == n);
    assert(tx2.chain_id == tx.chain_id);
    assert(tx2.nonce    == tx.nonce);
    assert(tx2.tip      == tx.tip);
    assert(tx2.body_len == tx.body_len);
    assert(tx2.memo_len == tx.memo_len);
    assert(memcmp(tx2.sig.b, tx.sig.b, AC_SIG_SIZE) == 0);
    assert(ac_tx_verify(&tx2) == 1);

    /* Hash stability. */
    ac_hash_t h1, h2;
    ac_tx_hash(&h1, &tx);
    ac_tx_hash(&h2, &tx2);
    assert(memcmp(h1.b, h2.b, AC_HASH_SIZE) == 0);
}

static void test_header_roundtrip(void) {
    ac_block_header_t h;
    memset(&h, 0, sizeof(h));
    h.version       = AC_BLOCK_VERSION;
    h.height        = 42;
    h.slot          = 99;
    h.timestamp_ms  = 1234567890000ULL;
    h.base_fee      = 5;
    h.gas_used      = 1000;
    h.gas_limit     = AC_BLOCK_GAS_LIMIT;
    h.tx_count      = 3;
    for (int i = 0; i < AC_PUBKEY_SIZE; ++i) h.proposer.b[i] = (uint8_t)i;
    for (int i = 0; i < AC_HASH_SIZE; ++i) {
        h.parent_hash.b[i] = (uint8_t)(0x10 + i);
        h.state_root.b[i]  = (uint8_t)(0x20 + i);
        h.tx_root.b[i]     = (uint8_t)(0x30 + i);
    }
    for (int i = 0; i < AC_VRF_PROOF_SIZE; ++i) h.proposer_vrf_proof[i] = (uint8_t)(0x40 + i);

    uint8_t buf[256];
    int n = ac_block_header_encode(buf, sizeof(buf), &h);
    assert(n > 0);
    ac_block_header_t h2;
    int n2 = ac_block_header_decode(&h2, buf, (size_t)n);
    assert(n2 == n);
    assert(memcmp(&h, &h2, sizeof(h)) == 0);
}

static void test_full_block_with_tx_roundtrip(void) {
    /* Regression test for the ac_block_decode bug where the tx-length
     * walker read body_len at the wrong offset, so any committed block
     * containing a transaction failed to deserialise (blocking sync). */
    ac_keypair_t kp;
    assert(ac_keypair_random(&kp) == 0);

    ac_tx_t tx;
    memset(&tx, 0, sizeof(tx));
    tx.version    = AC_TX_VERSION;
    tx.chain_id   = 1;
    tx.kind       = AC_TX_TRANSFER;
    memcpy(tx.sender.b, kp.pk, AC_PUBKEY_SIZE);
    tx.nonce      = 0;
    tx.gas_limit  = 200;
    tx.tip        = 1;
    tx.valid_until = 1000000;
    ac_body_transfer_t bt;
    memset(&bt, 0xAA, sizeof(bt));
    bt.amount = 7;
    int bn = ac_body_transfer_encode(tx.body, AC_TX_BODY_MAX, &bt);
    assert(bn > 0);
    tx.body_len = (uint32_t)bn;
    const char memo[] = "hello mainnet";
    memcpy(tx.memo, memo, sizeof(memo) - 1);
    tx.memo_len = sizeof(memo) - 1;
    assert(ac_tx_sign(&tx, &kp) == 0);
    assert(ac_tx_verify(&tx) == 1);

    ac_block_t blk;
    memset(&blk, 0, sizeof(blk));
    blk.header.version  = AC_BLOCK_VERSION;
    blk.header.height   = 65;
    blk.header.slot     = 100;
    blk.header.gas_limit = AC_BLOCK_GAS_LIMIT;
    blk.header.tx_count  = 1;
    blk.header.base_fee  = 1;
    blk.txs = &tx;
    blk.tx_count = 1;
    /* No commit cert for the encode test. */

    uint8_t *enc = NULL;
    size_t   enc_len = 0;
    assert(ac_block_encode(&enc, &enc_len, &blk) > 0);

    ac_block_t back;
    int rc = ac_block_decode(&back, enc, enc_len);
    assert(rc > 0);
    assert(back.tx_count == 1);
    assert(back.txs[0].nonce == 0);
    assert(back.txs[0].memo_len == sizeof(memo) - 1);
    assert(memcmp(back.txs[0].memo, memo, sizeof(memo) - 1) == 0);
    assert(ac_tx_verify(&back.txs[0]) == 1);

    free(enc);
    ac_block_free(&back);
}

int main(void) {
    if (ac_crypto_init() != 0) { fprintf(stderr, "crypto init failed\n"); return 1; }
    test_transfer_tx();
    test_header_roundtrip();
    test_full_block_with_tx_roundtrip();
    printf("test_codec OK\n");
    return 0;
}
