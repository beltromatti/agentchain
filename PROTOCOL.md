# AgentChain Protocol Specification

**Version:** 1.0.0
**Status:** Stable
**Author:** Noesis AI — Mattia Beltrami (Politecnico di Milano)
**License:** Apache-2.0

---

## 1. Abstract

AgentChain is a Layer-1 blockchain designed for two constituencies that current chains serve poorly:

1. **Home validators** on commodity hardware with intermittent uptime.
2. **AI agents** that need a deterministic, low-friction settlement layer for autonomous economic actions.

The protocol commits to four properties:

- **Light.** A full node runs in under 256 MB of RAM and ships as a single statically-linked binary under 5 MB.
- **Honest about decentralisation.** Stake influences validator selection through a square-root curve, not linearly. Doubling capital does not double voting power.
- **Forgiving of downtime.** Validators are not slashed for being offline. They are slashed only for cryptographically-provable misbehaviour (double signing).
- **Agent-native.** Transactions carry structured memos. Finality is deterministic and fast. The RPC surface is designed for non-human callers.

The native asset is **Credit**, abbreviated **CRD**. Total genesis supply is 100,000,000 CRD. The smallest unit is **1 micro-Credit** (`µCRD = 10⁻⁶ CRD`); all on-chain arithmetic is integer.

The consensus mechanism is called **Proof of Sustained Availability (PoSA)**. It combines well-understood primitives — VRF-based sortition (Algorand), square-root vote weighting (public-choice literature), single-round BFT commits, and EIP-1559-style fee burning — into a system tuned for the constituencies above. No claim is made that any individual component is novel. The contribution is the synthesis and its constraints.

---

## 2. Design Philosophy

AgentChain optimises for a small set of constraints, in priority order. When constraints conflict, the lower-numbered constraint wins.

1. **Safety beats liveness.** A halted chain is recoverable. A forked chain that finalised conflicting state is not. PoSA prefers to skip a slot rather than commit under ambiguity.
2. **Simplicity beats throughput.** The protocol surface fits in this document. There is no virtual machine in v1. There are no precompiles. Every byte of state and every byte on the wire is documented.
3. **Plurality beats efficiency.** A network of 10,000 small validators is preferred to a network of 100 large ones, even at a throughput cost. The economic model is calibrated against this constraint.
4. **Determinism beats flexibility.** A block produces the same state root on any honest implementation, byte for byte. There are no floating-point operations anywhere in the consensus path.

### 2.1 What AgentChain is not

AgentChain v1 is not a smart contract platform. It does not execute Turing-complete code. It settles value transfers and validator-set operations and nothing else. This is a deliberate scoping choice for v1: it is easier to add a constrained, deterministic execution layer to a small, audited base than to retrofit safety into a large, expressive one. The roadmap for v2 contemplates a deterministic, gas-metered, agent-friendly execution environment; v1 ships the foundation.

### 2.2 Why pure C

The reference client, **AgentChain Engine**, is written in C11 against POSIX. The reasons are operational, not stylistic:

- Static linkage produces a single artifact with predictable behaviour across distributions.
- The runtime surface is small enough to audit end-to-end.
- Memory and CPU footprint are deterministic; no garbage collector pauses interfere with consensus.
- Dependencies are limited to audited, standards-track libraries — `libsodium` for cryptography and the POSIX sockets API for networking. State is persisted as plain files with atomic rename; no database engine is linked.

The implementation is described in `TECHNICAL-IMPLEMENTATION.md`.

---

## 3. Cryptographic Primitives

| Purpose                | Primitive                                          | Source              |
| ---------------------- | -------------------------------------------------- | ------------------- |
| Digital signatures     | Ed25519                                            | RFC 8032, libsodium |
| Verifiable randomness  | Deterministic Ed25519-signature VRF (`§ 3.2`)      | this spec           |
| Cryptographic hash     | BLAKE2b-256                                        | RFC 7693, libsodium |
| Key derivation         | BLAKE2b-256 over labelled domains                  | this spec           |

All hashes in this document are BLAKE2b-256 unless stated otherwise. All signatures are Ed25519. Domain separation is applied to every hash input by prefixing a UTF-8 label of the form `"AGCH:<purpose>"` followed by a single `0x00` separator byte; this prevents cross-protocol signature reuse.

### 3.1 Addresses

An **address** is the 32-byte Ed25519 public key of an account. There is no second-level address scheme (no Base58, no Bech32 layer at the protocol level — clients may present addresses in any encoding, but the wire format is the raw 32-byte key). Equality of addresses is byte-equality of public keys.

### 3.2 Verifiable Random Function

PoSA depends on a VRF for unforgeable, publicly-verifiable, per-slot randomness. The construction is deliberately simple: it reuses Ed25519's deterministic signing rule (RFC 8032 § 5.1.6) without introducing any new cryptographic primitive.

```
α       (input)  := domain-tagged byte string
π       (proof)  := Ed25519_sign(sk, "AGCH:VRF:v1" || α)        // 64 bytes
β       (output) := BLAKE2b-256("AGCH:VRF-OUT:v1" || π)         // 32 bytes
verify(pk, α, π) := Ed25519_verify(pk, "AGCH:VRF:v1" || α, π)
```

This construction satisfies the four properties PoSA requires:

- **Determinism.** Ed25519 specifies a deterministic per-message nonce; a single (sk, α) yields a single (π, β).
- **Verifiability.** Anyone holding `pk` can confirm that `π` is the unique signature over `α`.
- **Pseudorandomness.** `β` is the BLAKE2b digest of a signature on a domain-tagged input; an observer without `sk` cannot distinguish it from a uniform 32-byte string.
- **Uniqueness.** Because Ed25519 signatures are deterministic, no two valid `π` exist for the same (sk, α); the VRF output is unique.

The construction is weaker than RFC 9381 ECVRF in formal terms (it does not give pseudorandomness *against the signer* for novel inputs, only verifiability and uniqueness), but those formal properties are not required by PoSA. What PoSA requires is that no validator can choose its own VRF output for a given slot, and that anyone can verify the output. Both are met.

The byte-tag `"AGCH:VRF:v1"` ensures these signatures are domain-separated from regular transaction signatures and cannot be cross-replayed.

---

## 4. Accounts and State

AgentChain uses the **account model** (not UTXO). The global state at the end of each block is a mapping `Address → AccountState` plus a small chain-level header. The state has no representation independent of the chain that produced it; the **state root** committed in each block header is a deterministic hash of this mapping.

### 4.1 Account state

```
AccountState {
    balance:   uint64   // available, transferable, in µCRD
    nonce:     uint64   // strictly-monotonic transaction counter
    stake:     uint64   // bonded for validator duties, in µCRD
    unbond_at: uint64   // slot at which `stake` becomes withdrawable (0 if not unbonding)
}
```

An account is **implicit**: it exists if its address has appeared in any committed transaction or has non-zero state. Empty accounts are not stored. The `nonce` of a non-existent account is implicitly zero.

### 4.2 State root

The state root is the BLAKE2b-256 hash of the canonical serialisation of all non-empty accounts, sorted lexicographically by address. The serialisation format is fixed (`§ 11.1`); a single bit-flip anywhere in state changes the root. The state root is committed in every block header (`§ 6.1`) and is the basis for the **fork-choice rule** (`§ 7`).

In v1 the state is small enough that recomputing the root is acceptable (at the order of microseconds per block for realistic state sizes). A future version may switch to a Sparse Merkle Tree without protocol change beyond a header version bump.

---

## 5. Transactions

A transaction is a signed authorisation to mutate state. Every transaction has the same envelope; the `kind` field selects what mutation it expresses.

### 5.1 Transaction envelope

```
Transaction {
    version:     uint8         // 1
    chain_id:    uint64        // mandatory replay protection
    kind:        uint8         // see § 5.2
    sender:      Address       // 32 bytes; account paying fees, providing nonce, authorising
    nonce:       uint64        // must equal sender.nonce
    gas_limit:   uint32        // upper bound on gas (§ 5.4); rejected if exceeded
    tip:         uint64        // µCRD/gas paid to validators on top of base fee
    valid_until: uint64        // slot number after which the tx is invalid; max sender_now + 7200 (~4h)
    body:        bytes         // kind-specific payload
    memo:        bytes         // 0..512 bytes, opaque, signed but not interpreted (§ 9.1)
    signature:   bytes[64]     // Ed25519(SignableBytes, sender_sk)
}
```

`SignableBytes` is the deterministic canonical serialisation of every field above `signature`, prefixed by the domain label `"AGCH:TX:v1"`. The full byte schema is in `§ 11.2`.

### 5.2 Transaction kinds (v1)

| `kind` | Name             | Body                                                  | Effect                                                         |
| ------ | ---------------- | ----------------------------------------------------- | -------------------------------------------------------------- |
| `0x01` | `TRANSFER`       | `{ recipient: Address, amount: uint64 }`              | Move `amount` µCRD from `sender.balance` to `recipient.balance`. |
| `0x02` | `STAKE_BOND`     | `{ amount: uint64 }`                                  | Move `amount` from `sender.balance` to `sender.stake`. Activates validator role. |
| `0x03` | `STAKE_UNBOND`   | `{ amount: uint64 }`                                  | Begin unbonding `amount` from `sender.stake`. Returns to `balance` after the cooldown window (`§ 6.4`). |
| `0x04` | `REGISTER_NAME`  | `{ name: bytes (1..32, [a-z0-9-]) }`                  | Bind a human-readable name to `sender`. Names are first-come; one name per account; one account per name. Costs `NAME_FEE` (`§ 8.4`). |
| `0x05` | `SLASH_EVIDENCE` | `{ evidence: EquivocationProof }`                     | Report a double-sign (`§ 6.6`). Slashing executes atomically; 50% of slashed stake to reporter. |

Future kinds are reserved. Unknown kinds are rejected by validators (no soft-fork ambiguity in v1).

### 5.3 Validity conditions

For a transaction to be eligible for inclusion:

1. `chain_id` equals the network's `chain_id`.
2. `version` equals 1.
3. `kind` is recognised.
4. `signature` verifies against `sender`.
5. `sender` exists in state and `sender.nonce == nonce`.
6. `valid_until > current_slot` and `valid_until ≤ current_slot + 7200`.
7. `gas_limit` is at least the kind's intrinsic gas (`§ 5.4`) and at most the block's remaining gas.
8. `sender.balance ≥ gas_limit × (base_fee + tip) + transfer-amount-if-any`.

The transaction is **executed** atomically. If execution fails after these checks (e.g. due to a `TRANSFER` that would underflow), state is rolled back to the pre-transaction snapshot but `nonce` is still incremented and the **paid gas is consumed** — this prevents cheap nonce-grinding attacks. The actual gas charged is `min(actual_used, gas_limit)`.

### 5.4 Gas schedule (v1)

| Operation                              | Gas |
| -------------------------------------- | --- |
| Base cost (any tx)                     | 21  |
| `TRANSFER`                             | +0  |
| `STAKE_BOND` / `STAKE_UNBOND`          | +30 |
| `REGISTER_NAME`                        | +200 |
| `SLASH_EVIDENCE`                       | +500 |
| Per memo byte                          | +1  |
| New account write (recipient empty)    | +50 |

The schedule is intentionally minimal. There are no dynamic costs; gas of any v1 transaction is a function of its bytes and kind, knowable client-side without simulation.

---

## 6. Blocks and Consensus: Proof of Sustained Availability

PoSA is a Byzantine-fault-tolerant consensus that runs in fixed-duration slots. In every slot a leader is selected by VRF lottery, and a committee of validators is selected by an independent VRF lottery. The leader produces a block; the committee votes. A block is **committed** when it accumulates more than two thirds of the slot's committee voting weight.

### 6.1 Block structure

```
BlockHeader {
    version:           uint8          // 1
    height:            uint64         // genesis = 0
    slot:              uint64
    parent_hash:       bytes[32]      // hash of parent BlockHeader
    timestamp_ms:      uint64         // proposer's wall clock at proposal time
    proposer:          Address
    proposer_vrf_proof: bytes[64]     // VRF proof over (epoch_seed || slot), § 3.2
    state_root:        bytes[32]      // post-execution state root (§ 4.2)
    tx_root:           bytes[32]      // BLAKE2b-256 over canonical concat of tx hashes
    base_fee:          uint64         // µCRD/gas charged on this block
    gas_used:          uint64
    gas_limit:         uint64         // 30,000,000 in v1
    tx_count:          uint32
}

Block {
    header:        BlockHeader
    transactions:  Transaction[tx_count]
    commit:        CommitCertificate
}

CommitCertificate {
    signers:       Address[]          // committee members who signed; ascending by address
    signatures:    bytes[64][]        // Ed25519 signatures, parallel to signers
    vrf_proofs:    bytes[64][]        // each signer's committee-eligibility proof
}
```

The block hash is `BLAKE2b-256("AGCH:BLOCK:v1" || canonical(BlockHeader))`. The commit certificate is **not** part of the block hash; the same block content can be supplemented with additional committee signatures after initial commit, and the cumulative weight is what determines fork choice (`§ 7`).

### 6.2 Slot timing

- `SLOT_DURATION = 2 seconds` (network wall time, no consensus on clocks beyond bounded drift).
- A validator considers a slot "current" if `now ∈ [slot_start - 500ms, slot_start + 4s)`. The 500ms tolerance front-loads clock skew; the 4s tail allows a block from this slot to still be relayed and committed in the next slot's interval.
- Block timestamps are checked: `|block.timestamp_ms - slot_start_ms| ≤ 3000`. Blocks outside the window are rejected.

### 6.3 Leader sortition

For slot `s` belonging to epoch `e`, every active validator `v` computes:

```
α        = "AGCH:LEADER" || epoch_seed[e] || u64_be(s)
(π, β)   = VRF(sk_v, α)
priority = blake2b256("AGCH:PRIO" || β) / sqrt_stake(v)
```

where `sqrt_stake(v) = floor(sqrt(v.stake))` and the division is interpreted as fixed-point comparison: lower `priority` ranks higher. Among validators that produce a proof, the one with the **lowest priority** is the canonical leader. In practice each validator broadcasts only if its priority is below a dynamic threshold targeting one or two proposers per slot; the threshold is recalibrated each epoch (`§ 8.6`).

The `epoch_seed[e]` for epoch `e ≥ 1` is `blake2b256("AGCH:SEED" || header_at(epoch_first_slot - 1).proposer_vrf_proof)`. Epoch 0 uses the genesis seed (`§ 10`). Epochs are 7,200 slots long (~4 hours).

A validator is **active** in epoch `e` if it had `stake ≥ MIN_STAKE` at the snapshot taken at the last block of epoch `e-2`. This two-epoch delay ensures every validator agrees on the active set for a given epoch without consulting live state.

`MIN_STAKE = 100 CRD = 100,000,000 µCRD`.

### 6.4 Committee sortition

Independently from leader sortition, each active validator computes:

```
α       = "AGCH:COMMITTEE" || epoch_seed[e] || u64_be(s)
(π, β)  = VRF(sk_v, α)
draw    = blake2b256("AGCH:DRAW" || β) interpreted as Q.256 in [0, 1)
weight  = sqrt_stake(v)
eligible if draw < (weight / total_sqrt_stake) × COMMITTEE_TARGET
```

`COMMITTEE_TARGET = 64`. The expected committee size per slot is 64 by construction, with binomial variance. The committee's actual voting weight is the sum of `sqrt_stake` of members who actually sign — this is the quantity used for the 2/3 commit threshold.

The committee is allowed to be a strict subset of the validator set; this is the mechanism that keeps PoSA light at scale. Even with 10,000 validators, only ~64 produce per-slot signatures.

### 6.5 Commit rule

A block at slot `s` is **committed** in the view of a validator when its `CommitCertificate` reaches:

```
Σ sqrt_stake(signer) > (2/3) × Σ sqrt_stake(committee_v_at_slot_s)
```

where the denominator is the **expected** committee weight (the total of all eligible-this-slot weights, computable by anyone with the active set and the epoch seed). This is a *strict* two-thirds rule. Because signers must prove committee eligibility via their VRF proof, a non-member cannot pretend to be a committee voter.

Once committed, a block is irrevocably part of one validator's local view. Finality across the network is *probabilistic in time but deterministic in commitments*: with the committee's honest threshold above 2/3, no two committed blocks at the same height can be produced by a non-equivocating committee. Equivocation, if it occurs, is detectable (`§ 6.6`).

In practice, applications treat a block as final once one further block has been committed on top of it (k = 2 block confirmations, ~4 seconds).

### 6.6 Equivocation and slashing

A validator that signs two different blocks at the same height has produced an **equivocation proof**. Both signatures can be packaged in a `SLASH_EVIDENCE` transaction (`§ 5.2`). On inclusion:

- The offender's entire `stake` is destroyed.
- 50% of the destroyed stake is minted to the reporter (`sender` of `SLASH_EVIDENCE`).
- The offender's validator role is terminated; further blocks signed by that key are inadmissible.

No other behaviours are slashable in v1. In particular, a validator that goes offline, produces no blocks, or misses every committee duty is **never penalised** beyond losing potential rewards. This is the central concession to home validators.

### 6.7 Validity of a block

A block is valid iff every condition holds:

1. `version == 1`.
2. `height == parent.height + 1` and `parent_hash` matches.
3. `slot > parent.slot` and the leader sortition proof verifies for `proposer` at this slot.
4. `timestamp_ms` is within tolerance (`§ 6.2`).
5. Every transaction is individually valid (`§ 5.3`) and was applied in the order listed.
6. `state_root` matches post-execution state.
7. `tx_root` matches the canonical concatenation of transaction hashes.
8. `gas_used ≤ gas_limit` and `base_fee` follows the adjustment rule (`§ 8.2`).
9. The `CommitCertificate` reaches the 2/3 threshold and every signature/VRF proof is valid.

A node rejecting any block on any of these grounds simply drops it; there is no fault attribution beyond equivocation.

---

## 7. Fork Choice and Finality

When a validator observes more than one committed block at the same height, it has detected a fork. The fork-choice rule is:

> **Prefer the chain whose committed-weight sum is greater. Break ties by lower last-block-hash.**

Concretely, a chain is summarised by the cumulative `Σ sqrt_stake(signer)` across every block's commit certificate from genesis to tip. The greater value wins. This converges to a single chain as soon as a supermajority of stake aligns; equivocation evidence guarantees the misbehaving stake is slashed and removed from the next epoch's active set.

A block is **final** under this rule once any further block has been committed on top of it. There is no separate finality gadget in v1.

---

## 8. Economic Model: Adaptive Equilibrium

The economic design has three goals, in this order:

1. **No single point of capital failure.** No validator's loss of access to private keys, no individual whale's exit, no exchange's outage causes the chain to halt or to behave anomalously.
2. **Predictable cost.** A transaction submitted today costs roughly what an equivalent transaction will cost tomorrow, barring sustained congestion shifts.
3. **Bounded inflation.** Net issuance asymptotically trends to zero as usage grows, without requiring discretionary parameter changes.

### 8.1 Supply

- **Genesis:** 100,000,000 CRD (100 million).
- **Issuance schedule:** A fixed-rate emission stream of approximately **2% of genesis per year for the first decade**, totalling 20 million CRD released as block rewards. The per-block emission is computed deterministically (`§ 8.3`), not from a smooth curve, so that no implementation drift can produce different supply.
- After year 10, emission steps down to **0.5% of genesis per year** in perpetuity (a small, predictable security floor).
- No discretionary minting exists. No address has the authority to create CRD outside this schedule and outside `SLASH_EVIDENCE` rewards (which are paid from destroyed stake, net-neutral to supply).

### 8.2 Fee market

Each block sets a `base_fee` (µCRD per gas) that is **burned** when paid. Transactions also include a `tip` (µCRD per gas) paid directly to validators.

The base fee adjusts every block according to:

```
target_gas = gas_limit / 2          // 15M gas/block
delta      = base_fee × (gas_used - target_gas) / target_gas / 8
base_fee'  = max(MIN_BASE_FEE, base_fee + delta)
```

`MIN_BASE_FEE = 1 µCRD/gas`. The factor `/8` caps adjustments at 12.5% per block, matching EIP-1559's well-studied stability properties.

Burning the base fee creates the **adaptive equilibrium**: every transaction destroys CRD proportional to the network's congestion. Over time, supply growth from issuance is offset by base-fee burn. At equilibrium the chain is asymptotically supply-neutral.

A `TRANSFER` of arbitrary amount on an uncongested network costs `21 gas × 1 µCRD = 21 µCRD ≈ 0.000021 CRD`. At a CRD price of $1, that is $0.000021 per transfer.

### 8.3 Block rewards

For block at height `h`:

```
slots_per_year   = 31,557,600 / 2 = 15,778,800
year             = h / slots_per_year
if year < 10:
    annual_emission = 2,000,000 CRD                  // ~2% of genesis
else:
    annual_emission = 500,000 CRD                    // ~0.5% of genesis
reward_per_block = annual_emission / slots_per_year  // in µCRD, integer arithmetic
```

The reward is paid in full to the **leader** of the slot. Tips (`tip × gas_used` for every transaction in the block) are credited to the leader in the same atomic state transition.

The decision to concentrate the per-block reward at the leader, rather than splitting it with committee voters, is a v1 simplification: it lets the leader compute a deterministic post-block `state_root` without knowing which validators will end up signing the commit certificate. The incentive to participate in committees survives because leader sortition and committee sortition draw from the same active set — committee members earn proportionally over time as their VRF priorities select them as leaders. A future protocol version may switch to a deferred, signer-proportional reward (paid in block `N+1` from the certificate of block `N`), once the engineering cost of two-block reward accounting is justified by measured validator-participation rates.

### 8.4 Other fees

- **`NAME_FEE`**: `1 CRD = 1,000,000 µCRD`, burned. Discourages name squatting at scale without making names a luxury.

### 8.5 Inflation reasoning

In year 1, at zero congestion: issuance is 2M CRD; burn is ~0; net inflation 2%. At 1 tx/s average network-wide (~31.5M tx/year) and current `base_fee` of `MIN_BASE_FEE`, the burn is ~660 CRD/year — negligible. At 1 tx/s average with `base_fee = 100 µCRD/gas` (congestion-driven), burn ~66,000 CRD/year — still smaller than issuance. Net inflation under all v1 traffic regimes stays in the 1.8–2.0% range. This is deliberately modest: the chain prioritises validator-set growth over near-term scarcity.

### 8.6 Threshold calibration

The dynamic threshold for leader sortition (`§ 6.3`) targets one or two leaders per slot regardless of validator-set size. It is recomputed at the start of each epoch as:

```
threshold[e] = 2 × MAX_VRF_OUTPUT / |active_set[e]|   (capped to 1)
```

With 10 validators the threshold is high (any of them is likely a leader). With 10,000 validators the threshold is low (only ~2 broadcast per slot). The mechanism scales linearly in validator-set size.

---

## 9. Agent-Native Primitives

AgentChain commits to a small set of features that exist specifically because autonomous agents — not humans — are the expected long-tail user. These are normative parts of v1, not aspirations.

### 9.1 Signed memo field

Every transaction has a 0..512-byte `memo` field that is signed and committed but not interpreted by the protocol. The encoding is unspecified at the protocol layer; agents are expected to use **CBOR** (RFC 8949) by convention so that structured intent (action label, correlation ID, deadline) is parsable across implementations. Memos are NOT free: each byte costs 1 gas (`§ 5.4`).

### 9.2 Named accounts

`REGISTER_NAME` (`§ 5.2`) lets an account claim a human/agent-readable identifier from `[a-z0-9-]{1,32}`. The mapping `name → address` is part of the state. Two agents can reliably address each other via `pay(@treasury, 1.0)` rather than 32 bytes of hex. Names cannot be transferred in v1; they are bound to the registering account for life.

### 9.3 Deterministic finality
A block reaches 2-confirmation finality in ~4 seconds. An agent that submitted a transaction and observes its inclusion plus one further block can treat the action as settled. The RPC exposes `subscribe_finality(tx_hash)` to make this a single asynchronous wait.

### 9.4 Batched RPC and predictive fees

The reference RPC (`TECHNICAL-IMPLEMENTATION.md § RPC`) accepts batched method arrays in a single request. It exposes `fee_forecast(n)` returning the projected base-fee envelope for the next `n` blocks (bounded by EIP-1559's adjustment caps). Agents planning bursts of transactions can budget deterministically.

### 9.5 Identity is first-class

An "agent" in AgentChain is simply an account. There is no distinct identity primitive in v1. The roadmap contemplates an optional v2 extension that lets accounts attest to non-transferable claims (operator, jurisdiction), but the protocol commits to keeping identity orthogonal to consensus — never a gating factor.

---

## 10. Genesis

The genesis block has `height = 0`, `parent_hash = 0x00…00`, `slot = 0`, and a fixed `epoch_seed[0]` of `BLAKE2b-256("AGCH:GENESIS:v1" || chain_id || genesis_timestamp_ms)`.

Genesis distributes the 100M CRD initial supply as follows:

| Allocation               | Amount        | Vesting                                          |
| ------------------------ | ------------- | ------------------------------------------------ |
| Validator reward pool    | 60,000,000    | Released as block rewards over 10+ years (`§ 8.3`). Held in a non-spendable system account. |
| Ecosystem & grants pool  | 20,000,000    | Held in a multi-signature account; spends are public state transitions. |
| Foundation (Noesis AI)   | 12,000,000    | 4-year linear vest, 1-year cliff. Operational and audit funding. |
| Genesis validators       | 8,000,000     | Distributed across the initial bootstrap set. Bonded as `stake` at genesis. |

Genesis validators, their pubkeys, their initial stakes, and the seed peers are recorded in `genesis.json` of each network. The `chain_id` distinguishes mainnet (`1`) from devnet (`2025`) and any local nets (`> 1000000`).

Mainnet `chain_id` is **1**.

---

## 11. Wire Formats

All multi-byte integers are **big-endian**. All variable-length fields are prefixed with a `uint32` byte length (`u32be`). Boolean fields are single bytes.

### 11.1 Canonical state serialisation

```
StateAccounts := for each account in lexicographic order by address:
                    address || u64be(balance) || u64be(nonce) || u64be(stake) || u64be(unbond_at)

StateNames    := u8(0xFF)                                    // separator tag
              || for each name in lexicographic byte-order:
                    u8(len) || name_bytes || address          // 32-byte address

state_root     := BLAKE2b-256("AGCH:STATE:v1" || StateAccounts || StateNames)
```

Empty accounts (all zero fields) are omitted. If no names are registered, `StateNames` is `u8(0xFF)` alone. The separator tag prevents an attacker crafting an `address` field that ends with a length byte that would mis-parse as a name entry.

### 11.2 Canonical transaction serialisation

```
TxBody := u8(version)
       || u64be(chain_id)
       || u8(kind)
       || sender                  // 32 bytes
       || u64be(nonce)
       || u32be(gas_limit)
       || u64be(tip)
       || u64be(valid_until)
       || u32be(len(body))   || body
       || u32be(len(memo))   || memo

TxSign := BLAKE2b-256("AGCH:TX:v1" || TxBody)
signature := Ed25519_sign(sender_sk, TxSign)

TxWire := TxBody || signature      // 64 trailing bytes
```

The `body` byte schema for each `kind` is given in `TECHNICAL-IMPLEMENTATION.md § Encoding`.

### 11.3 Canonical block serialisation

```
BlockHeaderBytes := u8(version)
                 || u64be(height) || u64be(slot)
                 || parent_hash                              // 32 bytes
                 || u64be(timestamp_ms)
                 || proposer                                 // 32 bytes
                 || proposer_vrf_proof                       // 64 bytes
                 || state_root || tx_root                    // 32 each
                 || u64be(base_fee) || u64be(gas_used) || u64be(gas_limit)
                 || u32be(tx_count)

block_hash := BLAKE2b-256("AGCH:BLOCK:v1" || BlockHeaderBytes)

BlockWire := BlockHeaderBytes
          || concat(TxWire for each tx in order)
          || u32be(len(signers))
          || concat(signer_i || sig_i || vrf_proof_i for each signer)
```

Signers are listed in ascending byte-order of address for canonical encoding of the commit certificate.

---

## 12. Network Protocol

AgentChain nodes communicate over **TCP**, length-prefixed framing, one logical message per frame. The wire is opaque and version-tagged; nodes that don't recognise a frame's type drop it without disconnecting.

```
Frame := u32be(payload_len) || u8(version) || u8(type) || payload[payload_len - 2]
```

Message types in v1 (full list in `TECHNICAL-IMPLEMENTATION.md`):

| Type | Name           | Notes                                                  |
| ---- | -------------- | ------------------------------------------------------ |
| 0x01 | `HELLO`        | Peer identity, chain_id, known-peers gossip.           |
| 0x02 | `HEADERS_REQ`  | Range request.                                         |
| 0x03 | `HEADERS_RES`  | Response.                                              |
| 0x04 | `BLOCK_REQ`    | Full block by hash.                                    |
| 0x05 | `BLOCK_RES`    | Full block.                                            |
| 0x06 | `BLOCK_ANN`    | Announce a newly-produced block (gossip).              |
| 0x07 | `TX_ANN`       | Announce a transaction (gossip).                       |
| 0x08 | `COMMIT_VOTE`  | Committee-member commit signature (gossip).            |
| 0x09 | `STATE_SNAP_REQ` | Snapshot request for fast-sync.                      |
| 0x0A | `STATE_SNAP`   | Snapshot response (chunked).                           |
| 0x0B | `PING` / `PONG`| RTT measurement.                                       |

Peer discovery is by **seed list** plus gossip exchange in `HELLO`. Each node maintains a target of 8 outbound and unlimited inbound peers (rate-limited per IP). A node behind NAT can run as an inbound-only peer; the protocol does not require accepting connections.

There is no DHT, no Kademlia, and no UDP discovery. The decision is operational: NAT and UDP do not coexist well, and a chain that needs to be cheap to run on a residential connection is better served by a small, explicit seed set plus gossip.

---

## 13. Security Model

The protocol is honest about its threat model. The claims it makes are:

| Property               | Holds when                                                     | Breaks when                                                    |
| ---------------------- | -------------------------------------------------------------- | -------------------------------------------------------------- |
| Safety (no two finalised conflicting blocks at the same height) | Honest committee weight per slot > 2/3.            | Adversarial **sqrt-weighted** stake exceeds 1/3 of the network. |
| Liveness               | At least one honest validator per slot is leader-eligible.     | Adversary controls the leader sortition outcome (requires breaking VRF). |
| Censorship-resistance  | Sortition is random and the committee changes per slot.       | An adversary holds enough sqrt-weighted stake to be present in every committee. |

Because stake is `sqrt`-weighted, an attacker holding `X` of the total supply controls roughly `√X / (√X + √(1 - X))` of the voting weight, not `X`. To exceed 1/3 of voting weight, an attacker needs ~20% of total supply. To exceed 2/3, ~80%.

### 13.1 Non-claims

- The protocol does **not** claim to be unforgeable against sufficiently-resourced attackers (a >67%-supply attack succeeds against any stake-based system).
- The protocol does **not** claim to prevent a coalition of large stakers from colluding to censor a specific transaction. Square-root weighting raises the cost of this attack but does not eliminate it.
- The protocol does **not** assume synchronous clocks; it assumes partial synchrony with bounded drift (`§ 6.2`).
- The protocol does **not** guarantee that a node will see every block; it guarantees that a node that sees the canonical chain will compute the same state root that every other honest node sees.

### 13.2 What "secure by design" means here

It means: every behaviour that is **not** authorised by a valid signature and a state-transition rule documented in this file is rejected. There is no privileged account beyond what genesis allocates. There is no upgrade mechanism in v1 that can change the rules of consensus without a hard fork — meaning, a coordinated client-software release. The state machine is a function, not a configuration.

---

## 14. Versioning and Forward Compatibility

This specification is `version = 1`. Any change that alters the meaning of a byte on the wire, the validity of a transaction, the rule for state transition, or the rule for fork choice is a **breaking change** and requires a coordinated upgrade. Versioning is by hard fork; the protocol does not contain a soft-fork mechanism.

The v2 roadmap (non-binding) is documented in `TECHNICAL-IMPLEMENTATION.md § Roadmap`. The headline items are: a deterministic, gas-metered, agent-friendly execution layer; sparse-Merkle-tree state for light clients; and optional Schnorr aggregate signatures for committee commit certificates.

---

## 15. References

- Algorand: Chen & Micali, *Algorand: A Secure and Efficient Distributed Ledger*, 2017.
- Ed25519: Josefsson, Liusvaara, *Edwards-Curve Digital Signature Algorithm*, RFC 8032 (2017).
- BLAKE2: Saarinen, Aumasson, *The BLAKE2 Cryptographic Hash and Message Authentication Code*, RFC 7693 (2015).
- EIP-1559: Buterin et al., *Fee Market Change for ETH 1.0 Chain*, Ethereum Improvement Proposal, 2019.
- CBOR: Bormann, Hoffman, *Concise Binary Object Representation*, RFC 8949 (2020).
- Public-choice literature on square-root voting: Penrose, *The Elementary Statistics of Majority Voting*, JRSS 1946.

---

*This specification is the authoritative description of AgentChain v1. The reference implementation, AgentChain Engine, is documented in `TECHNICAL-IMPLEMENTATION.md`. Any discrepancy between the implementation and this document is a bug in the implementation.*
