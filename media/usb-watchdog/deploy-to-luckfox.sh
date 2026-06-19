#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TARGET="${1:-}"
BIN_DEST="${2:-/usr/bin}"
CONFIG_DEST="${3:-/etc}"

if [[ -z "$TARGET" ]]; then
    echo "Usage: $0 <luckfox-host> [bin-dest] [config-dest]"
    echo "Example: $0 root@192.168.1.100"
    exit 1
fi

SSH_TARGET="$TARGET"
echo "[deploy-usb-watchdog] target=$SSH_TARGET"

ssh "$SSH_TARGET" "mkdir -p '$BIN_DEST' '$CONFIG_DEST/init.d'"

scp "$SCRIPT_DIR/scripts/usb-watchdog.sh" "$SSH_TARGET:$BIN_DEST/usb-watchdog.sh"
scp "$SCRIPT_DIR/scripts/S90usb-watchdog" "$SSH_TARGET:$CONFIG_DEST/init.d/S90usb-watchdog"

ssh "$SSH_TARGET" "chmod +x '$BIN_DEST/usb-watchdog.sh' '$CONFIG_DEST/init.d/S90usb-watchdog'"

echo "[deploy-usb-watchdog] deployed:"
echo "  - $BIN_DEST/usb-watchdog.sh"
echo "  - $CONFIG_DEST/init.d/S90usb-watchdog"
echo ""
echo "Start now:  ssh $SSH_TARGET '$CONFIG_DEST/init.d/S90usb-watchdog start'"
echo "It will auto-start on boot (S90 = before S97vcpd, S98ulama-gw, S99ulama)"
