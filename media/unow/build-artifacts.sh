#!/bin/bash

##############################################################################
# build-artifacts.sh — Build UNOW for both x86 (host) and ARM (LuckFox)
#
# Usage:
#   ./build-artifacts.sh [host|arm|all]
#
# Outputs:
#   ../../output/out/media_out/bin/x86_64/{libunow.so, libunow.a, unow_diag}
#   ../../output/out/media_out/bin/arm/{libunow.so, libunow.a, unow_diag}
#   ../../output/out/media_out/include/unow/* (headers)
#   ../../output/out/media_out/lib/x86_64/{libunow.so, libunow.a}
#   ../../output/out/media_out/lib/arm/{libunow.so, libunow.a}
##############################################################################

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
UNOW_ROOT="$SCRIPT_DIR"
OUTPUT_BASE="$SCRIPT_DIR/../../output/out/media_out"

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

##############################################################################
# BUILD HOST (x86_64)
##############################################################################
build_host() {
    log_info "Building for HOST (x86_64)..."
    
    local BUILD_DIR="$UNOW_ROOT/build/host"
    local PKG_OUT="$UNOW_ROOT/out/host"
    
    # Clean previous builds
    rm -rf "$BUILD_DIR" "$PKG_OUT"
    mkdir -p "$BUILD_DIR" "$PKG_OUT/bin" "$PKG_OUT/lib" "$PKG_OUT/include"
    
    # Determine libpcap location for x86
    local LIBPCAP_INCLUDE=""
    local LIBPCAP_LIB=""
    local LIBPCAP_FLAGS=""
    
    if pkg-config --exists libpcap 2>/dev/null; then
        LIBPCAP_INCLUDE=$(pkg-config --cflags libpcap)
        LIBPCAP_LIB=$(pkg-config --libs libpcap)
        log_info "Found libpcap via pkg-config"
    elif [ -f /usr/include/pcap/pcap.h ]; then
        LIBPCAP_INCLUDE="-I/usr/include"
        LIBPCAP_LIB="-lpcap"
        log_info "Found libpcap in /usr/include"
    else
        log_warn "libpcap not found, building without pcap support"
        LIBPCAP_INCLUDE=""
        LIBPCAP_LIB=""
    fi
    
    # Build configuration for host
    local CC="gcc"
    local AR="ar"
    local INC_FLAGS="-I$UNOW_ROOT/include -I$UNOW_ROOT/src $LIBPCAP_INCLUDE"
    local CFLAGS="-D_DEFAULT_SOURCE -g -Wall -Wextra -std=c11 $INC_FLAGS -fPIC"
    local LDLIBS="$LIBPCAP_LIB -lpthread"
    
    # Compile library objects
    for src in "$UNOW_ROOT"/src/*.c; do
        obj="$BUILD_DIR/src/$(basename "$src" .c).o"
        mkdir -p "$(dirname "$obj")"
        log_info "  Compiling $(basename "$src")..."
        $CC -c "$src" $CFLAGS -o "$obj"
    done
    
    # Compile tool objects
    for src in "$UNOW_ROOT"/tools/*.c; do
        obj="$BUILD_DIR/tools/$(basename "$src" .c).o"
        mkdir -p "$(dirname "$obj")"
        log_info "  Compiling $(basename "$src")..."
        $CC -c "$src" $CFLAGS -o "$obj"
    done
    
    # Link shared library
    log_info "  Linking libunow.so..."
    $CC $CFLAGS -shared "$BUILD_DIR"/src/*.o -o "$PKG_OUT/lib/libunow.so" $LDLIBS
    
    # Link static library
    log_info "  Creating libunow.a..."
    $AR -rcs "$PKG_OUT/lib/libunow.a" "$BUILD_DIR"/src/*.o
    
    # Link binary
    log_info "  Linking unow_diag..."
    $CC $CFLAGS "$BUILD_DIR"/tools/*.o -L"$PKG_OUT/lib" -lunow -o "$PKG_OUT/bin/unow_diag" $LDLIBS
    
    # Copy headers and scripts
    cp -rfa "$UNOW_ROOT/include" "$PKG_OUT/"
    mkdir -p "$PKG_OUT/bin/scripts"
    if [ -d "$UNOW_ROOT/scripts" ]; then
        cp -f "$UNOW_ROOT"/scripts/* "$PKG_OUT/bin/scripts/" 2>/dev/null || true
    fi
    
    log_info "Host build complete: $PKG_OUT"
    return 0
}

##############################################################################
# BUILD ARM (for LuckFox)
##############################################################################
build_arm() {
    log_info "Building for ARM (LuckFox)..."
    
    # Source the Makefile.param for cross-compiler config
    local MEDIA_PARAM="$UNOW_ROOT/../Makefile.param"
    if [ ! -f "$MEDIA_PARAM" ]; then
        log_error "Missing $MEDIA_PARAM - cannot find cross-compiler configuration"
        return 1
    fi
    
    # Extract RK_MEDIA_CROSS from cfg/cfg.mk
    local RK_MEDIA_CROSS
    RK_MEDIA_CROSS=$(grep "CONFIG_RK_MEDIA_CROSS" "$UNOW_ROOT/../cfg/cfg.mk" 2>/dev/null | head -1 | cut -d'=' -f2 | tr -d ' ')
    
    if [ -z "$RK_MEDIA_CROSS" ]; then
        # Try environment variable
        RK_MEDIA_CROSS="${RK_MEDIA_CROSS:-}"
    fi
    
    if [ -z "$RK_MEDIA_CROSS" ]; then
        log_error "Cannot determine RK_MEDIA_CROSS from Makefile.param or cfg/cfg.mk"
        return 1
    fi
    
    log_info "Using cross-compiler: $RK_MEDIA_CROSS"
    
    local BUILD_DIR="$UNOW_ROOT/build/arm"
    local PKG_OUT="$UNOW_ROOT/out/arm"
    
    # Clean previous builds
    rm -rf "$BUILD_DIR" "$PKG_OUT"
    mkdir -p "$BUILD_DIR" "$PKG_OUT/bin" "$PKG_OUT/lib" "$PKG_OUT/include"
    
    # Use the existing Makefile for ARM build
    # We'll invoke make with specific variables
    local CC="${RK_MEDIA_CROSS}-gcc"
    local AR="${RK_MEDIA_CROSS}-ar"
    
    # Verify toolchain is available
    if ! command -v "$CC" &> /dev/null; then
        log_error "Cross-compiler not found: $CC"
        log_error "Add to PATH or source Makefile.param: source $MEDIA_PARAM"
        return 1
    fi
    
    # Set up sysroot for pcap if available
    local UNOW_PCAP_SYSROOT="${UNOW_PCAP_SYSROOT:-$UNOW_ROOT/../../sysdrv/source/buildroot/buildroot-2023.02.6/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot}"
    
    local INC_FLAGS="-I$UNOW_ROOT/include -I$UNOW_ROOT/src"
    if [ -d "$UNOW_PCAP_SYSROOT/usr/include/pcap" ]; then
        INC_FLAGS="$INC_FLAGS -I$UNOW_PCAP_SYSROOT/usr/include"
        log_info "  Using sysroot pcap: $UNOW_PCAP_SYSROOT"
    fi
    
    # Get CFLAGS from Makefile.param if available
    local RK_MEDIA_OPTS="-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -ffunction-sections -fdata-sections -Os"
    local CFLAGS="$RK_MEDIA_OPTS -D_DEFAULT_SOURCE -g -Wall -Wextra -std=c11 $INC_FLAGS -fPIC"
    
    local LDFLAGS=""
    if [ -d "$UNOW_PCAP_SYSROOT/usr/lib" ]; then
        LDFLAGS="-L$UNOW_PCAP_SYSROOT/usr/lib -Wl,-rpath-link,$UNOW_PCAP_SYSROOT/usr/lib"
    fi
    local LDLIBS="-lpcap -lpthread"
    
    # Compile library objects
    for src in "$UNOW_ROOT"/src/*.c; do
        obj="$BUILD_DIR/src/$(basename "$src" .c).o"
        mkdir -p "$(dirname "$obj")"
        log_info "  Compiling $(basename "$src")..."
        $CC -c "$src" $CFLAGS -o "$obj"
    done
    
    # Compile tool objects
    for src in "$UNOW_ROOT"/tools/*.c; do
        obj="$BUILD_DIR/tools/$(basename "$src" .c).o"
        mkdir -p "$(dirname "$obj")"
        log_info "  Compiling $(basename "$src")..."
        $CC -c "$src" $CFLAGS -o "$obj"
    done
    
    # Link shared library
    log_info "  Linking libunow.so..."
    $CC $CFLAGS -shared "$BUILD_DIR"/src/*.o -o "$PKG_OUT/lib/libunow.so" $LDFLAGS $LDLIBS
    
    # Link static library
    log_info "  Creating libunow.a..."
    $AR -rcs "$PKG_OUT/lib/libunow.a" "$BUILD_DIR"/src/*.o
    
    # Link binary
    log_info "  Linking unow_diag..."
    $CC $CFLAGS "$BUILD_DIR"/tools/*.o -L"$PKG_OUT/lib" -lunow -o "$PKG_OUT/bin/unow_diag" $LDFLAGS $LDLIBS
    
    # Copy headers and scripts
    cp -rfa "$UNOW_ROOT/include" "$PKG_OUT/"
    mkdir -p "$PKG_OUT/bin/scripts"
    if [ -d "$UNOW_ROOT/scripts" ]; then
        cp -f "$UNOW_ROOT"/scripts/* "$PKG_OUT/bin/scripts/" 2>/dev/null || true
    fi
    
    log_info "ARM build complete: $PKG_OUT"
    return 0
}

##############################################################################
# INSTALL ARTIFACTS TO OUTPUT
##############################################################################
install_artifacts() {
    log_info "Installing artifacts to $OUTPUT_BASE..."
    
    mkdir -p "$OUTPUT_BASE/bin/x86_64" "$OUTPUT_BASE/bin/arm"
    mkdir -p "$OUTPUT_BASE/lib/x86_64" "$OUTPUT_BASE/lib/arm"
    mkdir -p "$OUTPUT_BASE/include"
    
    # Install host (x86_64) artifacts
    if [ -d "$UNOW_ROOT/out/host" ]; then
        log_info "Installing x86_64 artifacts..."
        cp -f "$UNOW_ROOT/out/host/bin/unow_diag" "$OUTPUT_BASE/bin/x86_64/" 2>/dev/null || true
        cp -f "$UNOW_ROOT/out/host/lib/libunow.so" "$OUTPUT_BASE/lib/x86_64/" 2>/dev/null || true
        cp -f "$UNOW_ROOT/out/host/lib/libunow.a" "$OUTPUT_BASE/lib/x86_64/" 2>/dev/null || true
    fi
    
    # Install ARM artifacts
    if [ -d "$UNOW_ROOT/out/arm" ]; then
        log_info "Installing ARM artifacts..."
        cp -f "$UNOW_ROOT/out/arm/bin/unow_diag" "$OUTPUT_BASE/bin/arm/" 2>/dev/null || true
        cp -f "$UNOW_ROOT/out/arm/lib/libunow.so" "$OUTPUT_BASE/lib/arm/" 2>/dev/null || true
        cp -f "$UNOW_ROOT/out/arm/lib/libunow.a" "$OUTPUT_BASE/lib/arm/" 2>/dev/null || true
    fi
    
    # Install headers (common)
    if [ -d "$UNOW_ROOT/out/host/include" ]; then
        cp -rfa "$UNOW_ROOT/out/host/include/unow" "$OUTPUT_BASE/include/" 2>/dev/null || true
    elif [ -d "$UNOW_ROOT/out/arm/include" ]; then
        cp -rfa "$UNOW_ROOT/out/arm/include/unow" "$OUTPUT_BASE/include/" 2>/dev/null || true
    fi
    
    # Also install to traditional location for compatibility
    log_info "Installing to local out/ folder..."
    mkdir -p "$UNOW_ROOT/out/bin" "$UNOW_ROOT/out/lib"
    [ -f "$OUTPUT_BASE/bin/arm/unow_diag" ] && cp -f "$OUTPUT_BASE/bin/arm/unow_diag" "$UNOW_ROOT/out/bin/" || true
    [ -f "$OUTPUT_BASE/lib/arm/libunow.so" ] && cp -f "$OUTPUT_BASE/lib/arm/libunow.so" "$UNOW_ROOT/out/lib/" || true
    [ -f "$OUTPUT_BASE/lib/arm/libunow.a" ] && cp -f "$OUTPUT_BASE/lib/arm/libunow.a" "$UNOW_ROOT/out/lib/" || true
    
    log_info "Installation complete!"
    
    # Print summary
    echo ""
    log_info "Build artifacts summary:"
    echo "  x86_64 binary:  $OUTPUT_BASE/bin/x86_64/unow_diag"
    echo "  x86_64 libs:    $OUTPUT_BASE/lib/x86_64/"
    echo "  ARM binary:     $OUTPUT_BASE/bin/arm/unow_diag"
    echo "  ARM libs:       $OUTPUT_BASE/lib/arm/"
    echo "  Headers:        $OUTPUT_BASE/include/unow/"
    echo ""
}

##############################################################################
# MAIN
##############################################################################
main() {
    local BUILD_TARGET="${1:-all}"
    
    case "$BUILD_TARGET" in
        host)
            build_host
            ;;
        arm)
            build_arm
            ;;
        all)
            build_host && build_arm
            ;;
        *)
            log_error "Invalid target: $BUILD_TARGET"
            echo "Usage: $0 [host|arm|all]"
            exit 1
            ;;
    esac
    
    install_artifacts
    log_info "Done!"
}

main "$@"
