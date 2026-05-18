/* AgentChain Engine — canonical wire formats (PROTOCOL § 11).
 *
 * All multi-byte integers are big-endian. Variable-length byte fields are
 * prefixed by a u32 length. The functions in this module are the *only*
 * source of truth for what a transaction or a block looks like on disk
 * and on the wire.
 */

#ifndef AGENTCHAIN_CODEC_H
#define AGENTCHAIN_CODEC_H

#include "common.h"
#include "crypto.h"

/* -------------------------------------------------------------------------- */
/* Hard limits.                                                               */
/* -------------------------------------------------------------------------- */

#define AC_TX_VERSION         1
#define AC_BLOCK_VERSION      1

#define AC_MEMO_MAX           512
#define AC_TX_BODY_MAX        512       /* slash evidence ~232 B; rest <= 40 B */
#define AC_TX_MAX_BYTES       2048      /* hard cap for a single encoded tx    */
#define AC_BLOCK_MAX_TXS      4096
#define AC_BLOCK_GAS_LIMIT    30000000ULL
#define AC_MIN_BASE_FEE       1ULL      /* µCRD/gas */
#define AC_NAME_MIN_LEN       1
#define AC_NAME_MAX_LEN       32
#define AC_COMMITTEE_TARGET   64
#define AC_COMMITTEE_MAX      256       /* hard cap on actual signers per block */
#define AC_SLOT_DURATION_MS   2000
#define AC_EPOCH_SLOTS        7200ULL   /* ~4 hours */
#define AC_MIN_STAKE_UCRD     (100ULL * 1000000ULL) /* 100 CRD */

/* -------------------------------------------------------------------------- */
/* Transactions.                                                              */
/* -------------------------------------------------------------------------- */

typedef enum {
    AC_TX_TRANSFER       = 0x01,
    AC_TX_STAKE_BOND     = 0x02,
    AC_TX_STAKE_UNBOND   = 0x03,
    AC_TX_REGISTER_NAME  = 0x04,
    AC_TX_SLASH_EVIDENCE = 0x05,
} ac_tx_kind_t;

typedef struct {
    uint8_t   version;
    uint64_t  chain_id;
    uint8_t   kind;
    ac_addr_t sender;
    uint64_t  nonce;
    uint32_t  gas_limit;
    uint64_t  tip;          /* µCRD/gas */
    uint64_t  valid_until;  /* slot     */
    uint32_t  body_len;
    uint8_t   body[AC_TX_BODY_MAX];
    uint32_t  memo_len;
    uint8_t   memo[AC_MEMO_MAX];
    ac_sig_t  sig;
} ac_tx_t;

/* Returns intrinsic gas cost for the kind, per PROTOCOL § 5.4. -1 = unknown. */
int      ac_tx_intrinsic_gas(uint8_t kind);

/* Returns the gas a tx burns at minimum if accepted: intrinsic + per-byte. */
uint64_t ac_tx_total_gas(const ac_tx_t *tx);

/* Encode/decode. Returns bytes written or -1 on error. */
int      ac_tx_encode(uint8_t *out, size_t out_cap, const ac_tx_t *tx);
int      ac_tx_decode(ac_tx_t *out, const uint8_t *buf, size_t len);

/* Bytes the signature covers: TxBody (no signature). */
int      ac_tx_signable_bytes(uint8_t *out, size_t out_cap, const ac_tx_t *tx);

/* Computes the canonical tx hash (used for tx_root, mempool index, RPC). */
void     ac_tx_hash(ac_hash_t *out, const ac_tx_t *tx);

/* Signs (in place) a tx whose every field but `sig` is set. */
int      ac_tx_sign(ac_tx_t *tx, const ac_keypair_t *kp);

/* Verifies the signature against tx->sender. Returns 1/0. */
int      ac_tx_verify(const ac_tx_t *tx);

/* -------------------------------------------------------------------------- */
/* Body shapes for each kind (helpers; the canonical form is `tx.body`).      */
/* -------------------------------------------------------------------------- */

typedef struct { ac_addr_t recipient; uint64_t amount; } ac_body_transfer_t;
typedef struct { uint64_t  amount;                     } ac_body_stake_t;
typedef struct { uint8_t   name[AC_NAME_MAX_LEN]; uint8_t name_len; } ac_body_name_t;

typedef struct {
    ac_addr_t  validator;
    uint64_t   height;
    ac_hash_t  block_hash_a;
    ac_sig_t   sig_a;
    ac_hash_t  block_hash_b;
    ac_sig_t   sig_b;
} ac_body_slash_t;

int ac_body_transfer_encode(uint8_t *out, size_t cap, const ac_body_transfer_t *b);
int ac_body_transfer_decode(ac_body_transfer_t *out, const uint8_t *buf, size_t len);
int ac_body_stake_encode(uint8_t *out, size_t cap, const ac_body_stake_t *b);
int ac_body_stake_decode(ac_body_stake_t *out, const uint8_t *buf, size_t len);
int ac_body_name_encode(uint8_t *out, size_t cap, const ac_body_name_t *b);
int ac_body_name_decode(ac_body_name_t *out, const uint8_t *buf, size_t len);
int ac_body_slash_encode(uint8_t *out, size_t cap, const ac_body_slash_t *b);
int ac_body_slash_decode(ac_body_slash_t *out, const uint8_t *buf, size_t len);

/* Returns 1 iff `name_len` bytes form a valid name per § 5.2 (REGISTER_NAME). */
int ac_name_valid(const uint8_t *name, size_t name_len);

/* -------------------------------------------------------------------------- */
/* Blocks.                                                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t   version;
    uint64_t  height;
    uint64_t  slot;
    ac_hash_t parent_hash;
    uint64_t  timestamp_ms;
    ac_addr_t proposer;
    uint8_t   proposer_vrf_proof[AC_VRF_PROOF_SIZE];
    ac_hash_t state_root;
    ac_hash_t tx_root;
    uint64_t  base_fee;
    uint64_t  gas_used;
    uint64_t  gas_limit;
    uint32_t  tx_count;
} ac_block_header_t;

typedef struct {
    ac_addr_t              signer;
    ac_sig_t               sig;        /* over "AGCH:VOTE:v1" || u64(height) || block_hash */
    uint8_t                vrf_proof[AC_VRF_PROOF_SIZE];
} ac_commit_signer_t;

typedef struct {
    ac_block_header_t      header;
    /* Transactions owned by the caller via tx_arr + tx_count. */
    ac_tx_t              *txs;          /* may be NULL when tx_count == 0 */
    uint32_t              tx_count;
    /* Commit certificate. */
    ac_commit_signer_t   *signers;      /* parallel array, length = nsigners */
    uint32_t              nsigners;
} ac_block_t;

/* Encode/decode header alone. Returns bytes written or -1 on error. */
int  ac_block_header_encode(uint8_t *out, size_t cap, const ac_block_header_t *h);
int  ac_block_header_decode(ac_block_header_t *out, const uint8_t *buf, size_t len);

/* Compute the block hash from the header (PROTOCOL § 6.1). */
void ac_block_hash(ac_hash_t *out, const ac_block_header_t *h);

/* Compute the tx_root: BLAKE2b-256 over concatenation of tx hashes in order. */
void ac_block_tx_root(ac_hash_t *out, const ac_tx_t *txs, uint32_t tx_count);

/* Build the bytes signed by a commit vote: "AGCH:VOTE:v1" || u64(height) || hash. */
int  ac_vote_message(uint8_t out[64], uint64_t height, const ac_hash_t *block_hash);

/* Encode/decode an entire block (PROTOCOL § 11.3). Allocations:
 *   - decode allocates tx and signer arrays; caller must call ac_block_free.
 *   - decode returns the number of bytes consumed; -1 on error.
 */
int  ac_block_encode(uint8_t **out, size_t *out_len, const ac_block_t *b);
int  ac_block_decode(ac_block_t *out, const uint8_t *buf, size_t len);
void ac_block_free(ac_block_t *b);

#endif /* AGENTCHAIN_CODEC_H */
