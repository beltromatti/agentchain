# AgentChain Engine — Technical Implementation Notes

This document describes how **AgentChain Engine v1.1.6**, the reference C client, realises the protocol specified in `PROTOCOL.md`. Read the protocol first; this document is a companion. Where the implementation deviates from the spec, the deviation is called out explicitly under `§ 9 Spec Deviations`.

---

## 1. Repository Layout

```
PROTOCOL.md                    # normative protocol specification
TECHNICAL-IMPLEMENTATION.md    # this file
README.md                      # project overview, install guide
LICENSE                        # Apache 2.0
CMakeLists.txt                 # single-binary build
.github/workflows/             # CI: build + multi-OS release on v* tags
deploy/                        # systemd unit, Dockerfile, docker-compose
testnet/run.sh                 # local multi-node testnet harness
tests/                         # unit tests run via ctest
src/                           # the engine, eleven modules + main
    common.{h,c}               # logging, hex, files, time, byte buffers
    crypto.{h,c}               # libsodium wrappers; Ed25519-VRF
    codec.{h,c}                # canonical Tx/Block serialisation
    state.{h,c}                # account map, name registry, state root
    chain.{h,c}                # block store, validation, fork choice, apply
    consensus.{h,c}            # PoSA sortition, slot loop, vote collection
    mempool.{h,c}              # ordered tx pool with fee market
    net.{h,c}                  # length-prefixed TCP gossip
    rpc.{h,c}                  # JSON-RPC 2.0 over HTTP
    node.{h,c}                 # process lifecycle, signal handling
    main.c                     # CLI entry point with subcommands
    version.h.in               # configure-substituted version header
```

There is no virtual machine. There are no precompiled libraries other than `libsodium` (system) and the C standard library.

---

## 2. Dependencies

| Dependency | Version | Why                                                            |
| ---------- | ------- | -------------------------------------------------------------- |
| libsodium  | ≥ 1.0.18 | Audited Ed25519 + BLAKE2b. The sole non-trivial third-party C dependency. |
| C compiler | C11      | `_Static_assert` and clean integer typing.                    |
| POSIX threads | —     | Single coarse mutex per subsystem; one thread per peer.       |
| CMake      | ≥ 3.16   | Cross-platform build.                                          |

The reference binary statically links its own modules into `libagentchain_engine.a` and dynamically links `libsodium`. Release artefacts produced by CI bundle a statically-linked `libsodium` for portability.

---

## 3. Mapping: Protocol → Source

The following table is the authoritative cross-reference. If a feature is in the protocol but not in this table, it is unimplemented in v1.1.6 (see `§ 9`).

| Protocol section                          | Implemented in                                       |
| ----------------------------------------- | ---------------------------------------------------- |
| § 3 Cryptography                          | `crypto.c` — `ac_crypto_init`, `ac_hash`, `ac_sign`, `ac_verify` |
| § 3.2 VRF                                 | `crypto.c` — `ac_vrf_prove`, `ac_vrf_verify`         |
| § 3.1 Addresses                           | `common.h` — `ac_addr_t` is a 32-byte struct         |
| § 4 Accounts & state                      | `state.c` — `ac_state_*`                             |
| § 4.2 State root                          | `state.c` — `ac_state_root`                          |
| § 5 Transactions                          | `codec.c` — `ac_tx_*`                                |
| § 5.2 Tx kinds                            | `state.c` — `ac_state_apply_tx` switch on kind       |
| § 5.3 Validity checks                     | `state.c` and `mempool.c` (pre-checks)               |
| § 5.4 Gas schedule                        | `codec.c` — `ac_tx_intrinsic_gas`, `ac_tx_total_gas` |
| § 6 PoSA blocks                           | `codec.c` (encoding) + `chain.c` (validation) + `consensus.c` (production) |
| § 6.2 Slot timing                         | `consensus.c` — `slot_loop`                          |
| § 6.3 Leader sortition                    | `consensus.c` — `am_i_leader`; `chain.c` — `validate_proposer_vrf` |
| § 6.4 Committee sortition                 | `consensus.c` — `am_i_committee`, `committee_eligible` |
| § 6.5 Commit rule                         | `consensus.c` — `try_commit`; `chain.c` — `validate_commit` |
| § 6.5.1 Vote convergence under multiple proposers | `consensus.c` — `vote_phase`, `compute_priority`, `slot_vote_for`, `our_vote_now` |
| § 6.6 Equivocation                        | `state.c` — `AC_TX_SLASH_EVIDENCE` handler           |
| § 7 Fork choice                           | `chain.c` — accept rule rejects non-extensions (v1 is single-tip; see `§ 9`) |
| § 8 Adaptive Equilibrium                  | `chain.c` — `ac_chain_block_reward`, `ac_chain_next_base_fee` |
| § 9 Agent-native primitives               | `codec.c` (memo field); `state.c` (names)            |
| § 10 Genesis                              | `chain.c` — `chain_apply_genesis`; `node.c` — `ac_node_load_genesis` |
| § 11 Wire formats                         | `codec.c`                                            |
| § 12 Network protocol                     | `net.c`                                              |
| § 13 Security model                       | `state.c` and `chain.c` — see `§ 5` below            |

---

## 4. Storage Layout on Disk

A running node stores everything under a single data directory:

```
<data_dir>/
    node.key          # 0600 — Ed25519 seed + public key
    meta.bin          # chain header (height, tip_hash, base_fee, …)
    state.bin         # full account/name snapshot at tip
    blocks/
        000000000000.blk   # genesis
        000000000001.blk
        …                  # one file per committed block
```

Every state-mutating operation writes via the atomic-rename idiom: `path.tmp` is opened, `fsync`'d, then `rename`'d over `path`. The containing directory is also `fsync`'d on macOS/Linux. A crash mid-write leaves either the old contents or the new — never partial.

The state snapshot is rewritten in full after every committed block. This is acceptable for the v1 throughput envelope (genesis state is ~50 entries; even a million accounts is ~50 MB to rewrite). Future versions may switch to a journaled sparse-Merkle-tree without changing any protocol-level format.

---

## 5. Subsystems

### 5.1 `common` — utilities

Provides the primitives every other module uses: `LOG_E/W/I/D` macros backed by a single-mutex stderr writer with ANSI colouring when stderr is a TTY; big-endian 8/16/32/64-bit pack/unpack inlines; hex encode/decode; constant-time memory comparison (`ac_memeq`); atomic file writer (`ac_file_write_atomic`); integer square root (`ac_isqrt_u64`) for stake weighting.

### 5.2 `crypto` — libsodium wrappers and the VRF

Ed25519 signing and verification call directly into libsodium. BLAKE2b-256 hashing is keyless and accepts a vector of `(buf, len)` chunks via `ac_hash_multi`, which is the only style used by domain-tagged hashes (every hash input is `domain_label || data`).

The VRF (`§ 3.2` of the protocol) is implemented as:

```c
proof  = ed25519_sign(sk, "AGCH:VRF:v1" || alpha)
output = blake2b("AGCH:VRF-OUT:v1" || proof)
```

Both ECVRF and this construction provide the four properties required for sortition (determinism, verifiability, pseudorandomness, uniqueness). The implementation deliberately reuses Ed25519's deterministic signature to avoid introducing a second cryptographic primitive.

### 5.3 `codec` — canonical wire formats

`ac_tx_encode` / `ac_tx_decode` and `ac_block_header_encode` / `ac_block_header_decode` produce the byte layouts defined in `PROTOCOL § 11`. They are pure functions with no allocation for transactions (max 2 KB on the stack) and small allocations only for full blocks (transactions array, signers array).

`ac_tx_hash` recomputes the canonical digest from the tx fields rather than over the wire bytes; this guarantees that a transaction's identity does not depend on which encoder produced it.

### 5.4 `state` — accounts, names, root, apply

Internally an `ac_state_t` is two sorted dynamic arrays — `accounts[]` ordered by address, `names[]` ordered by name — plus a `chain_id`. Lookups are binary searches; insertions are `memmove`'s. For the v1 expected size (tens of thousands of accounts), this is faster than a tree.

`ac_state_root` computes the protocol root by feeding `(domain || account[0].addr || account[0].balance | … || NAME_SEP_TAG || name[0].len | …)` into BLAKE2b in a single streaming pass.

`ac_state_apply_tx` is the only state-mutating entry point. It applies the protocol's transaction-validity and gas-charge rules. On a body failure after the static checks pass, the sender's nonce is still consumed and the gas charged, exactly matching `PROTOCOL § 5.3`.

### 5.5 `chain` — blocks, validation, apply, fork choice

`ac_chain_open` either loads an existing chain from `data_dir` or initialises one from a genesis configuration. Subsequent operations are serialised by a coarse `pthread_mutex_t`; consensus and network threads acquire it before mutating chain state.

`ac_chain_accept_block` is the engine's entry point for a fully-formed block. Its order of checks is:

1. Header version and shape.
2. Parent-hash and height continuation. (Forks at the same height are rejected if their hash differs from the recorded canonical block — see `§ 9 Spec Deviations`.)
3. Slot timing tolerance (`±3000 ms` of the slot's start time; up to `now + 1500 ms` accepted as "future-tolerant").
4. Proposer's VRF proof verifies against an active validator.
5. Base-fee follows the previous block's EIP-1559-style adjustment.
6. Every transaction's signature verifies.
7. Every transaction applies cleanly (or fails after consuming intrinsic gas).
8. `gas_used` matches the sum across the block.
9. `tx_root` matches the canonical concat-hash of transaction hashes.
10. Block reward + tips are credited to the leader, debited from the reward pool.
11. The resulting `state_root` matches the header's claim.
12. The commit certificate's cumulative `sqrt_stake` exceeds two-thirds of the network total.

If any check fails, state is rolled back to the pre-tx snapshot (taken at step 7) and the block is rejected. On success, the block file, state snapshot, and meta header are atomically persisted.

`ac_chain_build_block` performs the same flow speculatively to produce a proposal at the current slot: it temporarily applies the candidate transactions, computes the state root, then rolls back. The leader broadcasts the block; the authoritative apply happens via `accept_block` after the commit certificate is gathered.

`ac_chain_next_base_fee` implements the EIP-1559-style step: `delta = base × (gas_used - target) / target / 8`, floored at `MIN_BASE_FEE`. The factor `/8` caps each step at 12.5%.

`ac_chain_block_reward(h)` returns the deterministic emission for the block at height `h` per `PROTOCOL § 8.3`.

### 5.6 `consensus` — PoSA orchestration

A single thread (`slot_loop`) runs each slot in two phases:

1. **At slot start** — call `slot_routine(slot)`:
   - `am_i_leader(slot)` checks the node is an active validator, computes the VRF proof for `"AGCH:LEADER" || epoch_seed || slot`, and compares the VRF priority against a network-wide threshold targeted at ~2 candidates per slot.
   - If eligible, snapshot up to 256 transactions from the mempool, prune expired ones, call `ac_chain_build_block`, stash the proposal, and broadcast it. The proposal does **not** carry the proposer's own vote — the vote phase below handles that.
2. **At `slot_start + AC_VOTE_DELAY_MS` (900 ms)** — call `vote_phase(slot)`:
   - If the node is a committee member, scan the pending ring for proposals at `slot` and pick the one with the lowest leader priority (`§ 6.5.1` in `PROTOCOL.md`).
   - Sign exactly one `COMMIT_VOTE` for that proposal and broadcast it. `try_commit` aggregates the vote and calls `ac_chain_accept_block` once the cumulative `sqrt_stake` of signers exceeds two thirds.

Incoming proposals arrive via `ac_consensus_handle_block`: the routine validates the proposer's VRF, computes the proposer's leader priority, and stashes the block in an 8-slot pending ring. It does *not* vote immediately — the deferred vote phase is what gives every honest committee member the same proposal set to choose from, so they all converge on the lowest-priority block. Without that convergence step, two leader-eligible validators in the same slot could each accumulate ≥2/3 committee weight on distinct proposals.

When a node accepts a block locally, it rebroadcasts the fully-signed block over the network. This shortens convergence in the presence of message loss and lets peers behind by one block accept the newly-committed block directly (without rebuilding the certificate from individual votes).

When a node receives a proposal for a height beyond its current tip plus one, it issues a `HEADERS_REQ` over the network so peers can backfill the gap.

### 5.7 `mempool` — tx pool with fee-market ordering

Transactions are stored in a single sorted array (descending by `tip`). Insertion is O(n) — fine for the design's expected throughput. The pool dedupes by transaction hash and enforces nonce monotonicity per sender: a new tx with the same `(sender, nonce)` as an existing pool entry replaces it only if its tip is strictly higher (the standard "replace-by-fee" rule).

Capacity is `8192` entries. When full, the lowest-tip entry is evicted in favour of the incoming higher-tip one; if the incoming entry's tip is not strictly higher than the worst entry, it is rejected with `AC_MP_FULL_DROP_LOW_TIP`.

`ac_mempool_prune_expired(slot)` is called once per slot from the consensus thread.

### 5.8 `net` — TCP transport and gossip

Each outbound connection is dialled from a single connector thread (`connector_loop`); each accepted connection spawns a per-peer reader thread (`reader_loop`). Per-peer writes are serialised under a `write_mu`; reads are sequential by construction.

Every message is one frame: `u32 payload_len | u8 version | u8 type | payload[payload_len-2]`. On read, the type dispatches to a small switch in `dispatch_message` that performs hash-based dedup (for `BLOCK_ANN`, `TX_ANN`, `COMMIT_VOTE`) and either invokes a configured callback or re-gossips the message to other peers (skipping the originator).

The first message a peer must send is `HELLO`. It carries `chain_id`, the peer's pubkey, its advertised listen port, and an optional external host string. Mismatched `chain_id` or a self-connection are dropped before any further frame is read.

There is no DHT. Peer discovery is exactly: the configured seed list, plus what the gossip layer reveals over time. Mainnet seed lists are part of the release artefact (see `deploy/mainnet-seeds.txt`).

The reader/connector/listener threads cooperate during shutdown: `ac_net_stop` shuts down the listening socket, joins the listener and connector, then closes all peer sockets and joins every reader thread. After `ac_net_stop` returns, no callback can fire.

### 5.9 `rpc` — JSON-RPC 2.0 over HTTP

A minimal embedded HTTP server reads up to 64 KB of request, parses for `Content-Length` (case-insensitive), reads the body, and dispatches a single JSON-RPC method. Responses close the connection. No keep-alive in v1.

The JSON parser is hand-rolled and extracts top-level string and numeric fields by pattern match. It is sufficient for the limited request surface — `chain_info`, `account_get`, `name_lookup`, `tx_submit`, `mempool_size`, `block_get` — and rejects anything malformed.

The JSON-RPC methods exposed in v1:

| Method          | Parameters                                       | Returns                                                  |
| --------------- | ------------------------------------------------ | -------------------------------------------------------- |
| `chain_info`    | none                                             | `{chain_id, height, tip_hash, base_fee, genesis_timestamp_ms}` |
| `account_get`   | `{address: hex32}`                               | `{balance, nonce, stake, unbond_at}` (µCRD; zero if absent) |
| `name_lookup`   | `{name: string}`                                 | `{address: hex32}` or `null`                             |
| `tx_submit`     | `{tx_hex: string}`                               | `{hash: hex32}` on success; JSON-RPC error otherwise     |
| `mempool_size`  | none                                             | `{size: int}`                                            |
| `block_get`     | `{height: int}`                                  | Header summary fields                                    |

The RPC defaults to listening on `127.0.0.1:30304`. Exposing it over the network is operator-opt-in via `--rpc-host`.

### 5.10 `node` and `main` — lifecycle and CLI

`ac_node_start` initialises libsodium, loads or generates `node.key`, opens the chain (creating from genesis if necessary), initialises the mempool, wires the consensus broadcaster to `ac_net_broadcast`, wires the network callbacks to consensus and chain, then starts net, RPC, and the consensus slot loop. `ac_node_wait_for_signal` blocks until `SIGINT` or `SIGTERM`, after which the node tears down in reverse order: RPC, network (which joins every peer thread), consensus, mempool, chain.

`main.c` implements subcommands:

```
agentchain node       [--validator] [-v]
                      [--data-dir DIR] [--genesis FILE] [--seeds H:P,…]
                      [--port N] [--rpc-port N]
                      [--host BIND] [--rpc-host BIND] [--external-host H]
agentchain keygen     [--out FILE]
agentchain pubkey     [--key FILE | --data-dir DIR]
agentchain genesis    --chain-id N --out FILE [--timestamp-ms N]
                      [--account HEX:BAL:STAKE …]
agentchain send       --to HEX --amount UCRD
                      [--from-key FILE] [--rpc URL] [--tip N] [--memo TEXT] [--valid-slots N]
agentchain stake      --amount UCRD  [--from-key FILE] [--rpc URL] [--tip N] [--valid-slots N]
agentchain unbond     --amount UCRD  [--from-key FILE] [--rpc URL] [--tip N] [--valid-slots N]
agentchain balance    [--address HEX] [--rpc URL] [--key FILE]
agentchain info       [--rpc URL]
agentchain version
agentchain help
```

Defaults (defined in `src/mainnet.h`):

- `--rpc` → `https://api.agentchain.noesisai.it` (the Noesis-operated public RPC).
- `--from-key` / `--key` → `~/.agentchain/node.key`.
- `--data-dir` → `~/.agentchain`.
- `--genesis` → mainnet genesis embedded as a C string and materialised to `<data-dir>/genesis.txt` on first run when no file is present.
- `--seeds` → `34.61.207.49:30303` (the Iowa bootstrap seed).

Every default is overridable. Each subcommand supports `--help` with its own usage block and links to the relevant protocol section where v1.0.x behaviour deviates from the spec.

`send`, `stake`, and `unbond` share a common client-side path: each queries `chain_info` for `chain_id`, `account_get` for the sender's `nonce`, builds the body for the appropriate `kind` (`TRANSFER`, `STAKE_BOND`, `STAKE_UNBOND`), signs it, hex-encodes, and submits via `tx_submit`. A successful submission returns the transaction hash, and the RPC server gossips the transaction over `TX_ANN` so non-validator clients can still broadcast.

**HTTPS transport.** The CLI selects transport from the URL scheme: `http://` and bare `host:port` use a small native HTTP client; `https://…` shells out to the system `curl` with the request body piped via stdin (no URL or body bytes ever touch the shell, so injection is impossible). The daemon itself remains TLS-free — RPC is plain HTTP on the loopback interface, and any operator that wants a public endpoint pairs it with a reverse proxy (`§ 7.4`).

---

## 6. Threading Model

Threads owned by a running node:

1. **Main thread** — signal wait loop. No business logic.
2. **Consensus slot thread** — fires `slot_routine` once per slot.
3. **Network listener thread** — `accept()` loop.
4. **Network connector thread** — outbound seed dialer.
5. **N × peer reader thread** — one per active TCP connection.
6. **RPC accept thread** — `accept()` loop; each connection is handled inline (no per-request thread).

Shared mutable state is protected by:

- `ac_chain.mu` — covers chain state, account map, name map.
- `ac_mempool.mu` — covers the tx pool array.
- `ac_consensus.mu` — covers the pending-block ring.
- Per-peer `write_mu` — serialises `write(2)` to that peer.
- `ac_net.peers_mu` — covers peer-slot allocation.
- `ac_net.dedup_mu` — covers the gossip dedup ring.

Ordering rule: never hold `consensus.mu` and then `chain.mu`. Always acquire `chain.mu` first when both are needed. The codebase respects this.

---

## 7. Build and Release

### 7.1 Local

```sh
git clone https://github.com/beltromatti/agentchain
cd agentchain
sudo apt-get install -y libsodium-dev cmake build-essential   # Debian/Ubuntu
brew install libsodium cmake                                  # macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/agentchain version
```

### 7.2 Continuous integration

`.github/workflows/build.yml` runs on every push/PR. It builds the binary and runs the unit-test suite across `ubuntu-22.04`, `ubuntu-24.04-arm`, `macos-14`, and `windows-2022` (msys2). Failures upload the test logs as artefacts.

`.github/workflows/release.yml` runs when a `v*` tag is pushed. For each target it:

1. Builds in `Release` mode against a statically-linked libsodium (built in-CI).
2. Strips the binary.
3. Tars the binary, the seed list, the systemd unit, this file, and `PROTOCOL.md`.
4. Computes a SHA-256 digest of the tarball.
5. Uploads both the tarball and its `.sha256` companion as GitHub Release assets.

Target matrix:

| OS          | Architecture | Runner               | Toolchain                          |
| ----------- | ------------ | -------------------- | ---------------------------------- |
| Linux       | x86_64       | `ubuntu-22.04`       | GCC, glibc 2.35                    |
| Linux       | arm64        | `ubuntu-24.04-arm`   | GCC native arm64                   |
| macOS       | arm64        | `macos-14` (M-series)| AppleClang, native                 |
| macOS       | x86_64       | `macos-14` (M-series)| AppleClang, `-arch x86_64` cross   |
| Windows     | x86_64       | `windows-2022`       | msys2 / mingw-w64                  |

All five targets ship binaries on every `v*` release.

### 7.3 Deployment

`deploy/systemd/agentchain.service` runs the daemon as a non-root user, restarts on failure, and writes state to `/var/lib/agentchain`. `deploy/docker/Dockerfile` builds a multi-stage image weighing under 20 MB.

For mainnet, the seed list is shipped in `deploy/mainnet-seeds.txt`. Validators bring their own genesis via the official `genesis.txt` distributed in the release (or the copy embedded in the binary at `src/mainnet.h`).

### 7.4 Public RPC reverse-proxy topology

The Noesis-operated public RPC at `https://api.agentchain.noesisai.it` runs on the same Iowa `e2-micro` as the validator seed. It is layered:

```
   client ──HTTPS──▶  Caddy (Let's Encrypt cert) ──HTTP loopback──▶  agentchain
        ▲                    ▲                                            ▲
        │             nftables: 300/min burst 50 per IP                   │
        │                                                                 │
   DNS @ Cloudflare (DNS-only / grey cloud, A → 34.61.207.49)        --rpc-host 0.0.0.0
```

Why this shape:

- **DNS at Cloudflare, grey cloud**: free, fast, and gives us API-driven record management. Orange cloud (CF as proxy) would require Advanced Certificate Manager (`$10/mo`) for the 4-label hostname; on Free plan it provides only `*.zone` and `zone`. When the network outgrows the e2-micro we will revisit.
- **Caddy as origin proxy** ([`deploy/Caddyfile`](deploy/Caddyfile)): one-line `reverse_proxy 127.0.0.1:30304`, automatic Let's Encrypt cert via HTTP-01, 64 KB request body cap, method allowlist (`GET POST OPTIONS`), CORS for browser callers, gzip/zstd encoding, JSON access log rolled at 10 MB.
- **nftables rate limit** ([`deploy/nftables-agentchain-rl.conf`](deploy/nftables-agentchain-rl.conf)): kernel-level, dynamic set keyed on source IP, 300 new connections per minute with a burst of 50. Hits the wire before Caddy parses a single byte; in the first hour the rule dropped 60+ packets from automated `.env` / `.git/HEAD` scanners.
- **No write-side auth on the RPC**: the chain only accepts signature-verified transactions, so `tx_submit` is safe to leave public. An attacker can flood the mempool with valid but expensive transactions; rate-limit + mempool capacity (`8192` entries) bound that exposure.

This is exactly the architecture every other public RPC provider needs to operate — the daemon doesn't gain anything by knowing it's behind a proxy. To stand up a second provider (`api.foo.example`), copy [`deploy/Caddyfile`](deploy/Caddyfile), point a DNS record at your origin, and you're done.

---

## 8. Observability

`agentchain` writes structured logs to stderr in the format:

```
<rfc3339-ts> <LEVEL> [<module>] <message>
```

Modules: `node`, `chain`, `consen.`, `net`, `rpc`. Level controlled by `-v` (debug). No metrics endpoint in v1; operators are expected to read logs (or wrap them with their own scraping). A Prometheus exporter is on the v1.1 roadmap.

---

## 9. Spec Deviations in v1.0.x

These deviations are deliberate and documented. Each is annotated with the protocol section it relaxes and the reason the relaxation is acceptable for a bootstrapping network.

### 9.1 Active-set delay collapse

**Protocol § 6.3** specifies that the active validator set for epoch `e` is the set with `stake ≥ MIN_STAKE` at the end of epoch `e-2`. The v1.0.x reference client instead uses the active set as of the current chain tip. This shortens the time before a newly-bonded validator becomes eligible from two epochs (~8 hours) to zero, at the cost of a theoretical attack where an adversary times their bond to influence a single slot's leader sortition. The trade-off is acceptable for a bootstrap network whose initial validator set is controlled, and will be reverted in v1.1 once a stake-set snapshot per epoch is journaled. The validator-handover demonstration described in `§ 9.6` (Phases A→D) relies on this immediate-activation behaviour.

### 9.2 Single-tip fork choice

**Protocol § 7** describes a heaviest-chain fork-choice rule. v1.0.x simply rejects any block whose parent does not match the current tip. This means a node that diverges from the canonical chain by accepting a block that the rest of the network later abandons will need manual intervention (`rm -rf <data_dir>/blocks` and resync) to recover. In practice this does not happen because PoSA's vote-convergence rule (Protocol § 6.5.1, implemented in v1.0.11) guarantees safety even with multiple simultaneous proposers. The proper heaviest-chain implementation is on the v1.1 roadmap and will only matter under a Byzantine attack scenario.

### 9.3 Reward concentrated at the leader

**Protocol § 8.3** describes the per-block reward as paid to the slot's leader, with no committee share. The protocol's prior text described a 15/85 split. The current text and the implementation agree on the simpler leader-only rule; the change preserves a deterministic post-block state root knowable to the leader at proposal time.

### 9.4 Instant-release unbonding

**Protocol § 5.2** specifies a 24-hour cooldown for `STAKE_UNBOND`. The v1.0.x reference client instead instantly transfers from `stake` back to `balance` while recording the would-be unlock slot in `unbond_at`. Operators of v1 testnets do not lose security from this simplification; the cooldown enforcement is on the v1.1 roadmap.

### 9.5 Equivocation enforcement

**Protocol § 6.6** specifies that double-signing is slashable. v1.0.x implements the `SLASH_EVIDENCE` transaction, verifies the two signatures, and burns the validator's stake. What v1.0.x does **not** do automatically is detect equivocations in real time — a slasher must construct and submit the evidence transaction. A built-in equivocation watcher is on the v1.1 roadmap.

### 9.6 Patch-release history (v1.0.1 → v1.1.6)

The 1.0.x line has been amended several times since the initial mainnet launch. Each fix is recorded here for traceability; the corresponding security finding ID is in `SECURITY-AUDIT.md`.

1. **v1.0.1 → v1.0.5** — incremental sync ergonomics under WAN latency: `SO_SNDTIMEO` per socket (F-7), seed-dedup at `dial_peer`/HELLO stage (F-8), dead-peer reaping (F-9), `HEADERS_REQ` rate limit (F-10).
2. **v1.0.6** — `ac_net_broadcast` / `ac_net_send_to` moved to per-peer non-blocking write queues with a dedicated writer thread; producers no longer stall the consensus thread on slow peers (F-11).
3. **v1.0.7** — `ac_block_decode`'s tx-length walker read `body_len` at byte offset 66 instead of 70. Every block containing a transaction failed to deserialise, so `HEADERS_REQ` stalled at the first tx-bearing block. Offset corrected; regression covered by `tests/test_codec.c::test_full_block_with_tx_roundtrip` (F-12).
4. **v1.0.8** — `agentchain stake` and `agentchain unbond` promoted from raw-tx flows to first-class CLI subcommands (see `§ 5.10`). No protocol change.
5. **v1.0.9** — RPC `tx_submit` previously inserted into the mempool but did not gossip; a non-validator RPC client could submit a transaction that no validator ever saw. A `broadcast_tx` callback now wires the RPC into `ac_net_broadcast(TX_ANN, …)` (F-13).
6. **v1.0.10** — `validate_commit` previously evaluated the committee threshold against the *post-apply* validator set, so a block that itself added a validator (`STAKE_BOND`) could never meet the threshold. `ac_chain_accept_block` now snapshots the *pre-block* validator metrics (total `sqrt_stake` plus a per-signer stake lookup) and passes them through; the threshold is computed against the set that signed (F-14).
7. **v1.0.11** — committee members no longer vote immediately on the first proposal they see. They wait `AC_VOTE_DELAY_MS` (900 ms) after slot start, then sign exactly one `COMMIT_VOTE` for the proposal with the lowest VRF-derived leader priority. The change is normative on the spec side as well (`PROTOCOL.md § 6.5.1`). Without it, two leader-eligible validators in the same slot could each accumulate ≥2/3 committee weight on distinct proposals — the exact failure observed on mainnet at h=28699 under v1.0.10 (F-15).
8. **v1.0.12** — CLI UX overhaul. Every client command (`info`, `balance`, `send`, `stake`, `unbond`) defaults to `https://api.agentchain.noesisai.it` over HTTPS instead of `127.0.0.1:30304`. `--from-key` / `--key` defaults to `~/.agentchain/node.key`; `--data-dir` to `~/.agentchain`. The mainnet genesis is embedded in the binary (`src/mainnet.h`, byte-identical to `deploy/mainnet-genesis.txt`) and materialised on first run when no override is provided. HTTPS support is via a stdin-fed `curl` shell-out — the daemon's transport stays plain HTTP on the loopback interface, with the public-facing TLS terminated by the reverse proxy described in `§ 7.4`. Per-command `--help` was added, and `send/stake/unbond` now surface a human-readable summary (tx hash + RPC) instead of the raw JSON-RPC envelope. No protocol change.
9. **v1.0.13** — CI-gate cleanup: missing `<sys/wait.h>`/`<unistd.h>` includes for the v1.0.12 `https_post_curl` fork path, and `-Wformat-truncation` warnings on the `snprintf` calls that copy CLI flags into `ac_node_config_t` fields. Functionally identical to v1.0.12 (release artefacts there did not enable `-Werror`); only required to make the strict `build.yml` matrix green on Ubuntu 24.04 arm64.
10. **v1.0.14** — explorer-grade RPC surface (`tx_get`, `block_get_full`, `validators_list`, `accounts_top`, `peers_list`) and a chain-side in-memory tx-by-hash index rebuilt from `blocks/` at open and updated on every accept. CLI gained the explicit `bond` subcommand (alias for `stake`) for clarity in onboarding flows.
11. **v1.0.15** — `ac_isqrt_u64` previously returned `2^32` on inputs in `[(2^32-1)^2, 2^64-1]` because the Newton iteration overflowed at the boundary. With the v1.0.10 active-set scaling, this overflow silently inflated the total `sqrt_stake` of any large validator, breaking the leader-priority comparison in PoSA. Replaced with the textbook Newton-from-above method that stays in `[0, 2^32-1]` for every input.
12. **v1.1.0** — four interlocking changes shipped together on top of a clean mainnet alpha rebootstrap (`timestamp_ms=1779397200000`):
   1. **Live-set commit threshold.** `validate_commit` now compares signer weight against the *live* validator set (those that signed in the last `AC_LIVENESS_WINDOW=16` blocks), not the full bonded set. The previous behaviour froze the chain whenever a fresh validator bonded mid-sync: the threshold rose to include their stake, but they could not yet sign. The live-set rule is implemented in `chain.c` (`live_record_signer` on every accept, `ac_chain_live_sqrt_stake` queried by consensus) and rebuilt at open by replaying the last `AC_LIVENESS_WINDOW` blocks' signers.
   2. **Vote delay + seal grace.** `AC_VOTE_DELAY_MS` bumped 900 → 1100 ms; new `AC_SEAL_GRACE_MS=300` ms holds the seal open after the threshold is first reached so late `COMMIT_VOTE`s from high-latency validators land in the certificate. The slot loop is now four-phase: propose, vote (+1100 ms), seal (+1400 ms), sleep. Stockholm→Iowa votes that previously missed the seal under v1.0.11 now make it in.
   3. **HELLO v2 with peer-list gossip.** The HELLO frame now carries an optional peer-list trailer (cap=`AC_HELLO_PEER_MAX`=8). A new joiner that dials any one of the foundation seeds learns about the rest via gossip and dials them, so the topology stops being hub-and-spoke. v1.0.x HELLOs without the trailer are accepted unchanged.
   4. **`address_txs` RPC.** Backed by an in-memory address tx-history index (built on accept, rebuilt at open, parallel to the tx-by-hash index). The website explorer's address view uses it to show inbound + outbound transactions on the same panel that already shows balance + stake, mirroring the wallet's History tab.
   `mainnet.h` now lists three foundation seed peers as equals (Iowa/Frankfurt/Stockholm). CLI/help strings are relabelled "mainnet" → "mainnet alpha"; `chain_id` is unchanged at `1`. The rebootstrap discharged a unit-error in the previous genesis (seed validator's balance off by 3 orders of magnitude relative to the protocol's 100M total supply) and rotated the website faucet hot wallet (10,000 CRD funded from the seed validator post-launch).
13. **v1.1.1** — catch-up sync throttle. The v1.1.0 `AC_SEAL_GRACE_MS` window was applied unconditionally in `try_commit_inner`, including to fully-certified blocks received via `HEADERS_REQ` during sync. A syncing node would see the 2/3 threshold already met on an arriving block but wait the 300 ms grace before sealing — and since `seal_phase` only flushes pending blocks whose slot equals the *current* wall slot (never true for historical sync blocks), each block effectively sealed only when a duplicate re-broadcast re-triggered `try_commit`. Net effect: catch-up crawled at roughly one block per slot tick, slower than the chain produces, so a node that fell behind never converged. Fix: the grace is for late `COMMIT_VOTE`s during *live* consensus; during sync the block is already past and carries a quorum cert, so when the block's slot is more than one slot behind the wall slot (`catching_up`), seal immediately. A late joiner now drains the whole `HEADERS_REQ` burst in one pass.
14. **v1.1.2 / v1.1.3** — duplicate-peer eviction. `parse_hello`'s duplicate-`peer_id` guard kept the *first* connection and dropped the new one. That is backwards for the dominant real case — a peer presents a duplicate id because it just restarted while we still hold its previous slot, whose reader is parked in a blocking `recv()` (no RCV timeout) and won't observe the death until a write fails up to `SO_SNDTIMEO` (30 s) later. Until then every broadcast was shovelled into the dead socket and the reconnected peer received nothing: on mainnet alpha this stranded whichever validator last restarted (connected on both ends, listed in `validators_list`, but hundreds of blocks behind and earning nothing). **v1.1.2** flipped the rule to newest-wins — on a duplicate `peer_id`, shut the stale slot's fd and let the fresh connection take over. **v1.1.3** completed it: shutting the fd does *not* set `fd=-1` (only the reader does, on exit), and both `ac_net_broadcast` and `ac_net_send_to` route by `(peer_id_known && fd>=0)` with `send_to` stopping at the first match — so a lingering lower-index stale slot still swallowed the `HEADERS_REQ` *response* (sent via `send_to`) while broadcasts (sent to all) got through, leaving the peer stuck a few blocks short of the head. The eviction now also clears `peer_id_known` on the stale slot, making it instantly invisible to all routing before the reader is reaped. Restarting the *seed* used to mask the whole class of bug by clearing its slot table, which is why snapshot/reboot recovery looked like a fix while the network stayed two-of-three.
15. **v1.1.4** — `peers_list` reports the responding node itself, not just its dialled peers. The handler now emits a `node` object (`ac_net_self_info`: own pubkey + advertised host/port) alongside the `peers` array, so a client sees the full network the node participates in (self + peers) rather than an off-by-one view. On the hub seed this is the difference between reporting "2 connected" and the true three-node network. Purely additive to the JSON (`connected` and `peers` are unchanged); older clients ignore `node`. Foundation nodes now also pass `--external-host <public-ip>` so the advertised endpoint (in both HELLO and the `node` entry) is the real address rather than the `0.0.0.0` bind.
16. **v1.1.5** — split-brain fork fix (consensus **safety**, not just liveness). The v1.1.0 live-set threshold computes the commit denominator from only the validators that signed recently. Under a transient network partition each side's live set shrinks to what it can still see, so both sides can independently clear `2/3 of (their) live` and finalise *different* blocks at the same height. This happened on mainnet alpha at **h=82033**: a partition left the Iowa seed finalising one block alone (its live set had collapsed to just itself) while the Frankfurt+Stockholm pair finalised another, and with no automatic reorg (`§ 9.2`) the chain wedged — every node stuck, none able to re-reach quorum on its own branch. Fix: `validate_commit` (and the mirror in `consensus.c try_commit`) now require, in addition to `≥ 2/3 of the live set`, that signers exceed **1/2 of the *total* bonded sqrt-stake**. Two disjoint signer sets cannot both exceed half the total (quorum intersection), so at most one block can finalise per height regardless of partitions — the live-set's no-freeze benefit is kept for honest connected quorums, but a minority can no longer fork the chain. Both gates are consensus-critical and identical on every node, so all validators must run ≥ v1.1.5; a mixed network could split on the differing rule. Recovery from the h=82033 wedge: the minority branch (seed-only) was discarded and the seed resynced onto the majority branch (the Frankfurt+Stockholm pair, which carried the larger combined sqrt-stake) before the rolling v1.1.5 deploy.
17. **v1.1.6** — per-height vote lock (consensus **safety**, completes the v1.1.5 fork fix). The v1.1.5 floor stops two *disjoint* quorums from each finalising a block at one height, but it does not stop the *same* quorum from voting for two different blocks at one height across consecutive slots. The commit vote was tracked per **slot** (`slot_vote_t`), not per **height**: when a block failed to seal within its slot (votes delayed past the seal window) and the leader proposed a fresh block for the *same* height in the next slot, every committee member dutifully voted for both — so both blocks gathered the full quorum and finalised, splitting the tip. This recurred on mainnet alpha at **h=316036** and **h=335861**, each time leaving the seed and the Frankfurt validator one block apart with no reorg to heal it. Fix: the consensus state now records `(vote_height, vote_block)` — the height and block of our most recent commit vote — and `vote_phase` refuses to vote for any *different* block at `vote_height`. When a conflicting proposal appears at an already-voted height, the node re-broadcasts its original vote (via the now-idempotent `our_vote_now`, which will not double-count the signer) so the block it first backed still reaches the threshold and seals. Normal operation is unaffected — each height is voted exactly once, so the lock never triggers on the happy path (verified by the 3-node smoke and dropout/rejoin tests). Recovery from each wedge was a surgical one-block resync (the fork was always exactly one block deep, both blocks empty): copy the canonical node's block file + `meta.bin` + `state.bin` for the contested height onto the minority node and restart, far cheaper than a full snapshot. Like v1.1.5 this is consensus-critical — all validators must run ≥ v1.1.6.

End-to-end verification of v1.0.11 ran on the live mainnet on 2026-05-19 with two validators (an Iowa seed and a residential macOS host). The chain transitioned through four phases — both validators active, seed alone, both alive again, validator alone — without forks, without missed slots, and with the expected `signers={1,2}` shape on every commit. The associated mainnet `chain_id=1` history starts at the v1.0.11 genesis (`timestamp_ms=1779201674000`); the pre-v1.0.11 chain was abandoned during the coordinated reset described in the v1.0.11 release notes. Read + write through the public HTTPS RPC was verified on 2026-05-20 alongside the v1.0.13 deployment (chain_info → 200; `send` from a remote wallet → tx mined → recipient balance +1 µCRD; sender nonce 2 → 3).

### 9.7 Bootstrap genesis simplification

**Protocol § 10** specifies a four-bucket 100M CRD distribution: a 60M validator reward pool, a 20M ecosystem-and-grants pool, a 12M foundation vest, and 8M across the initial validator set. The currently-deployed mainnet genesis at `deploy/mainnet-genesis.txt` ships a single allocation of ~40M CRD (39,999,999.8 balance + 200 stake) to the bootstrap validator. This is deliberately below the protocol's intended supply and uses a one-account structure pending the formal multi-bucket distribution: the foundation vesting account, ecosystem multisig, and reward pool's non-spendable system account are not yet implemented in v1.0.x state-apply logic. Migrating the genesis to the protocol-specified shape is part of the v1.1 work; until then, the deployed total supply and account topology are operator state, not normative.

---

## 10. Performance Envelope (v1.0.x)

These are measured numbers from the local 4-node testnet (Apple M-series, single host). They are not extrapolations.

| Metric                                | Value                                    |
| ------------------------------------- | ---------------------------------------- |
| Time-to-first-block from genesis      | < 2 s                                    |
| Steady-state block interval            | 2 s (matches `SLOT_DURATION_MS`)         |
| 2-block (irreversible) finality        | ~ 4 s                                    |
| Transfer transaction RPC submit → seen in mempool | < 50 ms                  |
| Transfer transaction submit → committed | ~ 2–4 s (one to two slots)              |
| Node memory (RSS) at idle              | ~ 8 MB                                   |
| Binary size (stripped, x86_64 Linux)  | < 200 KB without libsodium static; < 700 KB statically linked |

These numbers will move once stake-set snapshots, heaviest-chain fork choice, and a real cross-WAN deployment are in place. They are reported here so future versions have something to compare against.

---

## 11. Testing

`testnet/run.sh` spins up `N` (default 4) validator nodes on this host, watches their heights via the JSON-RPC, submits a transfer, verifies the recipient's balance, and tears the cluster down. It is the gating check for every change.

```sh
N=4 RUN_S=30 testnet/run.sh
```

The script generates fresh keys, writes a genesis allocating 1000 CRD + 200 CRD stake to each, connects every node to every other, and reports per-node heights every three seconds. A v1.0.0 success criterion is: all `N` nodes converge to the same height by the end of `RUN_S` seconds.

Unit tests live under `tests/`. They cover the canonical encoders (round-trip), the state-root determinism (golden values), and gas-charging edge cases. The CI runs them via `ctest`.

---

## 12. Roadmap (non-binding)

This is what we plan to build on top of v1 before the protocol's hard-fork lever is pulled. None of it is committed to a date.

- **v1.1 — Robustness**: active-set delay, heaviest-chain fork choice, equivocation watcher, instant 24-hour unbonding queue, Prometheus exporter.
- **v1.2 — Agent ergonomics**: WebSocket subscriptions, `fee_forecast` RPC, agent-friendly batched RPC.
- **v2 — Deterministic execution**: a minimal, gas-metered, agent-native execution environment. Backwards-compatible at the wire level. No EVM compatibility.

The protocol is intentionally underspecified beyond v1.0.0 — getting v1 deployed and observed in the wild has priority over committing to architectures we have not yet stress-tested.
