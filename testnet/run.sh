#!/usr/bin/env bash
# AgentChain local testnet — spins up N validator nodes on this host, waits for
# them to make progress, runs a smoke transaction, and tears down cleanly.
#
# Usage:
#   testnet/run.sh                     # default: 4 nodes for 30s
#   N=3 RUN_S=60 testnet/run.sh        # 3 nodes for 60 seconds

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/agentchain}"
N="${N:-4}"
RUN_S="${RUN_S:-30}"

WORK="$ROOT/testnet/run"
LOG="$WORK/logs"
PIDS="$WORK/pids"
KEYS="$WORK/keys"
DATA="$WORK/data"
mkdir -p "$LOG" "$PIDS" "$KEYS" "$DATA"

cleanup() {
    set +e
    if [[ -d "$PIDS" ]]; then
        for f in "$PIDS"/*.pid; do
            [[ -e "$f" ]] || continue
            pid="$(cat "$f")"
            if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
                kill -TERM "$pid" 2>/dev/null
            fi
        done
        sleep 1
        for f in "$PIDS"/*.pid; do
            [[ -e "$f" ]] || continue
            pid="$(cat "$f")"
            if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
                kill -KILL "$pid" 2>/dev/null
            fi
        done
    fi
}
trap cleanup EXIT INT TERM

# --- 1. generate keys + pubkeys
PUBKEYS=()
for i in $(seq 1 "$N"); do
    KEY="$KEYS/node$i.key"
    [[ -e "$KEY" ]] || "$BIN" keygen --out "$KEY" >/dev/null
    PK="$("$BIN" pubkey --key "$KEY")"
    PUBKEYS+=("$PK")
done
echo "generated $N keys"

# --- 2. build genesis (each validator gets 1000 CRD balance + 200 CRD stake)
TS_MS="$(($(date +%s) * 1000))"
GENESIS="$WORK/genesis.txt"
GEN_ARGS=( --chain-id 2025 --timestamp-ms "$TS_MS" --out "$GENESIS" )
for pk in "${PUBKEYS[@]}"; do
    # balance: 1000 CRD = 1,000,000,000 µCRD
    # stake:    200 CRD =   200,000,000 µCRD
    GEN_ARGS+=( --account "${pk}:1000000000:200000000" )
done
"$BIN" genesis "${GEN_ARGS[@]}" >/dev/null
echo "wrote $GENESIS"

# --- 3. start nodes
PORT_BASE=31000
RPC_BASE=32000

SEEDS=""
for i in $(seq 1 "$N"); do
    SEEDS+="127.0.0.1:$((PORT_BASE + i)),"
done
SEEDS="${SEEDS%,}"

for i in $(seq 1 "$N"); do
    NDATA="$DATA/node$i"
    mkdir -p "$NDATA"
    cp "$KEYS/node$i.key" "$NDATA/node.key"
    chmod 600 "$NDATA/node.key"

    "$BIN" node \
        --data-dir   "$NDATA" \
        --genesis    "$GENESIS" \
        --port       "$((PORT_BASE + i))" \
        --rpc-port   "$((RPC_BASE + i))" \
        --seeds      "$SEEDS" \
        --validator \
        > "$LOG/node$i.log" 2>&1 &
    echo $! > "$PIDS/node$i.pid"
    echo "node $i: p2p=$((PORT_BASE+i)) rpc=$((RPC_BASE+i)) pid=$!"
done

# --- 4. wait for chain to progress
echo "running for ${RUN_S}s…"
sleep 2

# Wait for first non-genesis block on any node.
END_TS=$(( $(date +%s) + RUN_S ))
LAST_HEIGHTS=()
for i in $(seq 1 "$N"); do LAST_HEIGHTS+=(0); done

SENT_TX=""
while [[ $(date +%s) -lt $END_TS ]]; do
    for i in $(seq 1 "$N"); do
        out=$(curl -s -X POST -H 'content-type: application/json' \
              -d '{"jsonrpc":"2.0","id":1,"method":"chain_info"}' \
              "http://127.0.0.1:$((RPC_BASE+i))/" 2>/dev/null || true)
        height=$(echo "$out" | sed -E 's/.*"height":([0-9]+).*/\1/' || echo "?")
        if [[ "$height" =~ ^[0-9]+$ ]]; then
            LAST_HEIGHTS[i-1]="$height"
        fi
    done
    echo "  heights: ${LAST_HEIGHTS[*]}"

    # Once the chain has settled past height 2, try a transfer.
    if [[ -z "$SENT_TX" && "${LAST_HEIGHTS[0]}" -ge 3 ]]; then
        echo "  -- submitting transfer node1 -> node2 (100 µCRD)…"
        "$BIN" send \
            --rpc       "127.0.0.1:$((RPC_BASE+1))" \
            --from-key  "$KEYS/node1.key" \
            --to        "${PUBKEYS[1]}" \
            --amount    100 \
            --tip       1 \
            --valid-slots 100 \
            && SENT_TX="yes" || echo "  -- send failed"
    fi
    sleep 3
done

# Verify recipient balance went up.
if [[ -n "$SENT_TX" ]]; then
    bal=$(curl -s -X POST -H 'content-type: application/json' \
          -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"account_get\",\"params\":{\"address\":\"${PUBKEYS[1]}\"}}" \
          "http://127.0.0.1:$((RPC_BASE+2))/")
    echo ""
    echo "recipient (node 2) balance @ node 2:"
    echo "  $bal"
fi

# --- 5. report
echo ""
echo "FINAL"
for i in $(seq 1 "$N"); do
    echo "  node $i: height=${LAST_HEIGHTS[i-1]}"
done

# Spot-check log tails for errors.
echo ""
echo "ERROR/WARN tail across nodes:"
for i in $(seq 1 "$N"); do
    grep -E 'ERROR|WARN' "$LOG/node$i.log" | head -5 | sed "s/^/  node $i: /" || true
done

echo "done — cleaning up"
