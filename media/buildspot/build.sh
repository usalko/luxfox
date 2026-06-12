#!/bin/bash

set -e

# buildspot build script for Luckfox Pico Ultra
# Stages files into the same directories that `./build.sh firmware` packages
# into rootfs.img and oem.img.

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../" && pwd)"
OUTPUT_ROOT="$PROJECT_ROOT/output/out"

# Firmware staging paths used by the main SDK build:
# - output/out/media_out/root -> copied into rootfs during build_firmware
# - output/out/oem            -> packed into oem.img (or copied into /oem)
MEDIA_ROOT_STAGING="$OUTPUT_ROOT/media_out/root"
OEM_STAGING="$OUTPUT_ROOT/oem"

ROOTFS_INIT_DIR="$MEDIA_ROOT_STAGING/etc/init.d"
OEM_BIN_DIR="$OEM_STAGING/usr/bin"
LEGACY_INIT_PATH="$OUTPUT_ROOT/etc/init.d/S99buildspot-recv"
LEGACY_ROOTFS_SERVICE="$ROOTFS_INIT_DIR/S99buildspot-recv"
LEGACY_OEM_RECEIVER="$OEM_BIN_DIR/buildspot-recv.sh"

install_executable() {
	local src="$1"
	local dst="$2"

	mkdir -p "$(dirname "$dst")"
	cp -f "$src" "$dst"
	chmod +x "$dst"
}

echo "=== Buildspot: staging files for firmware packaging ==="
echo "Rootfs staging: $MEDIA_ROOT_STAGING"
echo "OEM staging:    $OEM_STAGING"

# The new MVP uses an on-demand foreground agent over stdio instead of a
# background receiver service on the console UART.
echo "Staging buildspot-agent.sh -> $OEM_BIN_DIR/buildspot-agent.sh"
install_executable "$SCRIPT_DIR/buildspot-agent.sh" "$OEM_BIN_DIR/buildspot-agent.sh"

# Remove legacy staged files so the console UART is no longer owned by a boot
# service and old layouts do not linger across rebuilds.
if [ -f "$LEGACY_INIT_PATH" ]; then
	echo "Removing stale legacy staging file: $LEGACY_INIT_PATH"
	rm -f "$LEGACY_INIT_PATH"
fi

if [ -f "$LEGACY_ROOTFS_SERVICE" ]; then
	echo "Removing legacy rootfs service: $LEGACY_ROOTFS_SERVICE"
	rm -f "$LEGACY_ROOTFS_SERVICE"
fi

if [ -f "$LEGACY_OEM_RECEIVER" ]; then
	echo "Removing legacy OEM receiver: $LEGACY_OEM_RECEIVER"
	rm -f "$LEGACY_OEM_RECEIVER"
fi

echo "✓ Buildspot staging complete"
echo "  - oem pickup:    $OEM_BIN_DIR/buildspot-agent.sh"
echo "  Next step: run ./flash.sh (or ./build.sh firmware) to rebuild images."
