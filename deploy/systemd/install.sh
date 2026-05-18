#!/usr/bin/env bash
# Install AgentChain as a systemd service.
#
# Usage:  sudo bash install.sh /path/to/agentchain /path/to/genesis.txt

set -euo pipefail

BIN="${1:-/usr/local/bin/agentchain}"
GENESIS="${2:-./genesis.txt}"

if [[ $EUID -ne 0 ]]; then
    echo "must be run as root" >&2; exit 1
fi
if [[ ! -x "$BIN" ]]; then
    echo "binary not found or not executable: $BIN" >&2; exit 1
fi
if [[ ! -f "$GENESIS" ]]; then
    echo "genesis file not found: $GENESIS" >&2; exit 1
fi

# 1. user/group
id -u agentchain >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin agentchain

# 2. install binary
install -m 0755 "$BIN" /usr/local/bin/agentchain

# 3. data + config dirs
install -d -m 0750 -o agentchain -g agentchain /var/lib/agentchain
install -d -m 0755 /etc/agentchain
install -m 0644 "$GENESIS" /etc/agentchain/genesis.txt

# 4. systemd unit
install -m 0644 "$(dirname "$0")/agentchain.service" /etc/systemd/system/agentchain.service
systemctl daemon-reload
systemctl enable --now agentchain
systemctl status --no-pager agentchain
