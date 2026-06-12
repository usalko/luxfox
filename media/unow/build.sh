#!/bin/bash

##############################################################################
# build.sh — Main build wrapper for UNOW project
#
# Supports:
#   - Building for host (x86_64 Ubuntu)
#   - Building for ARM (LuckFox)
#   - Dependency checking
#   - Local installation
#
# Usage:
#   ./build.sh [OPTIONS]
#
# Options:
#   -h, --help              Show this help
#   -t, --target [host|arm|all]
#                           Build target (default: all)
#   -j, --jobs N            Parallel build jobs (default: auto)
#   -v, --verbose           Verbose output
#   -c, --clean             Clean before build
#   --check-deps            Check dependencies only
#   --install-local         Install to ./out/ (default: true)
#   --install-media         Install to ../../output/out/media_out/
#
# Examples:
#   ./build.sh                      # Build all targets, default
#   ./build.sh -t host              # Build for x86_64 only
#   ./build.sh -t arm --check-deps  # Check ARM dependencies
#   ./build.sh -c -t all            # Clean and rebuild everything
##############################################################################

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
UNOW_ROOT="$SCRIPT_DIR"

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Options
BUILD_TARGET="all"
PARALLEL_JOBS=$(nproc 2>/dev/null || echo 4)
VERBOSE=0
CLEAN=0
CHECK_DEPS_ONLY=0
INSTALL_LOCAL=1
INSTALL_MEDIA=1

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
    grep "^#" "$0" | grep -E "^# " | sed 's/^# //' | head -40
}

##############################################################################
# DEPENDENCY CHECKING
##############################################################################
check_host_deps() {
    log_info "Checking HOST (x86_64) dependencies..."
    
    local missing=0
    
    # Check compiler
    if ! command -v gcc &> /dev/null; then
        log_error "gcc not found. Install with: sudo apt install build-essential"
        missing=1
    else
        log_info "  ✓ gcc $(gcc --version | head -1 | grep -oP '[0-9]+\.[0-9]+' | head -1)"
    fi
    
    # Check libpcap
    if ! pkg-config --exists libpcap 2>/dev/null && [ ! -f /usr/include/pcap/pcap.h ]; then
        log_warn "libpcap-dev not found. Install with: sudo apt install libpcap-dev"
        log_warn "  Building without pcap support (tests may fail)"
    else
        log_info "  ✓ libpcap found"
    fi
    
    # Check make
    if ! command -v make &> /dev/null; then
        log_error "make not found. Install with: sudo apt install make"
        missing=1
    else
        log_info "  ✓ make $(make --version | head -1 | grep -oP '[0-9]+\.[0-9]+' | head -1)"
    fi
    
    return $missing
}

check_arm_deps() {
    log_info "Checking ARM (LuckFox) dependencies..."
    
    local missing=0
    
    # Check for Makefile.param
    if [ ! -f "$UNOW_ROOT/../Makefile.param" ]; then
        log_error "Makefile.param not found in media/"
        log_error "Expected: $UNOW_ROOT/../Makefile.param"
        missing=1
    else
        log_info "  ✓ Makefile.param found"
        
        # Try to extract RK_MEDIA_CROSS using make
        local RK_MEDIA_CROSS
        RK_MEDIA_CROSS=$(cd "$UNOW_ROOT/.." && make -f Makefile.param print-cross 2>/dev/null || echo "")
        
        # If that doesn't work, try to extract from file directly
        if [ -z "$RK_MEDIA_CROSS" ]; then
            RK_MEDIA_CROSS=$(grep "CONFIG_RK_MEDIA_CROSS" "$UNOW_ROOT/../cfg/cfg.mk" 2>/dev/null | head -1 | cut -d'=' -f2 | tr -d ' ')
        fi
        
        if [ -z "$RK_MEDIA_CROSS" ]; then
            # Try environment variable
            RK_MEDIA_CROSS="${RK_MEDIA_CROSS:-}"
        fi
        
        if [ -z "$RK_MEDIA_CROSS" ]; then
            log_warn "RK_MEDIA_CROSS not set or cannot be detected"
            log_warn "  Set it manually: export RK_MEDIA_CROSS=arm-buildroot-linux-uclibcgnueabihf"
            log_warn "  Or ensure Makefile.param is properly sourced"
            # Don't fail here - user may set it manually
        else
            log_info "  ✓ Cross-compiler: $RK_MEDIA_CROSS"
            
            # Check if cross-compiler exists
            if ! command -v "${RK_MEDIA_CROSS}-gcc" &> /dev/null; then
                log_warn "Cross-compiler ${RK_MEDIA_CROSS}-gcc not found in PATH"
                log_warn "  Try: export PATH=/path/to/toolchain/bin:\$PATH"
                missing=1
            else
                log_info "  ✓ ${RK_MEDIA_CROSS}-gcc available"
            fi
        fi
    fi
    
    return $missing
}

check_all_deps() {
    local failed=0
    
    log_info "=========================================="
    log_info "Checking dependencies..."
    log_info "=========================================="
    
    case "$BUILD_TARGET" in
        host|all)
            check_host_deps || failed=1
            ;;
    esac
    
    case "$BUILD_TARGET" in
        arm|all)
            check_arm_deps || failed=1
            ;;
    esac
    
    if [ $failed -eq 0 ]; then
        log_info "✓ All dependencies satisfied"
        return 0
    else
        log_error "✗ Some dependencies are missing"
        return 1
    fi
}

##############################################################################
# CLEAN
##############################################################################
do_clean() {
    log_info "Cleaning build artifacts..."
    
    case "$BUILD_TARGET" in
        host)
            log_debug "Removing build/host/ and out/host/"
            rm -rf "$UNOW_ROOT/build/host" "$UNOW_ROOT/out/host"
            ;;
        arm)
            log_debug "Removing build/arm/ and out/arm/"
            rm -rf "$UNOW_ROOT/build/arm" "$UNOW_ROOT/out/arm"
            ;;
        all)
            log_debug "Removing build/ and out/"
            rm -rf "$UNOW_ROOT/build" "$UNOW_ROOT/out"
            ;;
    esac
    
    log_info "Clean complete"
}

##############################################################################
# BUILD
##############################################################################
do_build() {
    log_info "=========================================="
    log_info "Building UNOW (target: $BUILD_TARGET)"
    log_info "=========================================="
    
    if [ ! -x "$UNOW_ROOT/build-artifacts.sh" ]; then
        log_error "build-artifacts.sh not found or not executable"
        log_error "Expected: $UNOW_ROOT/build-artifacts.sh"
        return 1
    fi
    
    # Call the build script
    bash "$UNOW_ROOT/build-artifacts.sh" "$BUILD_TARGET"
    
    return $?
}

##############################################################################
# POST-BUILD SUMMARY
##############################################################################
print_summary() {
    log_info "=========================================="
    log_info "Build Summary"
    log_info "=========================================="
    
    case "$BUILD_TARGET" in
        host|all)
            if [ -d "$UNOW_ROOT/out/host" ]; then
                echo ""
                echo -e "${GREEN}✓ HOST (x86_64) Build${NC}"
                [ -f "$UNOW_ROOT/out/host/bin/unow_diag" ] && echo "  Binary:  $UNOW_ROOT/out/host/bin/unow_diag"
                [ -f "$UNOW_ROOT/out/host/lib/libunow.so" ] && echo "  Shared:  $UNOW_ROOT/out/host/lib/libunow.so"
                [ -f "$UNOW_ROOT/out/host/lib/libunow.a" ] && echo "  Static:  $UNOW_ROOT/out/host/lib/libunow.a"
            fi
            ;;
    esac
    
    case "$BUILD_TARGET" in
        arm|all)
            if [ -d "$UNOW_ROOT/out/arm" ]; then
                echo ""
                echo -e "${GREEN}✓ ARM (LuckFox) Build${NC}"
                [ -f "$UNOW_ROOT/out/arm/bin/unow_diag" ] && echo "  Binary:  $UNOW_ROOT/out/arm/bin/unow_diag"
                [ -f "$UNOW_ROOT/out/arm/lib/libunow.so" ] && echo "  Shared:  $UNOW_ROOT/out/arm/lib/libunow.so"
                [ -f "$UNOW_ROOT/out/arm/lib/libunow.a" ] && echo "  Static:  $UNOW_ROOT/out/arm/lib/libunow.a"
            fi
            ;;
    esac
    
    echo ""
    log_info "Output location: ../../output/out/media_out/"
    echo "  Binary:  bin/x86_64/ and bin/arm/"
    echo "  Library: lib/x86_64/ and lib/arm/"
    echo "  Headers: include/unow/"
    echo ""
}

print_next_steps() {
    echo -e "${BLUE}═══════════════════════════════════════════════════${NC}"
    echo -e "${BLUE}Next Steps:${NC}"
    echo -e "${BLUE}═══════════════════════════════════════════════════${NC}"
    echo ""
    echo "1. On your LuckFox device:"
    echo "   $ ssh <luckfox-ip>"
    echo "   $ bash media/unow/scripts/unow-mon.sh <wifi-adapter> mon0 6"
    echo "   $ ./media/unow/out/bin/unow_diag --iface mon0 --node-id 1 --listen 10"
    echo ""
    echo "2. On your Desktop (in another terminal):"
    echo "   $ bash media/unow/RUNME_G0_TEST.sh desktop tx"
    echo ""
    echo "3. Open the full UNOW guide:"
    echo "   $ less media/unow/README.md"
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
            -t|--target)
                BUILD_TARGET="$2"
                shift 2
                ;;
            -j|--jobs)
                PARALLEL_JOBS="$2"
                shift 2
                ;;
            -v|--verbose)
                VERBOSE=1
                shift
                ;;
            -c|--clean)
                CLEAN=1
                shift
                ;;
            --check-deps)
                CHECK_DEPS_ONLY=1
                shift
                ;;
            --install-local)
                INSTALL_LOCAL=1
                shift
                ;;
            --install-media)
                INSTALL_MEDIA=1
                shift
                ;;
            *)
                log_error "Unknown option: $1"
                print_usage
                exit 1
                ;;
        esac
    done
    
    # Validate target
    case "$BUILD_TARGET" in
        host|arm|all)
            ;;
        *)
            log_error "Invalid target: $BUILD_TARGET"
            exit 1
            ;;
    esac
    
    log_debug "BUILD_TARGET=$BUILD_TARGET"
    log_debug "PARALLEL_JOBS=$PARALLEL_JOBS"
    log_debug "VERBOSE=$VERBOSE"
    log_debug "CLEAN=$CLEAN"
    
    # Check dependencies
    if ! check_all_deps; then
        if [ $CHECK_DEPS_ONLY -eq 1 ]; then
            return 1
        fi
        log_warn "Some dependencies are missing, but will attempt to build anyway..."
    fi
    
    if [ $CHECK_DEPS_ONLY -eq 1 ]; then
        log_info "Dependency check complete"
        return 0
    fi
    
    # Clean if requested
    if [ $CLEAN -eq 1 ]; then
        do_clean
    fi
    
    # Build
    if do_build; then
        print_summary
        print_next_steps
        return 0
    else
        log_error "Build failed!"
        return 1
    fi
}

# Run main
main "$@"
