#!/bin/sh
# Mount /etc/agentchain/genesis.txt as your genesis file at runtime if the
# data directory does not yet contain a chain. Otherwise the existing chain
# state is reused.
set -e

if [ ! -f /var/lib/agentchain/meta.bin ]; then
    if [ -f /etc/agentchain/genesis.txt ]; then
        exec /usr/local/bin/agentchain "$@" --genesis /etc/agentchain/genesis.txt
    fi
fi

exec /usr/local/bin/agentchain "$@"
