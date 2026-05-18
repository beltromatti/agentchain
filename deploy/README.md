# Deploying AgentChain

Two ways to run a production AgentChain node. Both expect the binary from a [release](../../../releases) (or `cmake --build build`) and a `genesis.txt` for the network you intend to join.

## systemd (preferred for home and bare-metal validators)

```sh
sudo bash deploy/systemd/install.sh ./agentchain ./genesis.txt
sudo journalctl -u agentchain -f
```

The installer creates a non-root `agentchain` user, drops the binary at `/usr/local/bin/agentchain`, the genesis at `/etc/agentchain/genesis.txt`, the data directory at `/var/lib/agentchain`, and enables the systemd unit. Resource limits are set conservatively (`MemoryMax=512M`, `CPUQuota=50%`) so the node behaves as a polite guest on a shared machine.

Customise the seed list and ports by editing `/etc/systemd/system/agentchain.service` and running `sudo systemctl daemon-reload && sudo systemctl restart agentchain`.

## Docker

```sh
cp ./genesis.txt deploy/docker/genesis.txt
cd deploy/docker
docker compose up --build -d
docker compose logs -f
```

Image is built from Alpine + libsodium and weighs under 20 MB. Persistent state lives in the `agentchain-data` named volume. The RPC port is exposed on `0.0.0.0:30304` inside the container — if you publish the port externally, place it behind a firewall or reverse proxy.

## Mainnet bootstrap

AgentChain mainnet is **live** as of `2026-05-18T16:16:41Z`.

- **chain_id:** `1`
- **Seed:** `34.61.207.49:30303` (GCP us-central1-a, Iowa — Noesis AI)
- **RPC:** `http://34.61.207.49:30304`
- **Genesis file:** [`mainnet-genesis.txt`](mainnet-genesis.txt) — canonical, bit-for-bit reproducible

Join with any of the prebuilt binaries:

```sh
agentchain node \
    --data-dir   ~/agentchain-data \
    --genesis    deploy/mainnet-genesis.txt \
    --seeds      34.61.207.49:30303 \
    --validator
```

Bring up your own seed by running the same command on a public-IP host of your choice, and PR an entry to [`mainnet-seeds.txt`](mainnet-seeds.txt) for community discoverability.

## Joining a testnet

Run `testnet/run.sh` for a local 4-node loopback testnet; everything is in `testnet/` and tears down cleanly.

For a shared devnet, the project maintains an `agentchain-devnet` configuration in the release artefacts — see the README of the relevant release.
