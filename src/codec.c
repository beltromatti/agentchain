#include "codec.h"

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Cursor helpers — bounded write + bounded read, returning bytes consumed.   */
/* -------------------------------------------------------------------------- */

typedef struct { uint8_t *p; size_t cap; size_t used; bool err; } wcur_t;
typedef struct { const uint8_t *p; size_t cap; size_t pos; bool err; } rcur_t;

static inline void wput(wcur_t *c, const void *src, size_t n) {
    if (c->err || c->used + n > c->cap) { c->err = true; return; }
    if (n > 0) memcpy(c->p + c->used, src, n);
    c->used += n;
}
static inline void wput_u8(wcur_t *c, uint8_t v)   { wput(c, &v, 1); }
static inline void wput_u32(wcur_t *c, uint32_t v) {
    uint8_t b[4]; ac_be32(b, v); wput(c, b, 4);
}
static inline void wput_u64(wcur_t *c, uint64_t v) {
    uint8_t b[8]; ac_be64(b, v); wput(c, b, 8);
}

static inline void rget(rcur_t *c, void *dst, size_t n) {
    if (c->err || c->pos + n > c->cap) { c->err = true; return; }
    if (n > 0) memcpy(dst, c->p + c->pos, n);
    c->pos += n;
}
static inline uint8_t  rget_u8(rcur_t *c)  {
    uint8_t b = 0; rget(c, &b, 1); return b;
}
static inline uint32_t rget_u32(rcur_t *c) {
    uint8_t b[4] = {0}; rget(c, b, 4); return ac_rd32(b);
}
static inline uint64_t rget_u64(rcur_t *c) {
    uint8_t b[8] = {0}; rget(c, b, 8); return ac_rd64(b);
}

/* -------------------------------------------------------------------------- */
/* Body encoders/decoders.                                                    */
/* -------------------------------------------------------------------------- */

int ac_body_transfer_encode(uint8_t *out, size_t cap, const ac_body_transfer_t *b) {
    wcur_t c = { .p = out, .cap = cap };
    wput(&c, b->recipient.b, AC_PUBKEY_SIZE);
    wput_u64(&c, b->amount);
    return c.err ? -1 : (int)c.used;
}
int ac_body_transfer_decode(ac_body_transfer_t *out, const uint8_t *buf, size_t len) {
    rcur_t c = { .p = buf, .cap = len };
    rget(&c, out->recipient.b, AC_PUBKEY_SIZE);
    out->amount = rget_u64(&c);
    return (c.err || c.pos != len) ? -1 : (int)c.pos;
}

int ac_body_stake_encode(uint8_t *out, size_t cap, const ac_body_stake_t *b) {
    if (cap < 8) return -1;
    ac_be64(out, b->amount);
    return 8;
}
int ac_body_stake_decode(ac_body_stake_t *out, const uint8_t *buf, size_t len) {
    if (len != 8) return -1;
    out->amount = ac_rd64(buf);
    return 8;
}

int ac_name_valid(const uint8_t *name, size_t name_len) {
    if (name_len < AC_NAME_MIN_LEN || name_len > AC_NAME_MAX_LEN) return 0;
    for (size_t i = 0; i < name_len; ++i) {
        uint8_t c = name[i];
        bool ok = (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '-';
        if (!ok) return 0;
    }
    return 1;
}

int ac_body_name_encode(uint8_t *out, size_t cap, const ac_body_name_t *b) {
    if (b->name_len == 0 || b->name_len > AC_NAME_MAX_LEN) return -1;
    if (cap < (size_t)b->name_len + 1) return -1;
    out[0] = b->name_len;
    memcpy(out + 1, b->name, b->name_len);
    return 1 + b->name_len;
}
int ac_body_name_decode(ac_body_name_t *out, const uint8_t *buf, size_t len) {
    if (len < 1) return -1;
    uint8_t nl = buf[0];
    if (nl == 0 || nl > AC_NAME_MAX_LEN) return -1;
    if ((size_t)nl + 1 != len) return -1;
    out->name_len = nl;
    memcpy(out->name, buf + 1, nl);
    if (!ac_name_valid(out->name, nl)) return -1;
    return (int)len;
}

int ac_body_slash_encode(uint8_t *out, size_t cap, const ac_body_slash_t *b) {
    wcur_t c = { .p = out, .cap = cap };
    wput(&c, b->validator.b, AC_PUBKEY_SIZE);
    wput_u64(&c, b->height);
    wput(&c, b->block_hash_a.b, AC_HASH_SIZE);
    wput(&c, b->sig_a.b, AC_SIG_SIZE);
    wput(&c, b->block_hash_b.b, AC_HASH_SIZE);
    wput(&c, b->sig_b.b, AC_SIG_SIZE);
    return c.err ? -1 : (int)c.used;
}
int ac_body_slash_decode(ac_body_slash_t *out, const uint8_t *buf, size_t len) {
    rcur_t c = { .p = buf, .cap = len };
    rget(&c, out->validator.b, AC_PUBKEY_SIZE);
    out->height = rget_u64(&c);
    rget(&c, out->block_hash_a.b, AC_HASH_SIZE);
    rget(&c, out->sig_a.b,        AC_SIG_SIZE);
    rget(&c, out->block_hash_b.b, AC_HASH_SIZE);
    rget(&c, out->sig_b.b,        AC_SIG_SIZE);
    return (c.err || c.pos != len) ? -1 : (int)c.pos;
}

/* -------------------------------------------------------------------------- */
/* Transactions.                                                              */
/* -------------------------------------------------------------------------- */

int ac_tx_intrinsic_gas(uint8_t kind) {
    switch (kind) {
    case AC_TX_TRANSFER:       return 21 + 0;
    case AC_TX_STAKE_BOND:
    case AC_TX_STAKE_UNBOND:   return 21 + 30;
    case AC_TX_REGISTER_NAME:  return 21 + 200;
    case AC_TX_SLASH_EVIDENCE: return 21 + 500;
    default: return -1;
    }
}

uint64_t ac_tx_total_gas(const ac_tx_t *tx) {
    int g = ac_tx_intrinsic_gas(tx->kind);
    if (g < 0) return UINT64_MAX;
    return (uint64_t)g + tx->memo_len; /* memo: +1 gas/byte */
}

int ac_tx_signable_bytes(uint8_t *out, size_t out_cap, const ac_tx_t *tx) {
    static const char DOMAIN[] = "AGCH:TX:v1";
    wcur_t c = { .p = out, .cap = out_cap };
    wput(&c, DOMAIN, sizeof(DOMAIN) - 1);
    wput_u8(&c,  tx->version);
    wput_u64(&c, tx->chain_id);
    wput_u8(&c,  tx->kind);
    wput(&c, tx->sender.b, AC_PUBKEY_SIZE);
    wput_u64(&c, tx->nonce);
    wput_u32(&c, tx->gas_limit);
    wput_u64(&c, tx->tip);
    wput_u64(&c, tx->valid_until);
    wput_u32(&c, tx->body_len);
    wput(&c, tx->body, tx->body_len);
    wput_u32(&c, tx->memo_len);
    wput(&c, tx->memo, tx->memo_len);
    return c.err ? -1 : (int)c.used;
}

int ac_tx_encode(uint8_t *out, size_t out_cap, const ac_tx_t *tx) {
    wcur_t c = { .p = out, .cap = out_cap };
    wput_u8(&c,  tx->version);
    wput_u64(&c, tx->chain_id);
    wput_u8(&c,  tx->kind);
    wput(&c, tx->sender.b, AC_PUBKEY_SIZE);
    wput_u64(&c, tx->nonce);
    wput_u32(&c, tx->gas_limit);
    wput_u64(&c, tx->tip);
    wput_u64(&c, tx->valid_until);
    wput_u32(&c, tx->body_len);
    wput(&c, tx->body, tx->body_len);
    wput_u32(&c, tx->memo_len);
    wput(&c, tx->memo, tx->memo_len);
    wput(&c, tx->sig.b, AC_SIG_SIZE);
    return c.err ? -1 : (int)c.used;
}

int ac_tx_decode(ac_tx_t *out, const uint8_t *buf, size_t len) {
    if (len > AC_TX_MAX_BYTES) return -1;
    rcur_t c = { .p = buf, .cap = len };
    memset(out, 0, sizeof(*out));
    out->version   = rget_u8(&c);
    out->chain_id  = rget_u64(&c);
    out->kind      = rget_u8(&c);
    rget(&c, out->sender.b, AC_PUBKEY_SIZE);
    out->nonce     = rget_u64(&c);
    out->gas_limit = rget_u32(&c);
    out->tip       = rget_u64(&c);
    out->valid_until = rget_u64(&c);
    out->body_len  = rget_u32(&c);
    if (out->body_len > AC_TX_BODY_MAX) return -1;
    rget(&c, out->body, out->body_len);
    out->memo_len  = rget_u32(&c);
    if (out->memo_len > AC_MEMO_MAX) return -1;
    rget(&c, out->memo, out->memo_len);
    rget(&c, out->sig.b, AC_SIG_SIZE);
    if (c.err || c.pos != len) return -1;
    return (int)c.pos;
}

void ac_tx_hash(ac_hash_t *out, const ac_tx_t *tx) {
    uint8_t buf[AC_TX_MAX_BYTES];
    int n = ac_tx_signable_bytes(buf, sizeof(buf), tx);
    if (n < 0) { memset(out, 0, sizeof(*out)); return; }
    /* TxSign := BLAKE2b-256("AGCH:TX:v1" || TxBody) — already produced above. */
    ac_hash(out, buf, (size_t)n);
}

int ac_tx_sign(ac_tx_t *tx, const ac_keypair_t *kp) {
    uint8_t buf[AC_TX_MAX_BYTES];
    int n = ac_tx_signable_bytes(buf, sizeof(buf), tx);
    if (n < 0) return -1;
    ac_hash_t h;
    ac_hash(&h, buf, (size_t)n);
    ac_sign(&tx->sig, h.b, AC_HASH_SIZE, kp);
    return 0;
}

int ac_tx_verify(const ac_tx_t *tx) {
    uint8_t buf[AC_TX_MAX_BYTES];
    int n = ac_tx_signable_bytes(buf, sizeof(buf), tx);
    if (n < 0) return 0;
    ac_hash_t h;
    ac_hash(&h, buf, (size_t)n);
    return ac_verify(&tx->sig, h.b, AC_HASH_SIZE, tx->sender.b);
}

/* -------------------------------------------------------------------------- */
/* Blocks.                                                                    */
/* -------------------------------------------------------------------------- */

int ac_block_header_encode(uint8_t *out, size_t cap, const ac_block_header_t *h) {
    wcur_t c = { .p = out, .cap = cap };
    wput_u8(&c,  h->version);
    wput_u64(&c, h->height);
    wput_u64(&c, h->slot);
    wput(&c, h->parent_hash.b, AC_HASH_SIZE);
    wput_u64(&c, h->timestamp_ms);
    wput(&c, h->proposer.b, AC_PUBKEY_SIZE);
    wput(&c, h->proposer_vrf_proof, AC_VRF_PROOF_SIZE);
    wput(&c, h->state_root.b, AC_HASH_SIZE);
    wput(&c, h->tx_root.b,    AC_HASH_SIZE);
    wput_u64(&c, h->base_fee);
    wput_u64(&c, h->gas_used);
    wput_u64(&c, h->gas_limit);
    wput_u32(&c, h->tx_count);
    return c.err ? -1 : (int)c.used;
}

int ac_block_header_decode(ac_block_header_t *out, const uint8_t *buf, size_t len) {
    rcur_t c = { .p = buf, .cap = len };
    memset(out, 0, sizeof(*out));
    out->version = rget_u8(&c);
    out->height  = rget_u64(&c);
    out->slot    = rget_u64(&c);
    rget(&c, out->parent_hash.b, AC_HASH_SIZE);
    out->timestamp_ms = rget_u64(&c);
    rget(&c, out->proposer.b, AC_PUBKEY_SIZE);
    rget(&c, out->proposer_vrf_proof, AC_VRF_PROOF_SIZE);
    rget(&c, out->state_root.b, AC_HASH_SIZE);
    rget(&c, out->tx_root.b,    AC_HASH_SIZE);
    out->base_fee  = rget_u64(&c);
    out->gas_used  = rget_u64(&c);
    out->gas_limit = rget_u64(&c);
    out->tx_count  = rget_u32(&c);
    if (c.err) return -1;
    return (int)c.pos;
}

#define HEADER_BYTES_MAX 256

void ac_block_hash(ac_hash_t *out, const ac_block_header_t *h) {
    static const char DOMAIN[] = "AGCH:BLOCK:v1";
    uint8_t buf[HEADER_BYTES_MAX];
    int n = ac_block_header_encode(buf, sizeof(buf), h);
    if (n < 0) { memset(out, 0, sizeof(*out)); return; }
    const uint8_t *chunks[2] = { (const uint8_t *)DOMAIN, buf };
    const size_t   lens[2]   = { sizeof(DOMAIN) - 1,       (size_t)n };
    ac_hash_multi(out, chunks, lens, 2);
}

void ac_block_tx_root(ac_hash_t *out, const ac_tx_t *txs, uint32_t tx_count) {
    if (tx_count == 0) {
        memset(out, 0, sizeof(*out));
        return;
    }
    /* Concatenate tx hashes and BLAKE2b. */
    ac_hash_t accum[1];
    (void)accum;
    uint8_t *concat = (uint8_t *)malloc((size_t)tx_count * AC_HASH_SIZE);
    if (!concat) { memset(out, 0, sizeof(*out)); return; }
    for (uint32_t i = 0; i < tx_count; ++i) {
        ac_hash_t h;
        ac_tx_hash(&h, &txs[i]);
        memcpy(concat + (size_t)i * AC_HASH_SIZE, h.b, AC_HASH_SIZE);
    }
    ac_hash(out, concat, (size_t)tx_count * AC_HASH_SIZE);
    free(concat);
}

int ac_vote_message(uint8_t out[64], uint64_t height, const ac_hash_t *block_hash) {
    static const char DOMAIN[] = "AGCH:VOTE:v1";
    /* DOMAIN(12) + 8 + 32 = 52 bytes < 64. */
    memcpy(out, DOMAIN, sizeof(DOMAIN) - 1);
    ac_be64(out + sizeof(DOMAIN) - 1, height);
    memcpy(out + sizeof(DOMAIN) - 1 + 8, block_hash->b, AC_HASH_SIZE);
    return (int)(sizeof(DOMAIN) - 1 + 8 + AC_HASH_SIZE);
}

/* Full block (header + txs + commit). Variable size; output is malloc'd. */
int ac_block_encode(uint8_t **out, size_t *out_len, const ac_block_t *b) {
    /* Upper bound: header + tx_count * MAX_TX_BYTES + commit. */
    size_t upper = HEADER_BYTES_MAX
                 + (size_t)b->tx_count * AC_TX_MAX_BYTES
                 + 4 /* nsigners */
                 + (size_t)b->nsigners * (AC_PUBKEY_SIZE + AC_SIG_SIZE + AC_VRF_PROOF_SIZE);
    uint8_t *buf = (uint8_t *)malloc(upper);
    if (!buf) return -1;

    wcur_t c = { .p = buf, .cap = upper };
    /* Header. */
    int hn = ac_block_header_encode(c.p + c.used, c.cap - c.used, &b->header);
    if (hn < 0) { free(buf); return -1; }
    c.used += (size_t)hn;

    /* Transactions. */
    for (uint32_t i = 0; i < b->tx_count; ++i) {
        int tn = ac_tx_encode(c.p + c.used, c.cap - c.used, &b->txs[i]);
        if (tn < 0) { free(buf); return -1; }
        c.used += (size_t)tn;
    }

    /* Commit certificate. */
    wput_u32(&c, b->nsigners);
    for (uint32_t i = 0; i < b->nsigners; ++i) {
        wput(&c, b->signers[i].signer.b, AC_PUBKEY_SIZE);
        wput(&c, b->signers[i].sig.b,    AC_SIG_SIZE);
        wput(&c, b->signers[i].vrf_proof, AC_VRF_PROOF_SIZE);
    }
    if (c.err) { free(buf); return -1; }

    *out = buf;
    *out_len = c.used;
    return (int)c.used;
}

int ac_block_decode(ac_block_t *out, const uint8_t *buf, size_t len) {
    memset(out, 0, sizeof(*out));
    if (len < HEADER_BYTES_MAX - 100) return -1; /* header alone is ~232 B */

    rcur_t c = { .p = buf, .cap = len };
    int hn = ac_block_header_decode(&out->header, c.p + c.pos, c.cap - c.pos);
    if (hn < 0) return -1;
    c.pos += (size_t)hn;

    if (out->header.tx_count > AC_BLOCK_MAX_TXS) return -1;

    /* Transactions. */
    if (out->header.tx_count > 0) {
        out->txs = (ac_tx_t *)calloc(out->header.tx_count, sizeof(ac_tx_t));
        if (!out->txs) return -1;
        out->tx_count = out->header.tx_count;
        for (uint32_t i = 0; i < out->header.tx_count; ++i) {
            /* Peek length: scan forward through fields up to memo, then add 64. */
            /* Simpler: try decoding from increasing-size windows. We know each
             * tx is at most AC_TX_MAX_BYTES. Compute exact length from the
             * encoded layout. */
            size_t remaining = c.cap - c.pos;
            const uint8_t *p = c.p + c.pos;
            /* Walk fixed prefix: 1+8+1+32+8+4+8+8 = 70 bytes. */
            if (remaining < 70) { ac_block_free(out); return -1; }
            uint32_t body_len = ac_rd32(p + 70 - 4);
            size_t pos = 70;
            pos += body_len;
            if (remaining < pos + 4) { ac_block_free(out); return -1; }
            uint32_t memo_len = ac_rd32(p + pos);
            pos += 4 + memo_len + AC_SIG_SIZE;
            if (remaining < pos) { ac_block_free(out); return -1; }
            int tn = ac_tx_decode(&out->txs[i], p, pos);
            if (tn < 0) { ac_block_free(out); return -1; }
            c.pos += pos;
        }
    }

    /* Commit. */
    if (c.cap - c.pos < 4) { ac_block_free(out); return -1; }
    uint32_t nsigners = rget_u32(&c);
    if (nsigners > AC_COMMITTEE_MAX) { ac_block_free(out); return -1; }
    if (nsigners > 0) {
        out->signers = (ac_commit_signer_t *)calloc(nsigners, sizeof(ac_commit_signer_t));
        if (!out->signers) { ac_block_free(out); return -1; }
        out->nsigners = nsigners;
        for (uint32_t i = 0; i < nsigners; ++i) {
            rget(&c, out->signers[i].signer.b, AC_PUBKEY_SIZE);
            rget(&c, out->signers[i].sig.b,    AC_SIG_SIZE);
            rget(&c, out->signers[i].vrf_proof, AC_VRF_PROOF_SIZE);
        }
    }

    if (c.err) { ac_block_free(out); return -1; }
    return (int)c.pos;
}

void ac_block_free(ac_block_t *b) {
    if (!b) return;
    if (b->txs) { free(b->txs); b->txs = NULL; }
    if (b->signers) { free(b->signers); b->signers = NULL; }
    b->tx_count = b->nsigners = 0;
}
