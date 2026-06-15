#!/bin/bash
#
# Quick start: OEM Sync to LuckFox (fast iteration)
#
# Usage:
#   ./oem-sync.sh [DEVICE_IP]
#
# Default device: 192.168.100.1
# Example: ./oem-sync.sh 192.168.1.50
#

set -e

DEVICE_IP="${1:-192.168.100.1}"
PROJECT_ROOT="$(cd "$(dirname "$0")/../../" && pwd)"

echo "=================================================="
echo "ULAMA → LuckFox OEM Sync"
echo "=================================================="
echo ""
echo "Device:    $DEVICE_IP"
echo "OEM path:  $PROJECT_ROOT/output/out/oem"
echo ""

# Step 1: Build
echo "[1/3] Building ULAMA..."
cd "$PROJECT_ROOT/media/ulama"
./build.sh > /dev/null 2>&1
echo "✓ Build complete"

# Step 2: Sync
echo "[2/3] Syncing to device..."
cd "$PROJECT_ROOT"
./build.sh sync --host "$DEVICE_IP" 2>&1 | tail -5
echo "✓ Sync complete"

# Step 3: Verify
echo "[3/3] Verifying deployment..."
ssh -o ConnectTimeout=5 "root@$DEVICE_IP" "/oem/usr/bin/ulamad --help" > /dev/null 2>&1 && {
    echo "✓ ulamad is deployed and accessible"
} || {
    echo "⚠ Warning: Could not verify deployment"
}

echo ""
echo "=================================================="
echo "Ready for testing!"
echo "=================================================="
echo ""
echo "Next: ssh root@$DEVICE_IP"
echo ""
