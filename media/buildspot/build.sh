#!/bin/bash

# buildspot build script for Luckfox Pico Ultra
# Deploys service scripts to the output directory for inclusion in the firmware image.

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../" && pwd)"
OUTPUT_ROOT="$PROJECT_ROOT/output/out"

# Paths on device
DEVICE_BIN_DIR="$OUTPUT_ROOT/oem/usr/bin"
DEVICE_INIT_DIR="$OUTPUT_ROOT/etc/init.d"

echo "=== Buildspot: Deploying to output directory ==="

# Create directories if they don't exist
mkdir -p "$DEVICE_BIN_DIR"
mkdir -p "$DEVICE_INIT_DIR"

# 1. Copy receiver script
echo "Deploying buildspot-recv.sh to $DEVICE_BIN_DIR"
cp "$SCRIPT_DIR/buildspot-recv.sh" "$DEVICE_BIN_DIR/buildspot-recv.sh"
chmod +x "$DEVICE_BIN_DIR/buildspot-recv.sh"

# 2. Copy init.d script
echo "Deploying S99buildspot-recv to $DEVICE_INIT_DIR"
cp "$SCRIPT_DIR/S99buildspot-recv" "$DEVICE_INIT_DIR/S99buildspot-recv"
chmod +x "$DEVICE_INIT_DIR/S99buildspot-recv"

echo "✓ Buildspot deployment complete"
