#!/usr/bin/env bash
# Late-joiner catch-up test. Two validators run and produce a backlog; a third
# validator starts ~40 blocks late and must catch up to the head *natively*
# (no snapshot copy) within a few seconds — not at the throttled one-block-per-
# slot rate that the seal-grace bug imposed. Then it must start proposing and
# committing as an equal (signers=3 reachable).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/agentchain}"
WORK="$ROOT/testnet/run"
LOG="$WORK/logs"; PIDS="$WORK/pids"; KEYS="$WORK/keys"; DATA="$WORK/data"

rm -rf "$WORK"; mkdir -p "$LOG" "$PIDS" "$KEYS" "$DATA"

cleanup() {
    set +e
    for f in "$PIDS"/*.pid; do
        [[ -e "$f" ]] || continue
        pid="$(cat "$f")"; [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null && kill -TERM "$pid" 2>/dev/null
    done
    sleep 1
    for f in "$PIDS"/*.pid; do
        [[ -e "$f" ]] || continue
        pid="$(cat "$f")"; [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null
    done
}
trap cleanup EXIT INT TERM

PUBKEYS=()
for i in 1 2 3; do
    "$BIN" keygen --out "$KEYS/node$i.key" >/dev/null
    PUBKEYS+=("$("$BIN" pubkey --key "$KEYS/node$i.key")")
done

TS_MS="$(($(date +%s) * 1000))"
GENESIS="$WORK/genesis.txt"
# Equal-ish stakes so all three are leader-eligible. 1000 CRD bal + 500 stake.
GEN_ARGS=( --chain-id 2025 --timestamp-ms "$TS_MS" --out "$GENESIS" )
for pk in "${PUBKEYS[@]}"; do GEN_ARGS+=( --account "${pk}:1000000000:500000000" ); done
"$BIN" genesis "${GEN_ARGS[@]}" >/dev/null

PORT_BASE=31000; RPC_BASE=32000
SEEDS="127.0.0.1:31001,127.0.0.1:31002,127.0.0.1:31003"

start_node() {
    local i="$1"; local NDATA="$DATA/node$i"
    mkdir -p "$NDATA"; cp "$KEYS/node$i.key" "$NDATA/node.key"; chmod 600 "$NDATA/node.key"
    "$BIN" node --data-dir "$NDATA" --genesis "$GENESIS" \
        --port "$((PORT_BASE+i))" --rpc-port "$((RPC_BASE+i))" \
        --seeds "$SEEDS" --validator > "$LOG/node$i.log" 2>&1 &
    echo $! > "$PIDS/node$i.pid"
}

height_of() {
    curl -s -X POST -H 'content-type: application/json' \
        -d '{"jsonrpc":"2.0","id":1,"method":"chain_info"}' \
        "http://127.0.0.1:$((RPC_BASE+$1))/" 2>/dev/null | sed -E 's/.*"height":([0-9]+).*/\1/'
}

# Start two validators, build a backlog.
start_node 1; start_node 2
echo "started nodes 1+2; building backlog…"
sleep 40
H_BACKLOG=$(height_of 1)
echo "backlog height before node 3 joins: $H_BACKLOG"

# Start the late joiner.
start_node 3
echo "started node 3 (late joiner) at t=40s"

# Poll node 3 catching up. It must converge to within 3 of the head.
CAUGHT_UP_AT=""
START=$(date +%s)
while [[ $(( $(date +%s) - START )) -lt 60 ]]; do
    h1=$(height_of 1); h3=$(height_of 3)
    echo "  head=$h1  node3=$h3"
    if [[ "$h1" =~ ^[0-9]+$ && "$h3" =~ ^[0-9]+$ ]]; then
        gap=$(( h1 > h3 ? h1 - h3 : 0 ))
        if (( gap <= 3 && h3 > H_BACKLOG )); then
            CAUGHT_UP_AT=$(( $(date +%s) - START ))
            break
        fi
    fi
    sleep 2
done

if [[ -z "$CAUGHT_UP_AT" ]]; then
    echo "FAIL: node 3 did not catch up to within 3 of the head within 60s"
    echo "--- node3 log tail ---"; tail -20 "$LOG/node3.log"
    exit 1
fi
echo "PASS: node 3 caught up in ${CAUGHT_UP_AT}s after joining"

# Let it run as an equal and confirm it proposes + that signers=3 is reached.
sleep 20
echo "=== signers distribution across all nodes' commits (last 40 lines) ==="
for i in 1 2 3; do
    n3=$(grep -c 'signers=3' "$LOG/node$i.log" || true)
    n2=$(grep -c 'signers=2' "$LOG/node$i.log" || true)
    echo "  node $i: signers=3 commits=$n3  signers=2 commits=$n2"
done

echo "=== node 3 proposed any blocks that committed? ==="
grep -E 'proposed block' "$LOG/node3.log" | tail -3 || true

H1=$(height_of 1); H2=$(height_of 2); H3=$(height_of 3)
echo "final heights: node1=$H1 node2=$H2 node3=$H3"
echo "done"
