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

# Receiver must live under /oem/usr/bin on the target.
echo "Staging buildspot-recv.sh -> $OEM_BIN_DIR/buildspot-recv.sh"
install_executable "$SCRIPT_DIR/buildspot-recv.sh" "$OEM_BIN_DIR/buildspot-recv.sh"

# Init service must live under the media root staging tree so build_firmware
# copies it into rootfs.img as /etc/init.d/S99buildspot-recv.
echo "Staging S99buildspot-recv -> $ROOTFS_INIT_DIR/S99buildspot-recv"
install_executable "$SCRIPT_DIR/S99buildspot-recv" "$ROOTFS_INIT_DIR/S99buildspot-recv"

# Remove the old incorrect staging location so it does not keep misleading users.
if [ -f "$LEGACY_INIT_PATH" ]; then
	echo "Removing stale legacy staging file: $LEGACY_INIT_PATH"
	rm -f "$LEGACY_INIT_PATH"
fi

echo "✓ Buildspot staging complete"
echo "  - rootfs pickup: $ROOTFS_INIT_DIR/S99buildspot-recv"
echo "  - oem pickup:    $OEM_BIN_DIR/buildspot-recv.sh"
echo "  Next step: run ./flash.sh (or ./build.sh firmware) to rebuild images."
