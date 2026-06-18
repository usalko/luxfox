#!/bin/bash
# ulama-gw-start.sh — Set up monitor mode and launch ulama-gw
#
# Usage:
#   sudo ./ulama-gw-start.sh [adapter] [options...]
#
# Examples:
#   sudo ./ulama-gw-start.sh                          # auto-detect adapter
#   sudo ./ulama-gw-start.sh wlx088af1287d57          # specific adapter
#   sudo ./ulama-gw-start.sh --verbose                 # auto-detect + verbose
#   sudo ./ulama-gw-start.sh wlx088af1287d57 --verbose # specific + verbose

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

# Parse arguments: first non-flag arg is adapter name
ADAPTER=""
GW_ARGS=()
for arg in "$@"; do
    if [ -z "$ADAPTER" ] && [[ "$arg" != -* ]]; then
        ADAPTER="$arg"
    else
        GW_ARGS+=("$arg")
    fi
done

# Auto-detect adapter if not specified
if [ -z "$ADAPTER" ]; then
    ADAPTERS=()
    while IFS= read -r line; do
        iface=$(echo "$line" | awk '{print $2}')
        # Skip internal WiFi (wlp*, wlan*) — look for USB adapters (wlx*)
        if [[ "$iface" == wlx* ]]; then
            ADAPTERS+=("$iface")
        fi
    done < <(iw dev 2>/dev/null | grep "Interface" | grep -v "$MON_IFACE")

    if [ ${#ADAPTERS[@]} -eq 0 ]; then
        # Fallback: any managed WiFi interface
        while IFS= read -r line; do
            ADAPTERS+=("$(echo "$line" | awk '{print $2}')")
        done < <(iw dev 2>/dev/null | grep "Interface" | grep -v "$MON_IFACE")
    fi

    if [ ${#ADAPTERS[@]} -eq 0 ]; then
        echo "ERROR: no WiFi adapter found" >&2
        exit 1
    elif [ ${#ADAPTERS[@]} -eq 1 ]; then
        ADAPTER="${ADAPTERS[0]}"
    else
        echo "Multiple adapters found:" >&2
        for i in "${!ADAPTERS[@]}"; do
            echo "  $((i+1))) ${ADAPTERS[$i]}" >&2
        done
        read -p "Select [1-${#ADAPTERS[@]}]: " choice
        ADAPTER="${ADAPTERS[$((choice-1))]}"
    fi
fi

echo "[ulama-gw] adapter: $ADAPTER"

# Check if mon-host already exists and is monitor mode
if iw dev "$MON_IFACE" info 2>/dev/null | grep -q "type monitor"; then
    echo "[ulama-gw] $MON_IFACE already up"
else
    echo "[ulama-gw] creating $MON_IFACE from $ADAPTER on channel $CHANNEL"

    # Determine phy before any changes
    PHY=$(iw dev "$ADAPTER" info 2>/dev/null | grep wiphy | awk '{print "phy"$2}')
    if [ -z "$PHY" ]; then
        echo "ERROR: cannot determine phy for $ADAPTER" >&2
        exit 1
    fi

    # Remove stale monitor interface
    iw dev "$MON_IFACE" del 2>/dev/null || true

    # Stop NetworkManager/wpa_supplicant from managing this adapter
    if command -v nmcli >/dev/null 2>&1; then
        nmcli device set "$ADAPTER" managed no 2>/dev/null || true
    fi
    killall -q wpa_supplicant 2>/dev/null || true

    # Bring down managed interface and remove it to free the phy
    ip link set "$ADAPTER" down 2>/dev/null || true
    iw dev "$ADAPTER" del 2>/dev/null || true
    sleep 0.2

    # Create monitor interface on the freed phy
    iw "$PHY" interface add "$MON_IFACE" type monitor
    iw dev "$MON_IFACE" set channel "$CHANNEL"
    ip link set "$MON_IFACE" up

    echo "[ulama-gw] $MON_IFACE up on channel $CHANNEL (phy: $PHY, was: $ADAPTER)"
fi

# Cleanup on exit
cleanup() {
    echo ""
    echo "[ulama-gw] shutting down, restoring $ADAPTER"
    iw dev "$MON_IFACE" del 2>/dev/null || true
    # Restore managed interface if it was removed
    if ! iw dev "$ADAPTER" info >/dev/null 2>&1 && [ -n "${PHY:-}" ]; then
        iw "$PHY" interface add "$ADAPTER" type managed 2>/dev/null || true
    fi
    ip link set "$ADAPTER" up 2>/dev/null || true
    if command -v nmcli >/dev/null 2>&1; then
        nmcli device set "$ADAPTER" managed yes 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

# Launch ulama-gw
echo "[ulama-gw] starting gateway..."
exec "$GW_BIN" --transport unow --iface "$MON_IFACE" --node 1 "${GW_ARGS[@]}"
