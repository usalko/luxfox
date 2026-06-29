#!/bin/bash

##############################################################################
# build.sh — Build all subprojects, stage, sync to device
#
# Builds via Makefile (cross-compiler from Makefile.param), stages:
#   out/bin/*  → OEM only    (/oem/usr/bin/)
#   out/lib/*  → OEM only    (/oem/usr/lib/)
#   out/etc/*  → rootfs only (/etc/)
#
# Always syncs to device at the end.
##############################################################################

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../" && pwd)"
OUTPUT_ROOT="$PROJECT_ROOT/output/out"

OEM_STAGING="$OUTPUT_ROOT/oem"
MEDIA_ROOT_STAGING="$OUTPUT_ROOT/media_out/root"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

##############################################################################
# Stage out/ into OEM & rootfs — no duplication
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
# Increment ulama build number and regenerate version header
##############################################################################
bump_ulama_version() {
    local version_file="$SCRIPT_DIR/ulama/include/ulama/ulama_version.h"
    local build_number_file="$SCRIPT_DIR/ulama/.build_number"
    local build_num=0

    if [ -f "$build_number_file" ]; then
        build_num=$(($(cat "$build_number_file") + 1))
    fi
    echo "$build_num" > "$build_number_file"

    local git_hash=""
    local git_branch=""
    if git rev-parse --git-dir > /dev/null 2>&1; then
        git_hash=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
        git_branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")
    fi

    cat > "$version_file" << EOF
#ifndef ULAMA_VERSION_H
#define ULAMA_VERSION_H

#define ULAMA_BUILD_NUMBER    $build_num
#define ULAMA_GIT_HASH        "$git_hash"
#define ULAMA_GIT_BRANCH      "$git_branch"
#define ULAMA_BUILD_DATE      "$(date '+%Y-%m-%d %H:%M:%S')"

#endif /* ULAMA_VERSION_H */
EOF

    log_info "Build #$build_num ($git_branch@$git_hash)"
}

##############################################################################
# Build all subprojects via Makefile (cross-compiler from Makefile.param)
#
# Order matters: ulama and unow are libraries used by vcpd/radiod.
# Projects with build.sh (buildspot, etc) run their own scripts.
# Our three radio projects always use make for consistent cross-compilation.
##############################################################################

# Radio projects — always build via make, specific order
RADIO_PROJECTS="unow ulama vcpd radiod ulama-gw"

log_info "═══ Bumping version ═══"
bump_ulama_version

for dir in "$SCRIPT_DIR"/*/; do
    [ -d "$dir" ] || continue
    name=$(basename "$dir")

    # Radio projects handled separately below
    for skip in $RADIO_PROJECTS; do
        [ "$name" = "$skip" ] && continue 2
    done

    if [ -x "$dir/build.sh" ]; then
        log_info "═══ $name (build.sh) ═══"
        (cd "$dir" && ./build.sh)
    elif [ -f "$dir/Makefile" ]; then
        log_info "═══ $name (make) ═══"
        make -C "$dir"
        stage_project_out "$dir/out" "$name"
    fi
done

# Build radio projects in correct order via make
for name in $RADIO_PROJECTS; do
    dir="$SCRIPT_DIR/$name"
    [ -d "$dir" ] || continue
    [ -f "$dir/Makefile" ] || continue

    log_info "═══ $name (make) ═══"
    make -C "$dir" || { log_error "$name build FAILED"; exit 1; }
    stage_project_out "$dir/out" "$name"
done

##############################################################################
# Build host-side tools (for ground station)
##############################################################################
if [ -f "$SCRIPT_DIR/ulama-gw/Makefile" ]; then
    log_info "═══ ulama-gw (host-unow) ═══"
    make -C "$SCRIPT_DIR/ulama-gw" host-unow 2>/dev/null || log_warn "host-unow failed (libpcap-dev missing?)"
fi

##############################################################################
# Update rootfs staging init scripts
##############################################################################
ROOTFS_STAGING="$OUTPUT_ROOT/rootfs_uclibc_rv1106"
if [ -d "$ROOTFS_STAGING/etc/init.d" ]; then
    log_info "Updating rootfs init scripts..."
    for initscript in "$SCRIPT_DIR"/*/scripts/S[0-9][0-9]*; do
        [ -f "$initscript" ] || continue
        cp -f "$initscript" "$ROOTFS_STAGING/etc/init.d/"
        chmod +x "$ROOTFS_STAGING/etc/init.d/$(basename "$initscript")"
    done
fi

##############################################################################
# Verify critical binaries exist in staging
##############################################################################
log_info "═══ Verifying staging ═══"
FAIL=0
for bin in radiod ulamad vcpd; do
    if [ -f "$OEM_STAGING/usr/bin/$bin" ]; then
        log_info "  ✓ $bin"
    else
        log_error "  ✗ $bin MISSING in staging!"
        FAIL=1
    fi
done
[ $FAIL -eq 0 ] || { log_error "Staging incomplete, aborting sync"; exit 1; }

##############################################################################
# Sync to device
##############################################################################
log_info "═══ Syncing to device ═══"
cd "$PROJECT_ROOT" && ./build.sh sync

log_info "✓ Done"
