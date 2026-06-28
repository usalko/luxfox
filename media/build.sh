#!/bin/bash

##############################################################################
# build.sh — Auto-discover and build all subprojects, stage, sync to device
#
# For each subdirectory:
#   - has ./build.sh  →  run it (project handles its own staging)
#   - has Makefile     →  run make, then stage out/ into OEM & rootfs
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

##############################################################################
# Stage a Makefile project's out/ — no duplication:
#   out/bin/*          → OEM only    (usr/bin/*)
#   out/lib/*          → OEM only    (usr/lib/*)
#   out/etc/*          → rootfs only (etc/*)
#   out/etc/init.d/S*  → rootfs only (etc/init.d/S*, executable)
##############################################################################
stage_project_out() {
    local out_dir="$1" name="$2"
    [ -d "$out_dir" ] || return 0

    log_info "Staging $name..."

    # Binaries and libraries → OEM partition only
    if [ -d "$out_dir/bin" ]; then
        mkdir -p "$OEM_STAGING/usr/bin"
        cp -f "$out_dir/bin/"* "$OEM_STAGING/usr/bin/" 2>/dev/null || true
        chmod +x "$OEM_STAGING/usr/bin/"* 2>/dev/null || true
    fi
    if [ -d "$out_dir/lib" ]; then
        mkdir -p "$OEM_STAGING/usr/lib"
        cp -rf "$out_dir/lib/"* "$OEM_STAGING/usr/lib/" 2>/dev/null || true
    fi

    # Configs and init scripts → rootfs only
    if [ -d "$out_dir/etc" ]; then
        mkdir -p "$MEDIA_ROOT_STAGING/etc"
        cp -rf "$out_dir/etc/"* "$MEDIA_ROOT_STAGING/etc/"
        find "$MEDIA_ROOT_STAGING/etc/init.d" -maxdepth 1 -type f -name 'S*' \
            -exec chmod +x {} \; 2>/dev/null || true
    fi
}

##############################################################################
# Build all subprojects (auto-discovery)
##############################################################################
for dir in "$SCRIPT_DIR"/*/; do
    [ -d "$dir" ] || continue
    name=$(basename "$dir")

    if [ -x "$dir/build.sh" ]; then
        log_info "═══ $name (build.sh) ═══"
        (cd "$dir" && ./build.sh)
    elif [ -f "$dir/Makefile" ]; then
        log_info "═══ $name (make) ═══"
        make -C "$dir"
        stage_project_out "$dir/out" "$name"
    fi
done

##############################################################################
# Build host-side ulama_gw with UNOW support (requires libpcap-dev)
##############################################################################
if [ -f "$SCRIPT_DIR/ulama-gw/Makefile" ]; then
    log_info "═══ ulama-gw (host-unow) ═══"
    make -C "$SCRIPT_DIR/ulama-gw" clean
    make -C "$SCRIPT_DIR/ulama-gw" host-unow || log_error "host-unow failed (libpcap-dev missing?)"
fi

##############################################################################
# Update rootfs staging init scripts (generic: all scripts/S* from subprojects)
##############################################################################
ROOTFS_STAGING="$OUTPUT_ROOT/rootfs_uclibc_rv1106"
if [ -d "$ROOTFS_STAGING/etc/init.d" ]; then
    log_info "Updating rootfs init scripts..."
    for initscript in "$SCRIPT_DIR"/*/scripts/S[0-9][0-9]*; do
        [ -f "$initscript" ] || continue
        cp -f "$initscript" "$ROOTFS_STAGING/etc/init.d/"
        chmod +x "$ROOTFS_STAGING/etc/init.d/$(basename "$initscript")"
    done
    log_info "✓ rootfs staging updated"
fi

##############################################################################
# Sync to device via SSH
##############################################################################
log_info "═══ Syncing to device ═══"
cd "$PROJECT_ROOT" && ./build.sh sync

log_info "✓ Done"
