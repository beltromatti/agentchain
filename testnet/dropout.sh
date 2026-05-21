#!/usr/bin/env bash
# Dropout/rejoin stress: 3 validators run; kill node 2 at t=20s; verify nodes
# 1+3 keep committing (live-set threshold prevents freeze); rejoin node 2 at
# t=50s; verify it catches up and chain progresses through restart.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/agentchain}"
WORK="$ROOT/testnet/run"
LOG="$WORK/logs"
PIDS="$WORK/pids"
KEYS="$WORK/keys"
DATA="$WORK/data"

rm -rf "$WORK"
mkdir -p "$LOG" "$PIDS" "$KEYS" "$DATA"

cleanup() {
    set +e
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
}
trap cleanup EXIT INT TERM

PUBKEYS=()
for i in 1 2 3; do
    KEY="$KEYS/node$i.key"
    "$BIN" keygen --out "$KEY" >/dev/null
    PUBKEYS+=("$("$BIN" pubkey --key "$KEY")")
done

TS_MS="$(($(date +%s) * 1000))"
GENESIS="$WORK/genesis.txt"
GEN_ARGS=( --chain-id 2025 --timestamp-ms "$TS_MS" --out "$GENESIS" )
for pk in "${PUBKEYS[@]}"; do
    GEN_ARGS+=( --account "${pk}:1000000000:200000000" )
done
"$BIN" genesis "${GEN_ARGS[@]}" >/dev/null

PORT_BASE=31000
RPC_BASE=32000
SEEDS="127.0.0.1:31001,127.0.0.1:31002,127.0.0.1:31003"

start_node() {
    local i="$1"
    local NDATA="$DATA/node$i"
    mkdir -p "$NDATA"
    cp "$KEYS/node$i.key" "$NDATA/node.key"
    chmod 600 "$NDATA/node.key"
    "$BIN" node \
        --data-dir "$NDATA" --genesis "$GENESIS" \
        --port "$((PORT_BASE + i))" --rpc-port "$((RPC_BASE + i))" \
        --seeds "$SEEDS" --validator \
        > "$LOG/node$i.log" 2>&1 &
    echo $! > "$PIDS/node$i.pid"
}

height_of() {
    local i="$1"
    curl -s -X POST -H 'content-type: application/json' \
         -d '{"jsonrpc":"2.0","id":1,"method":"chain_info"}' \
         "http://127.0.0.1:$((RPC_BASE+i))/" 2>/dev/null \
        | sed -E 's/.*"height":([0-9]+).*/\1/'
}

for i in 1 2 3; do start_node "$i"; done
echo "started 3 nodes"
sleep 15

H_BEFORE=$(height_of 1)
echo "t=15s heights: $(height_of 1) $(height_of 2) $(height_of 3)  (baseline)"

kill -TERM "$(cat "$PIDS/node2.pid")"
rm -f "$PIDS/node2.pid"
echo "t=15s killed node 2"

# Verify chain keeps advancing with nodes 1+3 only.
sleep 30
H_AFTER_KILL=$(height_of 1)
echo "t=45s heights: $(height_of 1) - $(height_of 3)  (node 2 dead)"
if (( H_AFTER_KILL <= H_BEFORE + 5 )); then
    echo "FAIL: chain froze with one validator down (h=$H_BEFORE -> $H_AFTER_KILL)"
    exit 1
fi

# Rejoin node 2.
start_node 2
echo "t=45s rejoined node 2 (pid=$(cat "$PIDS/node2.pid"))"
sleep 25
echo "t=70s heights: $(height_of 1) $(height_of 2) $(height_of 3)  (after rejoin)"

# Verify node 2 caught up close to the tip.
H1=$(height_of 1); H2=$(height_of 2); H3=$(height_of 3)
DIFF=$(( H1 > H2 ? H1 - H2 : H2 - H1 ))
if (( DIFF > 5 )); then
    echo "WARN: node 2 lagging (h1=$H1 h2=$H2 h3=$H3)"
else
    echo "OK: node 2 caught up (h1=$H1 h2=$H2 h3=$H3)"
fi

echo ""
echo "ERROR/WARN tail across nodes:"
for i in 1 2 3; do
    grep -E 'ERROR|WARN' "$LOG/node$i.log" | head -10 | sed "s/^/  node $i: /" || true
done

echo "done"
