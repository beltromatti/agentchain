# blockchain

Build:
```
cmake -S . -B build
cmake --build build
```

Run a node:
```
./build/blockchain node
```
If `data/chain.state` is missing, `node` tries to sync from peers; configure `BC_SEEDS` (e.g. `1.2.3.4:30303`) to join the existing chain.

Generate a new keypair (overwrites `data/identity.key`) and start the node:
```
./build/blockchain node --new-keys
```

Create a new dev chain (genesis) on this machine:
```
./build/blockchain bootstrap
```

Print local node public key:
```
./build/blockchain pubkey
```

Send a transfer:
```
./build/blockchain transfer <receiver_pub_hex> <amount>
```

Check balance:
```
./build/blockchain balance [pub_hex]
```

Environment:
- `BC_PRIVKEY` / `BC_PUBKEY`: hex keys (`BC_PRIVKEY` can be 32B seed or 64B secret; priv required for node/transfer/mint)
- `BC_PORT`: peer UDP port (default 30303)
- `BC_CTL_PORT`: local control port (default 30304)
- `BC_SEEDS`: comma-separated `ip:port` seeds
