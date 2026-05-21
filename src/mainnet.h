/* AgentChain — mainnet alpha defaults baked into the binary.
 *
 * Centralising these in one header lets the CLI default to mainnet alpha
 * everywhere without forcing the user to type the same flags on every
 * invocation. Anything that should not be a default on a fresh testnet
 * binary goes in `src/codec.h` (protocol constants); anything that is
 * just "where mainnet alpha lives right now" goes here.
 *
 * The mainnet alpha genesis is reproduced verbatim from
 * `deploy/mainnet-genesis.txt`. Both copies are part of the release
 * artefact; mismatches are caught by `tests/test_mainnet_genesis.c`.
 */
#ifndef AGENTCHAIN_MAINNET_H
#define AGENTCHAIN_MAINNET_H

/* ------------------------------------------------------------------------- */
/* JSON-RPC endpoints.                                                       */
/* ------------------------------------------------------------------------- */

/* The canonical public read/write JSON-RPC URL for `chain_id=1`. Operated by
 * Noesis AI on a GCP Always-Free e2-micro in Iowa, behind a Caddy reverse
 * proxy with kernel-level rate limiting. See `SECURITY-AUDIT.md § 6`. */
#define AC_MAINNET_RPC          "https://api.agentchain.noesisai.it"

/* Loopback URL for users running their own node. */
#define AC_LOCAL_RPC            "http://127.0.0.1:30304"

/* ------------------------------------------------------------------------- */
/* P2P bootstrap seeds (comma-separated host:port).                          */
/* ------------------------------------------------------------------------- */

/* Bootstrap peer list. Each entry is a host:port that any fresh node tries
 * to dial when joining. All three foundation validators are listed equally
 * — no single node is special: if Iowa goes offline the new joiner falls
 * over to Frankfurt or Stockholm. Once a TCP connection is established the
 * HELLO peer-list gossip (HELLO v2, engine v1.1.0+) builds the rest of the
 * mesh dynamically. */
#define AC_MAINNET_SEEDS \
    "34.61.207.49:30303," \
    "18.192.176.100:30303," \
    "16.171.43.222:30303"

/* ------------------------------------------------------------------------- */
/* Mainnet genesis (chain_id=1).                                             */
/*                                                                            */
/* Embedded as a C string so a freshly-downloaded binary can launch a node    */
/* without first downloading a separate file. Must stay byte-identical to     */
/* `deploy/mainnet-genesis.txt`.                                              */
/* ------------------------------------------------------------------------- */

#define AC_MAINNET_GENESIS \
    "# AgentChain genesis configuration\n" \
    "chain_id     = 1\n" \
    "timestamp_ms = 1779397200000\n" \
    "\n" \
    "account 4bfe9fb5ea49f00bf3c78fb9e979223ee5fa630743befc5c1f5686ac923bf79a 39997000000000 1000000000\n" \
    "account 69f02a87d94d8e1ba76ea4ee374b5358ea4782d6f4249fbd8731e9b4ac863a9e 500000000 500000000\n" \
    "account 89dfe243a931659dd4f0d71b68110711262af7dcb58a053a9e9135ea454fa4fd 500000000 500000000\n"

#endif /* AGENTCHAIN_MAINNET_H */
