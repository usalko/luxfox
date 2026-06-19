#!/bin/bash

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../" && pwd)"
OUTPUT_ROOT="$PROJECT_ROOT/output/out"

MEDIA_ROOT_STAGING="$OUTPUT_ROOT/media_out/root"
OEM_STAGING="$OUTPUT_ROOT/oem"

ROOTFS_INIT_DIR="$MEDIA_ROOT_STAGING/etc/init.d"
OEM_BIN_DIR="$OEM_STAGING/usr/bin"

install_executable() {
    local src="$1"
    local dst="$2"
    mkdir -p "$(dirname "$dst")"
    cp -f "$src" "$dst"
    chmod +x "$dst"
}

echo "=== USB Watchdog: staging for firmware packaging ==="

echo "Staging usb-watchdog.sh -> $OEM_BIN_DIR/"
install_executable "$SCRIPT_DIR/scripts/usb-watchdog.sh" "$OEM_BIN_DIR/usb-watchdog.sh"

echo "Staging S90usb-watchdog -> $ROOTFS_INIT_DIR/"
install_executable "$SCRIPT_DIR/scripts/S90usb-watchdog" "$ROOTFS_INIT_DIR/S90usb-watchdog"

echo "✓ USB Watchdog staging complete"
echo "  - Watchdog script: $OEM_BIN_DIR/usb-watchdog.sh"
echo "  - Init service:    $ROOTFS_INIT_DIR/S90usb-watchdog"
