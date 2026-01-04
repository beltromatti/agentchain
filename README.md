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
