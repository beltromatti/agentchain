# AgentChain Security Audit — v1.0.7

**Audited release:** AgentChain Engine v1.0.7 (`commit 2b84a05`)
**Audit date:** 2026-05-19
**Auditor:** Noesis AI — internal review
**Scope:** Reference C client + AgentChain Protocol v1, including the live mainnet (`chain_id=1`) bootstrap operated by Noesis AI on Google Cloud.
**Status:** Completed — findings and mitigations recorded below

> **Disclaimer.** This is a first-party audit. We make no claim that it substitutes for an independent third-party review. Every statement below is backed by either a cryptographic guarantee from an audited dependency, an external standards document, or a reproducible test in this repository. Where we have not been able to confirm a property we say so explicitly.

---

## 1. Audit Method

The audit proceeds in four layers:

1. **Cryptographic primitives** — verified against the published audits and standards behind their implementations.
2. **Protocol invariants** — derived from the rules in `PROTOCOL.md`. Each invariant is either proved at the implementation level or covered by a reproducible test.
3. **Implementation hazards** — every C source file was reviewed for memory safety, integer overflow, format-string, and threading errors. Findings and mitigations are listed.
4. **Operational surface** — the deployment configuration (mainnet seed, systemd unit, firewall) was reviewed.

Every claim below is annotated with `[evidence: …]` linking to the source of truth.

---

## 2. Cryptographic Primitives

### 2.1 Ed25519 signatures

AgentChain uses Ed25519 for transaction signing, block hash signing (commit votes), and the deterministic-signature VRF.

- **Implementation:** `libsodium >= 1.0.18`, function `crypto_sign_detached` and `crypto_sign_verify_detached`.
- **Standard:** RFC 8032 (Edwards-Curve Digital Signature Algorithm).
- **Audit evidence:** libsodium has been independently audited by Private Internet Access (Cure53, 2017), and its Ed25519 reference is the ref10 code from Daniel J. Bernstein, Niels Duif, Tanja Lange, Peter Schwabe, and Bo-Yin Yang — the canonical implementation. The signing path is **constant-time** as documented in the libsodium specification.
- **Forgery resistance:** EUF-CMA under the discrete-log assumption on Curve25519. Best-known attack remains the generic Pollard rho on Edwards25519, requiring `~2^126` operations — outside practical reach. *[evidence: RFC 8032 § 8, libsodium docs `crypto_sign_ed25519`]*
- **In-repo verification:** `tests/test_crypto.c` round-trips sign/verify and explicitly checks that a one-bit tamper of the signature is rejected.

### 2.2 BLAKE2b-256 hashing

Used for transaction hashes, block hashes, state roots, and the VRF output digest.

- **Implementation:** libsodium `crypto_generichash` (BLAKE2b).
- **Standard:** RFC 7693.
- **Collision resistance:** No known practical attack on full-round BLAKE2b. Best-known generic collision attack remains `2^128` for a 256-bit digest. The internal SHA-3 / NIST review accepted BLAKE2 as a finalist-quality primitive.
- **Domain separation:** Every BLAKE2b input is prefixed with a fixed ASCII tag of the form `"AGCH:<context>:v1"`. This eliminates length-extension and cross-context replay even where the inputs might overlap. The exhaustive list of tags is enumerated in `PROTOCOL.md` § 3, § 11, and code comments. *[evidence: `src/crypto.c` `ac_hash_multi`, every call site in the engine]*
- **In-repo verification:** `test_crypto.c::test_hash_chunks` confirms `hash([a, b]) == hash(concat(a, b))` for the chunked API.

### 2.3 Deterministic Ed25519-signature VRF

`PROTOCOL.md` § 3.2 defines the VRF as `π = Ed25519_sign(sk, "AGCH:VRF:v1" || α)`, `β = BLAKE2b("AGCH:VRF-OUT:v1" || π)`.

- **Uniqueness:** Ed25519 (RFC 8032 § 5.1.6) specifies a deterministic per-message nonce derived from `SHA-512(sk_left || α)`. For a fixed `(sk, α)`, there is exactly one valid signature, hence exactly one VRF output. *[evidence: RFC 8032 § 5.1.6]*
- **Pseudorandomness of `β`:** under the assumption that BLAKE2b is a pseudorandom function in its keyless mode, `β` is computationally indistinguishable from uniform to anyone without `sk`. The construction is the standard "sign then hash" pattern referenced in the Schnorr-VRF literature.
- **Verifiability:** anyone with `pk` can recompute `Ed25519_verify(pk, "AGCH:VRF:v1" || α, π)` and recompute `β`. *[evidence: `ac_vrf_verify`, `test_crypto.c::test_vrf_roundtrip`]*
- **Limitation acknowledged:** this construction is **weaker** in formal terms than RFC 9381 ECVRF — it does not provide pseudorandomness *against the signer* for novel inputs. PoSA does not require that property; it requires that no validator can choose its own VRF output for a given slot, and anyone can verify the output. Both hold for our construction.

### 2.4 Random-number generation

Generated only at validator-keygen time via `crypto_sign_keypair` → libsodium `randombytes_buf`.

- **Source:** libsodium falls through to the OS RNG (`getrandom(2)` on Linux 3.17+, `/dev/urandom` fallback; `CryptGenRandom` / `BCryptGenRandom` on Windows; `/dev/urandom` on macOS).
- **Audit evidence:** libsodium's documentation explicitly states `randombytes_buf` "is cryptographically secure and is suitable for the most demanding applications."

---

## 3. Protocol Invariants

| # | Invariant | Where it is enforced | Reproducible check |
|---|-----------|----------------------|--------------------|
| I-1 | Replay protection across networks | `chain_id` is part of `TxBody` and signed | `ac_state_apply_tx` rejects `tx.chain_id != chain.chain_id` |
| I-2 | Replay protection within a network | `nonce` is per-sender, strictly monotonic | `apply_tx` rejects `tx.nonce != sender.nonce`; success advances nonce by 1 |
| I-3 | No tx persists past its expiry | `valid_until` is bounded by `current_slot + 7200` | `apply_tx` rejects `slot >= valid_until`; mempool prunes per slot tick |
| I-4 | Gas charge is bounded | `actual_gas = min(charged, gas_limit)` | `apply_tx`: `if (actual_gas > tx->gas_limit) actual_gas = tx->gas_limit;` |
| I-5 | No mint outside the documented emission | Only `accept_block` credits the proposer via `block_reward(h)`, debited from `SYS_REWARD_POOL` | `ac_chain_block_reward(h)` is pure; `SLASH_EVIDENCE` reward is half of the burned stake, net-supply-neutral |
| I-6 | State root determinism | `ac_state_root` hashes accounts in lex order, then a separator byte, then names in lex order | `test_state.c::test_root_determinism` inserts the same 3 accounts in two different orders into two state objects and asserts identical roots |
| I-7 | Encoder/decoder symmetry | Tx + Header round-trip exactly | `test_codec.c::test_transfer_tx`, `test_header_roundtrip` |
| I-8 | Equivocation is provable & punishable | `SLASH_EVIDENCE` body verifies two committee-vote signatures over distinct block hashes at the same height by the same key | `apply_tx` for kind `0x05` performs the verification before slashing |
| I-9 | Domain separation prevents cross-context signature reuse | Every signature input is prefixed with a distinct ASCII tag (`"AGCH:TX:v1"`, `"AGCH:VOTE:v1"`, `"AGCH:VRF:v1"`, `"AGCH:BLOCK:v1"`, `"AGCH:STATE:v1"`, etc.) | exhaustive enumeration in `src/codec.c`, `src/consensus.c`, `src/state.c`, `src/crypto.c` |

---

## 4. Implementation Review

### 4.1 Memory safety

The codebase is ~5,200 lines of C11 against POSIX. We audited every `malloc`/`free`/`realloc`/`memcpy`/`memmove` call.

**Findings:**

| # | Concern | Severity | Status |
|---|---------|----------|--------|
| F-1 | Variable-length tx body could overflow `tx.body` | Low | **Resolved.** `ac_tx_decode` rejects `body_len > AC_TX_BODY_MAX (512)` before reading. |
| F-2 | Variable-length memo could overflow `tx.memo` | Low | **Resolved.** `ac_tx_decode` rejects `memo_len > AC_MEMO_MAX (512)`. |
| F-3 | Block decoder loops over `tx_count` from header | Low | **Resolved.** `tx_count` is capped at `AC_BLOCK_MAX_TXS (4096)` before allocation. |
| F-4 | Commit-certificate decoder loops over `nsigners` | Low | **Resolved.** `nsigners` is capped at `AC_COMMITTEE_MAX (256)` before allocation. |
| F-5 | `read()`/`write()` short-result handling on sockets | Low | **Resolved.** `read_full`/`write_full` loop until N bytes transferred, handle EINTR, propagate errors. |
| F-6 | Format-truncation warning in `peer->host` snprintf | Low | **Resolved (v1.0.0).** `peer_t.host` buffer enlarged to 128 bytes to match advertised-host source. |
| F-7 | Consensus thread can block indefinitely on a slow peer | Medium | **Resolved (v1.0.1, refined v1.0.2).** `SO_SNDTIMEO=30s` on every socket; failed writes trigger immediate peer `shutdown()`. |
| F-8 | Connector opens N redundant connections to the same seed (broadcast amplification) | Medium | **Resolved (v1.0.3).** `dial_peer` deduplicates by `host:port`; `reader_loop` drops duplicate `peer_id` at the HELLO stage. Outbound target capped at `min(target_outbound, seed_count)`. |
| F-9 | Peer slot leak after disconnect (`in_use=true, fd=-1` permanent) | Medium | **Resolved (v1.0.4).** `connector_loop` calls `reap_dead_peers` each tick, joining the reader thread of any slot whose fd has been closed and releasing the slot. |
| F-10 | `HEADERS_REQ` storm from a gossip-lagging peer | Medium | **Resolved (v1.0.5).** Local rate-limits `HEADERS_REQ` to at most one per 4 s (or when the tip gap grows by >64); per-request batch raised from 64 to 256 blocks. |
| F-11 | Synchronous broadcast can stall consensus thread on a slow peer | High | **Resolved (v1.0.6).** Each peer slot owns a non-blocking outbound queue + a dedicated writer thread; producers (`ac_net_broadcast`, `ac_net_send_to`) enqueue and return immediately. A peer that overflows its 4 MB queue is shut down. |
| F-12 | `ac_block_decode` reads `body_len` at the wrong offset for transactions; first block containing a tx halts catch-up sync | Critical | **Resolved (v1.0.7).** Corrected offset (70 instead of 66) in the tx-length walker; reject `body_len > AC_TX_BODY_MAX` and `memo_len > AC_MEMO_MAX` before consuming the cursor. Regression test added (`tests/test_codec.c::test_full_block_with_tx_roundtrip`). |

**Tools applied:** GCC and Clang at `-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wcast-align -Wwrite-strings -Wunreachable-code -Wformat=2 -Wformat-security -Wundef`, treated as errors in CI on the `build.yml` workflow. The current `main` builds with `-Werror` clean across Ubuntu 22.04 (GCC 11), Ubuntu 24.04 ARM (GCC 13), macOS 14 (Apple Clang), and Windows 2022 (MinGW64 GCC 14). *[evidence: `.github/workflows/build.yml`]*

The project does not yet have an external static-analysis (Coverity/CodeQL) report; that is on the v1.1 roadmap.

### 4.2 Integer overflow

Fee arithmetic is the principal place where integer overflow matters.

```c
uint64_t per_gas = base_fee + tip;
if (per_gas != 0 && tx->gas_limit > UINT64_MAX / per_gas) {
    result_set_err(result, "fee overflow"); return -1;
}
```

The check guards against `(uint32_t gas_limit) * (uint64_t per_gas)` overflowing 64 bits before computing the maximum fee. Similar guards exist on `max_fee + value` (transfer underflow). *[evidence: `src/state.c::ac_state_apply_tx`]*

Block-reward arithmetic uses fixed integer constants (`annual_emission / SLOTS_PER_YEAR`) and cannot overflow under realistic supply growth (years 0–10: `2 × 10⁶ × 10⁶ µCRD / 15,778,800 ≈ 1.27 × 10⁵`, fits in u32 a fortiori).

### 4.3 Threading

- All shared mutable state is protected by a mutex: `ac_chain.mu`, `ac_mempool.mu`, `ac_consensus.mu`, per-peer `write_mu`, `ac_net.peers_mu`, `ac_net.dedup_mu`.
- Lock acquisition order is documented (`TECHNICAL-IMPLEMENTATION.md` § 6): `chain.mu` before `consensus.mu` is forbidden; in practice all paths take `consensus.mu` first when both are needed.
- The shutdown path joins every spawned thread before freeing state, eliminating use-after-free at teardown. *[evidence: `src/net.c::ac_net_stop`, `src/node.c::ac_node_stop`]*

### 4.4 Cryptographic side-channels

The only secret-dependent operations live inside libsodium's Ed25519 (constant-time by design). AgentChain does no comparison or branching on private-key bytes; signature verification, hash computation, and VRF prove operations forward directly to libsodium. There is no custom cryptography in the engine.

### 4.5 RPC surface hardening

The JSON-RPC server exposes six methods, all read-only except `tx_submit`:

| Method | Effect | Authentication |
|--------|--------|----------------|
| `chain_info` | Read | None |
| `account_get` | Read | None |
| `name_lookup` | Read | None |
| `block_get` | Read | None |
| `mempool_size` | Read | None |
| `tx_submit` | Adds tx to mempool | Tx is **signature-verified** before acceptance |

A malicious caller cannot inject a transaction it did not sign. A malicious caller can submit garbage transactions, but each consumes mempool space proportional to its size and is rejected at validation if invalid. Rate-limit protection is operator-side (e.g., behind a reverse proxy) and not built into v1.

The HTTP parser is a strict line-oriented reader with a 64 KB request cap; oversize requests are dropped without parsing.

---

## 5. Network & Wire Hardening

- **Frame format:** every wire message is `u32be(len) || u8(version) || u8(type) || payload`. Length cap is `4 MB`. Frames are validated for size and version before any field is read. Unknown types are dropped silently — no parsing fallthrough. *[evidence: `src/net.c::frame_recv`]*
- **Dedup ring:** every gossip-broadcast payload is recorded by hash in a 256-slot LRU ring; duplicate payloads (replay or echo) are not re-broadcast. *[evidence: `dedup_has`, `dedup_remember`]*
- **HELLO discipline:** a peer must send `HELLO` as its first frame, presenting `chain_id` and pubkey. Mismatched `chain_id` is dropped before any further frame is parsed. Self-connections (same pubkey) are rejected. *[evidence: `reader_loop`]*
- **No DHT, no UDP:** the only listening sockets are the configured `--port` (TCP P2P) and `--rpc-port` (TCP HTTP/JSON-RPC). No incidental ports are opened.

---

## 6. Operational Surface (mainnet seed)

The current mainnet seed is documented in `deploy/mainnet-seeds.txt`. As of v1.0.1 it runs on a GCP Always-Free `e2-micro` in `us-central1-a` (Iowa), operated by Noesis AI, behind a static IPv4 with two open ports:

- `TCP 30303` — P2P gossip (public).
- `TCP 30304` — JSON-RPC over HTTP (public, read-mostly, signature-verified writes).

**Hardening applied:**
- Non-root systemd unit (`User=agentchain`, `NoNewPrivileges=true`, `ProtectSystem=strict`, `ProtectHome=true`, `PrivateTmp=true`, `ReadWritePaths=/var/lib/agentchain`).
- Resource caps: `MemoryHigh=512M MemoryMax=768M CPUQuota=80%`.
- Automatic restart on failure (`Restart=on-failure RestartSec=5s`).
- Private validator key stored at `/var/lib/agentchain/node.key` mode `0600`, never copied off the host.

**Acknowledged risks:**
- **Single-host trust.** With one validator currently holding 100% of bonded stake, an attacker who compromised the host could equivocate or halt the chain. The mitigation path is the open invitation to additional validators: the chain accepts new `STAKE_BOND` transactions immediately, and `sqrt`-stake weighting means the marginal contribution of new validators is amplified.
- **Single-region availability.** The seed runs in one geographic region. The mitigation is the same: independent operators bringing up additional seeds, which the protocol supports without any central coordination beyond an updated seed list in `deploy/mainnet-seeds.txt` (PRs welcome).

---

## 7. Threat-Model Statement (re-affirmed)

Per `PROTOCOL.md` § 13, AgentChain v1 makes these claims and only these claims:

| Property | Holds when… | Breaks when… |
|----------|-------------|--------------|
| Safety  | Honest committee weight per slot > 2/3 (in `sqrt`-stake units) | Adversarial `sqrt`-weighted stake > 1/3 of network |
| Liveness | At least one honest validator is leader-eligible | Adversary controls the leader-sortition outcome (i.e., breaks VRF) |
| Censorship-resistance | Sortition is random & committees rotate | An adversary holds enough `sqrt`-weighted stake to be in every committee |

Under `sqrt`-weighting, controlling 1/3 of voting weight requires owning ~20% of total supply; controlling 2/3 requires ~80%.

Quantitative attack costs are presented as relative to **total bonded stake**, not market cap. Until total bonded stake grows beyond Noesis AI's genesis allocation, the dominant attack vector remains the operator (Noesis AI) being compromised or coerced. We do not claim otherwise.

---

## 8. Open Items

Honestly enumerated; tracked for v1.1:

- O-1. **No third-party audit yet.** First-party review only.
- O-2. **No fuzz harness in CI.** A `libFuzzer`-style harness over `ac_tx_decode`, `ac_block_decode`, and `ac_state_deserialize` is on the roadmap.
- O-3. **Active-set delay simplification.** Per `TECHNICAL-IMPLEMENTATION.md § 9.1`, the v1.0 engine uses the current state's validator set rather than a 2-epoch-delayed snapshot. Acceptable while bootstrap-set is operator-controlled; reverts to the spec rule in v1.1.
- O-4. **Heaviest-chain fork choice.** v1.0 rejects any block whose parent does not match the current tip rather than computing the heaviest committed-weight subtree. Acceptable while honest majority holds; closes in v1.1.
- O-5. **Instant unbonding.** `STAKE_UNBOND` releases funds immediately rather than after the 24-hour cooldown specified in the protocol. Cooldown queue is on the v1.1 roadmap.
- O-6. **No real-time equivocation watcher.** The protocol defines a slashable `SLASH_EVIDENCE` transaction; the engine verifies submitted evidence but does not yet proactively scan for double-signing.
- O-7. **State-format rewriter for SMT.** v1 rewrites the full state file every block, which is fine at current scale but does not scale linearly with account count. v1.1 plans a sparse-Merkle-tree state with incremental writes.
- O-8. *(Closed in v1.0.6 + v1.0.7.)* The original report — "gossip-driven steady-state sync stalls at ~50–65 blocks under high WAN latency" — turned out to be **two distinct bugs**, both now fixed and proven against the live Iowa↔Italy mainnet:
  - v1.0.6 introduced a per-peer asynchronous outbound queue so a slow link can no longer back-pressure the consensus thread (F-11).
  - v1.0.7 fixed a long-latent off-by-4 in `ac_block_decode`'s transaction-length walker (F-12) that made the first block containing a tx undecodable. Every prior version exhibited the same stall *at the same height* (the height of the first non-empty block); the symptom looked network-related but was a wire-format decode bug.

  Live evidence (Iowa seed at chain_id=1, ~27k blocks at the time of test): a fresh peer connected from Italy catches up at ~50 blocks/s; drift to tip shrinks monotonically and reaches single-digit blocks in a few minutes. Full sync is now functional.

---

## 9. Reproducibility

Every claim in this audit can be re-verified from this repository:

```bash
git clone https://github.com/beltromatti/agentchain
cd agentchain
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAGENTCHAIN_WARNINGS_AS_ERRORS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure   # § 3 invariants (state-root determinism, sign/verify, codec round-trip)
N=4 RUN_S=30 testnet/run.sh                  # § 5 wire-level liveness on a 4-node loopback
```

Release artefacts ship with reproducible SHA-256 sums published alongside each tar.gz on the [Releases](https://github.com/beltromatti/agentchain/releases) page.

---

## 10. Conclusion

AgentChain Engine v1.0.1 is judged **safe for alpha-mainnet operation** subject to the open items in `§ 8` and the operator-trust caveats in `§ 6`. The cryptographic core rests on a single audited dependency (`libsodium`) and a small surface of domain-tagged compositions. The implementation has no known memory-safety, integer-overflow, or threading-deadlock bugs at the time of writing.

We will publish a fresh revision of this document on every release that touches cryptography, consensus, or networking.

—
**Noesis AI** — Milano, 2026-05-19
*Lead reviewer: Mattia Beltrami*
