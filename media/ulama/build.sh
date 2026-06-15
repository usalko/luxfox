#!/bin/bash

##############################################################################
# build.sh — Build ULAMA and stage files for flashing to LuckFox
#
# Builds ULAMA and stages files into the same directories that `./flash.sh`
# packages into rootfs.img.
#
# Usage:
#   ./build.sh [OPTIONS]
#
# Options:
#   -h, --help              Show this help
#   -c, --clean             Clean build artifacts
#   -v, --verbose           Verbose output
#
# Examples:
#   ./build.sh                      # Build all targets and stage files
#   ./build.sh -c                   # Clean and rebuild
#
##############################################################################

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
ULAMA_ROOT="$SCRIPT_DIR"
UNOW_ROOT="$SCRIPT_DIR/../unow"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../" && pwd)"
OUTPUT_ROOT="$PROJECT_ROOT/output/out"

# Firmware staging paths (same as used by main SDK build)
# - output/out/media_out/root -> copied into rootfs during build_firmware
MEDIA_ROOT_STAGING="$OUTPUT_ROOT/media_out/root"

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Options
CLEAN=0
VERBOSE=0

# Logging functions
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_debug() {
    if [ $VERBOSE -eq 1 ]; then
        echo -e "${BLUE}[DEBUG]${NC} $1"
    fi
}

print_usage() {
    grep "^#" "$0" | grep -E "^# " | sed 's/^# //' | head -30
}

##############################################################################
# CLEAN
##############################################################################
do_clean() {
    log_info "Cleaning build artifacts..."
    
    log_debug "Removing build/ and out/"
    rm -rf "$ULAMA_ROOT/build" "$ULAMA_ROOT/out"
    
    log_info "Clean complete"
}

##############################################################################
# BUILD
##############################################################################
do_build() {
    log_info "=========================================="
    log_info "Building ULAMA"
    log_info "=========================================="
    
    cd "$ULAMA_ROOT"
    
    log_info "Step 1/4: make host"
    make host || {
        log_error "make host failed"
        return 1
    }
    
    log_info "Step 2/4: make host-unow"
    make host-unow || {
        log_error "make host-unow failed"
        return 1
    }
    
    log_info "Step 3/4: make (ARM build)"
    make || {
        log_error "make failed"
        return 1
    }
    
    return 0
}

##############################################################################
# STAGE FILES FOR FIRMWARE PACKAGING
##############################################################################
stage_files() {
    log_info "=========================================="
    log_info "Staging files for firmware packaging"
    log_info "=========================================="
    
    log_info "Rootfs staging: $MEDIA_ROOT_STAGING"
    
    # Create staging directories
    mkdir -p "$MEDIA_ROOT_STAGING/usr/bin/scripts"
    mkdir -p "$MEDIA_ROOT_STAGING/etc/init.d"
    
    # Stage /usr/bin/ulamad
    log_debug "Staging /usr/bin/ulamad"
    if [ ! -f "$ULAMA_ROOT/out/bin/ulamad" ]; then
        log_error "Binary not found: $ULAMA_ROOT/out/bin/ulamad"
        return 1
    fi
    cp -f "$ULAMA_ROOT/out/bin/ulamad" "$MEDIA_ROOT_STAGING/usr/bin/ulamad"
    chmod +x "$MEDIA_ROOT_STAGING/usr/bin/ulamad"
    
    # Stage /etc/ulama.conf
    log_debug "Staging /etc/ulama.conf"
    if [ ! -f "$ULAMA_ROOT/defaults/ulama.conf" ]; then
        log_error "Config not found: $ULAMA_ROOT/defaults/ulama.conf"
        return 1
    fi
    cp -f "$ULAMA_ROOT/defaults/ulama.conf" "$MEDIA_ROOT_STAGING/etc/ulama.conf"
    
    # Stage /etc/init.d/S99ulama
    log_debug "Staging /etc/init.d/S99ulama"
    if [ ! -f "$ULAMA_ROOT/scripts/S99ulama" ]; then
        log_error "Init script not found: $ULAMA_ROOT/scripts/S99ulama"
        return 1
    fi
    cp -f "$ULAMA_ROOT/scripts/S99ulama" "$MEDIA_ROOT_STAGING/etc/init.d/S99ulama"
    chmod +x "$MEDIA_ROOT_STAGING/etc/init.d/S99ulama"
    
    # Stage /usr/bin/scripts/unow-mon.sh
    log_debug "Staging /usr/bin/scripts/unow-mon.sh"
    if [ ! -f "$UNOW_ROOT/scripts/unow-mon.sh" ]; then
        log_error "Script not found: $UNOW_ROOT/scripts/unow-mon.sh"
        return 1
    fi
    cp -f "$UNOW_ROOT/scripts/unow-mon.sh" "$MEDIA_ROOT_STAGING/usr/bin/scripts/unow-mon.sh"
    chmod +x "$MEDIA_ROOT_STAGING/usr/bin/scripts/unow-mon.sh"
    
    # Stage /usr/bin/scripts/unow-down.sh
    log_debug "Staging /usr/bin/scripts/unow-down.sh"
    if [ ! -f "$UNOW_ROOT/scripts/unow-down.sh" ]; then
        log_error "Script not found: $UNOW_ROOT/scripts/unow-down.sh"
        return 1
    fi
    cp -f "$UNOW_ROOT/scripts/unow-down.sh" "$MEDIA_ROOT_STAGING/usr/bin/scripts/unow-down.sh"
    chmod +x "$MEDIA_ROOT_STAGING/usr/bin/scripts/unow-down.sh"
    
    log_info "✓ Files staged successfully:"
    log_info "  - usr/bin/ulamad"
    log_info "  - etc/ulama.conf"
    log_info "  - etc/init.d/S99ulama"
    log_info "  - usr/bin/scripts/unow-mon.sh"
    log_info "  - usr/bin/scripts/unow-down.sh"
    
    return 0
}

##############################################################################
# POST-BUILD SUMMARY
##############################################################################
print_summary() {
    log_info "=========================================="
    log_info "Build Summary"
    log_info "=========================================="
    
    if [ -f "$ULAMA_ROOT/out/bin/ulamad" ]; then
        echo ""
        echo -e "${GREEN}✓ ULAMA Build Complete${NC}"
        echo "  Binary:  $ULAMA_ROOT/out/bin/ulamad"
        echo "  Library: $ULAMA_ROOT/libulama.a"
    fi
    
    if [ -d "$MEDIA_ROOT_STAGING" ]; then
        echo ""
        echo -e "${GREEN}✓ Files Staged for Flashing${NC}"
        echo "  Staging: $MEDIA_ROOT_STAGING"
    fi
    
    echo ""
}

print_next_steps() {
    echo -e "${BLUE}═══════════════════════════════════════════════════${NC}"
    echo -e "${BLUE}Next Steps:${NC}"
    echo -e "${BLUE}═══════════════════════════════════════════════════${NC}"
    echo ""
    echo "• Files are staged in: $MEDIA_ROOT_STAGING"
    echo "• To flash to device: $PROJECT_ROOT/flash.sh"
    echo ""
}

##############################################################################
# MAIN
##############################################################################
main() {
    # Parse arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                print_usage
                exit 0
                ;;
            -c|--clean)
                CLEAN=1
                shift
                ;;
            -v|--verbose)
                VERBOSE=1
                shift
                ;;
            *)
                log_error "Unknown option: $1"
                print_usage
                exit 1
                ;;
        esac
    done
    
    log_debug "CLEAN=$CLEAN"
    log_debug "VERBOSE=$VERBOSE"
    
    # Clean if requested
    if [ $CLEAN -eq 1 ]; then
        do_clean
    fi
    
    # Build
    if ! do_build; then
        log_error "Build failed!"
        return 1
    fi
    
    # Stage files
    if ! stage_files; then
        log_error "Failed to stage files for flashing!"
        return 1
    fi
    
    # Print summary
    print_summary
    print_next_steps
    return 0
}

# Run main
main "$@"
