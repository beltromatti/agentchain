<div align="center">

<img src="docs/assets/banner.svg" alt="AgentChain — Layer-1 in pure C" width="100%"/>

**A Layer-1 blockchain for home validators and autonomous agents — written in pure C.**

[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![CI](https://github.com/beltromatti/agentchain/actions/workflows/build.yml/badge.svg)](https://github.com/beltromatti/agentchain/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/beltromatti/agentchain?include_prereleases&sort=semver)](https://github.com/beltromatti/agentchain/releases)
[![Protocol](https://img.shields.io/badge/protocol-v1-informational)](PROTOCOL.md)

[Protocol](PROTOCOL.md) · [Implementation notes](TECHNICAL-IMPLEMENTATION.md) · [Releases](https://github.com/beltromatti/agentchain/releases)

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

## What is shipping in v1.0.0

A reference client — **AgentChain Engine** — that implements the protocol end-to-end. Transfers, validator bonding, name registration, equivocation slashing, EIP-1559-style fee burn, deterministic 2-second slots, ~4-second finality, JSON-RPC over HTTP, gossip over TCP, atomic on-disk state. Cross-platform releases for Linux and macOS, x86_64 and arm64.

There is no smart-contract VM in v1, on purpose. The strategy is to ship a small, audited foundation now and add deterministic, agent-friendly execution on top later — not the other way around.

## How the consensus works (in 60 seconds)

Every 2 seconds is a slot. In every slot:

1. Each validator privately computes a VRF over `(epoch_seed, slot_number)` using its private key. The VRF output tells the validator whether *it* is eligible to propose this slot, and whether *it* is in this slot's 64-member committee. The two roles are sampled independently.
2. Validators that are leader-eligible produce a block and broadcast it. Validators that are committee-eligible sign the block they see and broadcast their signature.
3. When more than two thirds of the committee's `sqrt(stake)` has signed the same block, every node accepts it. Two blocks later, it is irreversible.

The mechanism is a faithful synthesis of well-understood building blocks: VRF sortition (originally Algorand), square-root vote weighting (public-choice literature), single-round BFT commits, and EIP-1559 fee burning. No individual component is novel. The contribution is the combination, tuned for an audience that historical PoS designs have been quietly excluding.

The mechanism has a name: **Proof of Sustained Availability**. The full design is in [PROTOCOL.md](PROTOCOL.md).

## Try it in 60 seconds

**Pre-built binaries (Linux x86_64, macOS arm64/x86_64) are on the [releases page](https://github.com/beltromatti/agentchain/releases/latest).** Download, verify the SHA-256, extract, and you have a node:

```sh
curl -L https://github.com/beltromatti/agentchain/releases/latest/download/agentchain-v1.0.0-macos-arm64.tar.gz | tar xz
cd agentchain-v1.0.0-*
./agentchain version
```

The binary is statically linked against libsodium, under 300 KB stripped, runs on `tier-2` hardware (Raspberry Pi 4, M1 MacBook Air, any Linux laptop).

## Try it locally

A four-node testnet runs on a single host in under 30 seconds.

```sh
# Prerequisites
sudo apt-get install -y cmake build-essential libsodium-dev   # Debian/Ubuntu
brew install cmake libsodium                                  # macOS

# Build
git clone https://github.com/beltromatti/agentchain
cd agentchain
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Run a 4-node testnet for 30 seconds, send a transfer, watch it commit
N=4 RUN_S=30 testnet/run.sh
```

You should see each node's height progress in lockstep, and the recipient's balance go up by 100 µCRD plus accumulated block rewards. The harness tears every process down cleanly on exit.

## CLI quick reference

```sh
agentchain node       --data-dir DIR [--genesis FILE] [--port 30303]
                       [--rpc-port 30304] [--seeds host:port,…] [--validator]

agentchain keygen     --out node.key
agentchain pubkey     --data-dir DIR
agentchain genesis    --chain-id N --out genesis.txt
                       --account HEX:BAL:STAKE [--account …]

agentchain send       --rpc 127.0.0.1:30304 --from-key node.key
                       --to HEX --amount UCRD [--tip N] [--memo TEXT]
agentchain balance    --rpc 127.0.0.1:30304 --address HEX
agentchain info       --rpc 127.0.0.1:30304
```

Every command is idempotent in the obvious way; data lives under one directory; keys are stored at mode 0600.

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

All eleven files compile into a single binary. The whole codebase is under 6,000 lines of C, by design.

## Where to go from here

- Read **[PROTOCOL.md](PROTOCOL.md)** if you want the normative specification: state model, consensus rules, fee market, security model, wire formats.
- Read **[TECHNICAL-IMPLEMENTATION.md](TECHNICAL-IMPLEMENTATION.md)** if you want to know how the reference client realises the protocol, where v1.0.0 deviates and why, the threading model, and the performance envelope.
- See **`deploy/`** for the systemd unit, Dockerfile, and the operator-side notes for running a validator.
- File issues, send PRs. The protocol is small enough to read end-to-end in 45 minutes. Auditing is welcome.

## Who is building this

**[Noesis AI](https://github.com/noesis-ai)** — an open-source organisation operating out of Milan, Italy. AgentChain is the first project under the Noesis AI umbrella. The protocol design and the reference client are by **[Mattia Beltrami](https://github.com/beltromatti)**, an undergraduate at Politecnico di Milano. The work is independent and unfunded; contributions are welcome.

Reach out: [agentchain@noesis.ai](mailto:agentchain@noesis.ai) (issues are the preferred channel for substantive discussion).

## License

Apache 2.0. See [LICENSE](LICENSE). No part of this repository is encumbered by patents, royalty agreements, or restrictive terms.

---

<div align="center">

*Honest decentralisation. Deterministic finality. One binary. Read the protocol — it fits on a coffee break.*

</div>
