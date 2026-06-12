#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ULAMA_ROOT="$SCRIPT_DIR"
UNOW_ROOT="$SCRIPT_DIR/../unow"

TARGET="${1:-}"
BIN_DEST="${2:-/usr/bin}"
CONFIG_DEST="${3:-/etc}"

usage() {
	cat <<EOF
ULAMA deploy to LuckFox

Usage:
  $0 <luckfox-host> [bin-dest] [config-dest]

Examples:
  $0 192.168.1.100
  $0 root@luckfox.local
  $0 192.168.1.100 /usr/bin /etc
EOF
}

if [[ -z "$TARGET" ]]; then
	usage
	exit 1
fi

if [[ ! -x "$ULAMA_ROOT/out/bin/ulamad" ]]; then
	make -C "$ULAMA_ROOT" >/dev/null
fi

SSH_TARGET="$TARGET"
echo "[deploy-ulama] target=$SSH_TARGET bin_dest=$BIN_DEST config_dest=$CONFIG_DEST"

ssh "$SSH_TARGET" "mkdir -p '$BIN_DEST' '$BIN_DEST/scripts' '$CONFIG_DEST' '$CONFIG_DEST/init.d'"

scp "$ULAMA_ROOT/out/bin/ulamad" "$SSH_TARGET:$BIN_DEST/ulamad"
scp "$ULAMA_ROOT/defaults/ulama.conf" "$SSH_TARGET:$CONFIG_DEST/ulama.conf"
scp "$ULAMA_ROOT/scripts/S99ulama" "$SSH_TARGET:$CONFIG_DEST/init.d/S99ulama"
scp "$UNOW_ROOT/scripts/unow-mon.sh" "$SSH_TARGET:$BIN_DEST/scripts/unow-mon.sh"
scp "$UNOW_ROOT/scripts/unow-down.sh" "$SSH_TARGET:$BIN_DEST/scripts/unow-down.sh"

ssh "$SSH_TARGET" "chmod +x '$BIN_DEST/ulamad' '$BIN_DEST/scripts/unow-mon.sh' '$BIN_DEST/scripts/unow-down.sh' '$CONFIG_DEST/init.d/S99ulama'"

echo "[deploy-ulama] deployed:"
echo "  - $BIN_DEST/ulamad"
echo "  - $CONFIG_DEST/ulama.conf"
echo "  - $CONFIG_DEST/init.d/S99ulama"
echo "  - $BIN_DEST/scripts/unow-mon.sh"