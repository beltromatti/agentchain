<div align="center">

<img src="docs/assets/banner.svg" alt="AgentChain — Layer-1 in pure C" width="100%"/>

**Agent-native Layer-1 blockchain — written in pure C, live on the public internet.**

[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![CI](https://github.com/beltromatti/agentchain/actions/workflows/build.yml/badge.svg)](https://github.com/beltromatti/agentchain/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/beltromatti/agentchain?include_prereleases&sort=semver)](https://github.com/beltromatti/agentchain/releases)
[![Protocol](https://img.shields.io/badge/protocol-v1-informational)](PROTOCOL.md)
[![Mainnet](https://img.shields.io/badge/mainnet-live-brightgreen)](#mainnet-status)

[Protocol](PROTOCOL.md) · [Implementation notes](TECHNICAL-IMPLEMENTATION.md) · [Security audit](SECURITY-AUDIT.md) · [Releases](https://github.com/beltromatti/agentchain/releases)

</div>

---

## Why AgentChain exists

Two constituencies are poorly served by today's chains.

The first is anyone who would like to run a validator on a home computer, with a flaky residential connection, and not be punished for it. Existing proof-of-stake networks slash for downtime, optimise for datacenter-grade uptime, and concentrate voting weight in whichever entities can afford that uptime. The honest cost of decentralisation has been quietly priced out.

The second is the next generation of software — AI agents that act on behalf of people, settle commitments between themselves, and need a deterministic, low-friction venue to do so. The crypto rails we have today were built for humans clicking buttons, not for processes whose entire failure mode is unbounded retries against a busy mempool.

AgentChain is a single design that addresses both. It is a Layer-1 with:

- **A consensus tuned for participation, not capital.** Validator influence is `sqrt(stake)`, not `stake`. Doubling your bond does not double your vote. There is no slashing for going offline — only for cryptographically-provable double-signing.
- **A single-binary client under 1 MB.** Built in C11, depends only on `libsodium`, runs comfortably in 16 MB of RAM. Designed to sit on the same laptop someone is using for everything else.
- **A protocol that ships in one document.** [PROTOCOL.md](PROTOCOL.md) is the entire surface. There is no virtual machine in v1. There are no precompiles. Every byte on the wire is documented; every state transition fits on a page.

The native asset is **Credit (CRD)**. The smallest unit is the micro-Credit (`µCRD = 10⁻⁶ CRD`). All on-chain arithmetic is integer.

## Mainnet status

**AgentChain mainnet is live.**

| | |
| --- | --- |
| `chain_id` | `1` |
| **Public RPC** | `https://api.agentchain.noesisai.it` (HTTPS, JSON-RPC 2.0 — read + write) |
| **Bootstrap seed (P2P)** | `34.61.207.49:30303` (Iowa, US — operated by Noesis AI) |
| **Genesis** | embedded in the binary; also at [`deploy/mainnet-genesis.txt`](deploy/mainnet-genesis.txt) |
| **Block time** | 2 s · **2-block finality** ≈ 4 s |
| **Engine release** | [`v1.0.13`](https://github.com/beltromatti/agentchain/releases/latest) |

The public RPC is a Caddy reverse proxy in front of the seed's local JSON-RPC port, with Let's Encrypt TLS and kernel-level (nftables) rate limiting. It is the same `agentchain` daemon any operator runs; the proxy layer just provides HTTPS, request shaping, and a stable hostname. Both reads and writes are public — anyone can call `tx_submit`, the signature is verified on the way in. See [`SECURITY-AUDIT.md` § 6](SECURITY-AUDIT.md) for the hardening detail.

Equivalent to `https://api.mainnet-beta.solana.com` for Solana, or `https://bitcoin-rpc.publicnode.com` for Bitcoin: a foundation-operated public good, best-effort, no SLA, free. Production deployments should run their own node (see [Run your own node](#run-your-own-node)) and treat the public RPC as a fallback.

```sh
curl -sS -X POST -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"chain_info"}' \
  https://api.agentchain.noesisai.it/
```

## Try it in 60 seconds

Download the binary for your OS from the [latest release](https://github.com/beltromatti/agentchain/releases/latest) — Linux x86_64/arm64, macOS x86_64/arm64, Windows x86_64 are all built on every tag.

```sh
# macOS arm64 example
curl -L https://github.com/beltromatti/agentchain/releases/latest/download/agentchain-v1.0.13-macos-arm64.tar.gz | tar xz
cd agentchain-v1.0.13-*
./agentchain info                           # queries mainnet by default
```

That's it — `info`, `balance`, `send`, `stake`, `unbond` all default to the public RPC. Run any command with `--help` for its full options.

### Send a transaction in three commands

```sh
./agentchain keygen                                    # writes ~/.agentchain/node.key, prints your address
./agentchain balance                                   # checks your own balance on mainnet
./agentchain send --to <recipient_address> --amount 1000000   # 1 CRD = 1,000,000 µCRD
```

Getting CRD to spend: this is mainnet, there is no faucet. Receive CRD from someone who has it. The Noesis AI founder allocation is open to early contributors and projects building on AgentChain — open a GitHub issue or write to `info@noesis.ai` with a brief description of what you're going to do.

## Run your own node

The same binary that is a CLI is also the daemon. With no flags, `agentchain node` joins mainnet as a syncing peer (data goes to `~/.agentchain`, genesis is embedded, seeds are baked in):

```sh
./agentchain node                            # join mainnet, sync only
./agentchain node --validator                # also propose & vote on blocks
```

Becoming a validator requires bonded stake (`≥ 100 CRD = 100,000,000 µCRD`). The full path is:

1. `agentchain keygen` — generates your wallet at `~/.agentchain/node.key` (the same key is used for both transactions and block signing). Print the address with `agentchain pubkey`.
2. Acquire CRD (see above).
3. `agentchain stake --amount 100000000` — bond 100 CRD. Your key becomes eligible for sortition immediately.
4. Run `agentchain node --validator`. The node starts proposing blocks the next slot in which sortition selects it.

Going offline is **never** slashed — the protocol is deliberately forgiving about uptime. You lose potential block rewards while offline but nothing else. The only slashable behaviour is cryptographically-provable double-signing; see [`PROTOCOL.md` § 6.6](PROTOCOL.md). Block reward economics are in [`PROTOCOL.md` § 8](PROTOCOL.md).

To unbond, `agentchain unbond --amount …`. In v1.0.x the funds return to your balance immediately; the protocol-specified 24-hour cooldown queue is a v1.1 item (see [`TECHNICAL-IMPLEMENTATION.md` § 9.4](TECHNICAL-IMPLEMENTATION.md)).

### Run your own public RPC

If you want to provide an RPC endpoint to others (free, paid, or rate-limited), do exactly what the foundation seed does:

1. Run `agentchain node --rpc-host 0.0.0.0` — exposes the JSON-RPC port on all interfaces.
2. Put it behind your own reverse proxy with TLS + rate limiting (Caddy + nftables, or nginx, or Cloudflare — see [`deploy/`](deploy/) for the reference setup).
3. Point your own DNS at it.

The protocol does not gate on any centralised registry: every public RPC is just a node someone decided to expose. The Noesis-operated `api.agentchain.noesisai.it` is the first; more are welcome.

## How the consensus works (in 60 seconds)

Every 2 seconds is a slot. In every slot:

1. Each validator privately computes a VRF over `(epoch_seed, slot_number)` using its private key. The output decides whether *it* is leader-eligible this slot and whether *it* sits in this slot's 64-member committee. The two roles are sampled independently.
2. Leader-eligible validators build a block and broadcast it. The leader threshold is permissive on purpose (~2 candidates per slot) so a single offline leader does not stall the slot.
3. **At slot start + 900 ms**, every committee member picks the proposal with the lowest VRF-derived leader priority it has seen and signs *exactly one* commit vote for it. Priority is a deterministic function of the proposer's VRF output and `sqrt(stake)`, so every honest committee member chooses the same winner. The deferred-vote window is what lets two simultaneous proposers coexist without forking the chain.
4. When committee signers carrying more than two thirds of the slot's expected `sqrt(stake)` weight have signed the same block, every node accepts it. Two blocks later it is irreversible.

The mechanism is a faithful synthesis of well-understood building blocks: VRF sortition (originally Algorand), square-root vote weighting (public-choice literature), single-round BFT commits, EIP-1559 fee burning, and a deferred priority-based vote convergence step that makes the protocol safe for any number of validators from 1 to 10⁶. No individual component is novel. The contribution is the combination, tuned for an audience that historical PoS designs have been quietly excluding.

The mechanism has a name: **Proof of Sustained Availability**. The full design is in [PROTOCOL.md](PROTOCOL.md); the vote-convergence rule is normative under § 6.5.1.

## Build from source

```sh
sudo apt-get install -y cmake build-essential libsodium-dev   # Debian/Ubuntu
brew install cmake libsodium                                  # macOS

git clone https://github.com/beltromatti/agentchain
cd agentchain
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
N=4 RUN_S=30 testnet/run.sh    # 4-node local testnet smoke
```

## CLI quick reference

Every command defaults to mainnet over HTTPS. Override with `--rpc URL` (any host:port or http(s):// URL).

```sh
# Wallet & queries
agentchain keygen     [--out FILE]                                # default: ~/.agentchain/node.key
agentchain pubkey     [--key FILE | --data-dir DIR]               # print your address
agentchain balance    [--address HEX] [--rpc URL] [--key FILE]    # check an account
agentchain info       [--rpc URL]                                 # chain state

# Transactions (signed locally, broadcast via JSON-RPC)
agentchain send       --to HEX --amount UCRD
                      [--from-key FILE] [--rpc URL] [--tip N] [--memo TEXT]
agentchain stake      --amount UCRD  [--from-key FILE] [--rpc URL] [--tip N]
agentchain unbond     --amount UCRD  [--from-key FILE] [--rpc URL] [--tip N]

# Operating a node
agentchain node       [--validator] [-v]
                      [--data-dir DIR] [--genesis FILE] [--seeds H:P,…]
                      [--port N] [--rpc-port N] [--rpc-host BIND]
agentchain genesis    --chain-id N --out FILE [--timestamp-ms N]
                      [--account HEX:BAL:STAKE …]                 # for testnets

agentchain version
agentchain help                                                   # plus `--help` per command
```

## Architecture, in one diagram

```
                       ┌──────────────────────┐
   slot tick (2s) ────▶│  consensus           │◀── tx ann / vote / block ann (gossip)
                       │  (PoSA orchestration)│
                       └─────────┬────────────┘
                                 │ proposes, votes
                                 ▼
   ┌──────────────┐       ┌──────────────┐       ┌──────────────┐
   │  mempool     │──tx──▶│   chain      │──blk─▶│   state      │
   │  (fee mkt)   │       │   (storage,  │       │   (accounts, │
   │              │       │    validate) │       │    names)    │
   └──────▲───────┘       └──────┬───────┘       └──────────────┘
          │                      │  state root
       tx │                      │
          │                      ▼
   ┌──────┴───────┐       ┌──────────────┐
   │  RPC (HTTP)  │       │  net (TCP)   │
   │  JSON-RPC    │       │  gossip      │
   └──────────────┘       └──────────────┘
```

Ten modules plus `main.c` compile into a single binary. The whole codebase is around 6,600 lines of C, by design.

## Security

A first-party security audit by Noesis AI is published at [`SECURITY-AUDIT.md`](SECURITY-AUDIT.md). It enumerates the cryptographic primitives used, the protocol invariants enforced, the code-level hazards reviewed, every implementation finding with status, and the known open items. Every claim is annotated with its evidence — a libsodium audit, an IETF RFC, or a reproducible test in this repository.

If you find a vulnerability, please open a private security advisory on GitHub or reach out at [info@noesis.ai](mailto:info@noesis.ai).

## Where to go from here

- **[PROTOCOL.md](PROTOCOL.md)** — the normative specification.
- **[TECHNICAL-IMPLEMENTATION.md](TECHNICAL-IMPLEMENTATION.md)** — how the reference client realises the protocol, the threading model, the v1.0 deviations and their rationale.
- **[SECURITY-AUDIT.md](SECURITY-AUDIT.md)** — first-party audit, evidence-based.
- **[`deploy/`](deploy/)** — systemd unit, Dockerfile, mainnet-seeds list, the canonical mainnet-genesis file.

## Who is building this

**[Noesis AI](https://github.com/beltromatti)** — an open-source effort operating out of Milan, Italy. AgentChain is the first project under the Noesis AI umbrella. The protocol design and the reference client are by **[Mattia Beltrami](https://github.com/beltromatti)**, an undergraduate at Politecnico di Milano. The work is independent and unfunded; contributions are welcome.

Reach out: [info@noesis.ai](mailto:info@noesis.ai). Issues are the preferred channel for substantive discussion.

## License

Apache 2.0. See [LICENSE](LICENSE). No part of this repository is encumbered by patents, royalty agreements, or restrictive terms.

---

<div align="center">

*Honest decentralisation. Deterministic finality. One binary. Read the protocol — it fits on a coffee break.*

</div>
