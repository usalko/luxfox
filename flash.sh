#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR1="$SCRIPT_DIR/output/out/media_out/root"
ROOT_DIR2="$SCRIPT_DIR/media/out"
OEM_DIR="$SCRIPT_DIR/output/out/oem"

ULAMA_CONF="etc/ulama.conf"
VCPD_CONF="etc/vcpd.conf"
RADIOD_CONF="etc/radiod.conf"

NODE_ID="${1:-}"

# Parse --node N from arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --node)
            NODE_ID="$2"
            shift 2
            ;;
        [0-9]*)
            NODE_ID="$1"
            shift
            ;;
        *)
            shift
            ;;
    esac
done

# Interactive prompt if not provided
if [[ -z "$NODE_ID" ]]; then
    # Try to read from existing config
    CURRENT_NODE=""
    if [[ -f "$ROOT_DIR1/$RADIOD_CONF" ]]; then
        CURRENT_NODE=$(grep -oP '^node=\K[0-9]+' "$ROOT_DIR1/$RADIOD_CONF" 2>/dev/null || true)
    fi

    if [[ -n "$CURRENT_NODE" ]]; then
        read -rp "Drone node ID [${CURRENT_NODE}]: " NODE_ID
        NODE_ID="${NODE_ID:-$CURRENT_NODE}"
    else
        read -rp "Drone node ID (1-250): " NODE_ID
    fi
fi

# Validate
if ! [[ "$NODE_ID" =~ ^[0-9]+$ ]] || [[ "$NODE_ID" -lt 1 ]] || [[ "$NODE_ID" -gt 250 ]]; then
    echo "ERROR: node ID must be 1-250, got: $NODE_ID"
    exit 1
fi

echo "=== Flash LuckFox with node_id=$NODE_ID ==="

# Patch node ID in configs before building firmware image
if [[ -f "$ROOT_DIR1/$ULAMA_CONF" ]]; then
    sed -i "s/^node=.*/node=$NODE_ID/" "$ROOT_DIR1/$ULAMA_CONF"
    echo "  ulama.conf: node=$NODE_ID"
fi
if [[ -f "$ROOT_DIR2/$ULAMA_CONF" ]]; then
    sed -i "s/^node=.*/node=$NODE_ID/" "$ROOT_DIR2/$ULAMA_CONF"
    echo "  ulama.conf: node=$NODE_ID"
fi

if [[ -f "$ROOT_DIR1/$VCPD_CONF" ]]; then
    sed -i "s/^node=.*/node=$NODE_ID/" "$ROOT_DIR1/$VCPD_CONF"
    echo "  vcpd.conf:  node=$NODE_ID"
fi
if [[ -f "$ROOT_DIR2/$VCPD_CONF" ]]; then
    sed -i "s/^node=.*/node=$NODE_ID/" "$ROOT_DIR2/$VCPD_CONF"
    echo "  vcpd.conf:  node=$NODE_ID"
fi

if [[ -f "$ROOT_DIR1/$RADIOD_CONF" ]]; then
    sed -i "s/^node=.*/node=$NODE_ID/" "$ROOT_DIR1/$RADIOD_CONF"
    echo "  vcpd.conf:  node=$NODE_ID"
fi
if [[ -f "$ROOT_DIR2/$RADIOD_CONF" ]]; then
    sed -i "s/^node=.*/node=$NODE_ID/" "$ROOT_DIR2/$RADIOD_CONF"
    echo "  vcpd.conf:  node=$NODE_ID"
fi

# Build firmware (packs OEM partition with patched configs)
./build.sh firmware

echo "=== Flashing... ==="
sudo ./rkflash.sh update

echo ""
echo "=== Done! Node ID: $NODE_ID ==="
echo "  Callsign: C-$(printf '%03d' "$NODE_ID")"
