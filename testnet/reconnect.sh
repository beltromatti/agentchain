#!/usr/bin/env bash
# Restart-reconnect convergence test. Reproduces the stale-slot stall: a node
# that is fully synced is killed and restarted while its peers keep producing.
# On reconnect the peers still hold its old (dead) slot; with the "first
# connection wins" rule the fresh connection was dropped and the restarted
# node stranded. With "newest wins" eviction it must re-converge to the head
# within a few seconds.

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

for i in 1 2 3; do start_node "$i"; done
echo "started 3 nodes; letting them sync to a common head…"
sleep 30
echo "heights pre-kill: $(height_of 1) $(height_of 2) $(height_of 3)"

# Kill node 3 (was fully synced) and immediately restart it. Its peers still
# hold the stale slot when the new connection arrives.
kill -KILL "$(cat "$PIDS/node3.pid")"; rm -f "$PIDS/node3.pid"
echo "killed node 3"
sleep 2
start_node 3
echo "restarted node 3 — measuring re-convergence:"

CONVERGED=""
START=$(date +%s)
while [[ $(( $(date +%s) - START )) -lt 40 ]]; do
    h1=$(height_of 1); h3=$(height_of 3)
    echo "  head=$h1 node3=$h3"
    if [[ "$h1" =~ ^[0-9]+$ && "$h3" =~ ^[0-9]+$ ]]; then
        gap=$(( h1 > h3 ? h1 - h3 : 0 ))
        # require it to be tracking the live head, not just its pre-kill height
        if (( gap <= 2 && h3 >= h1 - 2 && h1 > 30 )); then
            CONVERGED=$(( $(date +%s) - START )); break
        fi
    fi
    sleep 2
done

if [[ -z "$CONVERGED" ]]; then
    echo "FAIL: node 3 did not re-converge within 40s of restart (stale-slot stall)"
    echo "--- node1 net log ---"; grep -iE 'peer|evict|duplicate' "$LOG/node1.log" | tail -10
    exit 1
fi
echo "PASS: node 3 re-converged ${CONVERGED}s after restart"
echo "evictions observed (newest-wins): $(grep -c 'evicting stale slot' "$LOG"/node*.log | paste -sd+ | bc 2>/dev/null || grep -h 'evicting stale slot' "$LOG"/node*.log | wc -l)"
echo "done"
