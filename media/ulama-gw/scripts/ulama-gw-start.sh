#!/bin/bash
# ulama-gw-start.sh — Set up monitor mode and launch ulama-gw
#
# Usage:
#   sudo ./ulama-gw-start.sh <adapter> [gw-options...]
#
# Examples:
#   sudo ./ulama-gw-start.sh wlx088af1287d57
#   sudo ./ulama-gw-start.sh wlx088af1287d57 --verbose

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GW_BIN="$SCRIPT_DIR/../build/host-unow-tools/ulama_gw"
MON_IFACE="mon-host"
CHANNEL=6

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: requires sudo" >&2
    exit 1
fi

if [ ! -x "$GW_BIN" ]; then
    echo "ERROR: ulama_gw not found at $GW_BIN" >&2
    echo "Run: cd $(dirname "$SCRIPT_DIR") && make host-unow" >&2
    exit 1
fi

ADAPTER="${1:-}"
if [ -z "$ADAPTER" ] || [[ "$ADAPTER" == -* ]]; then
    echo "Usage: $0 <wifi-adapter> [gw-options...]" >&2
    echo "" >&2
    echo "Available adapters:" >&2
    iw dev 2>/dev/null | grep "Interface" | awk '{print "  " $2}'
    exit 1
fi
shift
GW_ARGS=("$@")

if ! iw dev "$ADAPTER" info >/dev/null 2>&1; then
    echo "ERROR: interface $ADAPTER not found" >&2
    exit 1
fi

PHY=$(iw dev "$ADAPTER" info | grep wiphy | awk '{print "phy"$2}')
echo "[ulama-gw] adapter: $ADAPTER ($PHY)"

# Check if mon-host already exists and is on the right phy
if iw dev "$MON_IFACE" info 2>/dev/null | grep -q "type monitor"; then
    echo "[ulama-gw] $MON_IFACE already up"
else
    echo "[ulama-gw] setting up $MON_IFACE on $PHY channel $CHANNEL"

    iw dev "$MON_IFACE" del 2>/dev/null || true

    # Aggressively disable managed interface — NM keeps bringing it back
    if command -v nmcli >/dev/null 2>&1; then
        nmcli device set "$ADAPTER" managed no 2>/dev/null || true
        nmcli device disconnect "$ADAPTER" 2>/dev/null || true
    fi
    ip link set "$ADAPTER" down 2>/dev/null || true

    # Delete managed interface to fully free the phy
    iw dev "$ADAPTER" del 2>/dev/null || true
    sleep 0.3

    # Create monitor interface on the now-free phy
    iw "$PHY" interface add "$MON_IFACE" type monitor
    iw dev "$MON_IFACE" set channel "$CHANNEL"
    ip link set "$MON_IFACE" up

    echo "[ulama-gw] $MON_IFACE up (channel $CHANNEL)"
fi

cleanup() {
    echo ""
    echo "[ulama-gw] shutting down, restoring $ADAPTER"
    iw dev "$MON_IFACE" del 2>/dev/null || true
    # Recreate managed interface if we deleted it
    if ! iw dev "$ADAPTER" info >/dev/null 2>&1; then
        iw "$PHY" interface add "$ADAPTER" type managed 2>/dev/null || true
    fi
    ip link set "$ADAPTER" up 2>/dev/null || true
    if command -v nmcli >/dev/null 2>&1; then
        nmcli device set "$ADAPTER" managed yes 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

echo "[ulama-gw] starting gateway..."
exec "$GW_BIN" --transport unow --iface "$MON_IFACE" --node 254 "${GW_ARGS[@]}"
