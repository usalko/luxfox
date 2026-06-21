#!/bin/bash

##############################################################################
# build.sh — Build and deploy all umbrella projects: ULAMA, UNOW, VCPD
#
# Stages all artifacts into OEM and rootfs staging directories, then syncs
# to the device via SSH.
#
# Both deployment paths work after running this script:
#   - ./flash.sh          (full firmware flash via USB)
#   - ./build.sh sync     (quick SSH sync of OEM partition)
##############################################################################

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../" && pwd)"
OUTPUT_ROOT="$PROJECT_ROOT/output/out"

OEM_STAGING="$OUTPUT_ROOT/oem"
MEDIA_ROOT_STAGING="$OUTPUT_ROOT/media_out/root"

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

stage_file() {
    local src="$1" dst="$2"
    mkdir -p "$(dirname "$dst")"
    cp -f "$src" "$dst"
}

stage_executable() {
    stage_file "$1" "$2"
    chmod +x "$2"
}

##############################################################################
# 1. ULAMA (builds ulama + ulama-gw, stages ulama + unow scripts)
##############################################################################
log_info "═══ [1/3] Building ULAMA ═══"
cd "$SCRIPT_DIR/ulama" && ./build.sh

##############################################################################
# 2. UNOW (builds unow library + diagnostic tools)
##############################################################################
log_info "═══ [2/3] Building UNOW ═══"
cd "$SCRIPT_DIR/unow" && ./build.sh

##############################################################################
# 3. VCPD (build + stage)
##############################################################################
log_info "═══ [3/3] Building VCPD ═══"
cd "$SCRIPT_DIR"
make -C "$SCRIPT_DIR/vcpd"

VCPD_OUT="$SCRIPT_DIR/vcpd/out"

if [ ! -f "$VCPD_OUT/bin/vcpd" ]; then
    log_error "vcpd binary not found: $VCPD_OUT/bin/vcpd"
    exit 1
fi

log_info "Staging VCPD artifacts..."
for dir in "$OEM_STAGING" "$MEDIA_ROOT_STAGING"; do
    stage_executable "$VCPD_OUT/bin/vcpd"              "$dir/usr/bin/vcpd"
    stage_file       "$VCPD_OUT/etc/vcpd.conf"         "$dir/etc/vcpd.conf"
    stage_executable "$VCPD_OUT/etc/init.d/S97vcpd"    "$dir/etc/init.d/S97vcpd"
done

log_info "✓ VCPD staged"

##############################################################################
# 4. Buildspot (SSH infrastructure for sync)
##############################################################################
log_info "═══ Staging buildspot (SSH) ═══"
cd "$SCRIPT_DIR/buildspot" && ./build.sh

##############################################################################
# 5. Sync to device via SSH
##############################################################################
log_info "═══ Syncing to device ═══"
cd "$PROJECT_ROOT" && ./build.sh sync

log_info "✓ All umbrella projects built and deployed: ulama, unow, vcpd"
