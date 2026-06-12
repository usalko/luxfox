#!/bin/sh

##############################################################################
# buildspot-recv.sh — Device-side helper for receiving OEM files via UART
##############################################################################

# Redirect everything to the log file immediately
LOGFILE="/var/log/buildspot-recv.log"
exec >> "$LOGFILE" 2>&1

set -e

OEM_DIR="/oem"

# Smart UART detection
detect_smart_uart() {
    # 1. Check if we have USB-UART
    local dev=$(ls /dev/ttyUSB0 2>/dev/null || true)
    [ -n "$dev" ] && echo "$dev" && return

    # 2. Check for FIQ debugger (Common console on Luckfox/Rockchip)
    dev=$(ls /dev/ttyFIQ0 2>/dev/null || true)
    [ -n "$dev" ] && echo "$dev" && return
    
    # 3. Check for standard UARTs (S1 usually available on Luckfox Ultra)
    dev=$(ls /dev/ttyS1 2>/dev/null || true)
    [ -n "$dev" ] && echo "$dev" && return

    # 4. Global fallback
    echo "/dev/ttyS0"
}

UART_DEVICE=$(detect_smart_uart)

# Ensure we have a sane PATH
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin

# Helper to run with timeout if available, else run directly
run_with_timeout() {
    local t=$1
    shift
    if command -v timeout >/dev/null 2>&1; then
        timeout "$t" "$@"
    else
        # Fallback: run without timeout if utility is missing
        "$@"
    fi
}

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

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
    echo -e "${BLUE}[DEBUG]${NC} $1"
}

##############################################################################
# VALIDATE SETUP
##############################################################################
validate_setup() {
    log_info "Validating device setup..."
    
    # Check if lrzsz is installed
    if ! command -v rz &>/dev/null; then
        log_error "lrzsz not installed"
        log_info "Install: opkg install lrzsz (or apt install lrzsz)"
        exit 1
    fi
    log_info "✓ lrzsz found"
    
    # Check OEM directory
    if [ ! -d "$OEM_DIR" ]; then
        log_warn "OEM directory not found: $OEM_DIR"
        log_info "Creating: mkdir -p $OEM_DIR"
        mkdir -p "$OEM_DIR"
    fi
    log_info "✓ OEM directory ready: $OEM_DIR"
    
    # Check UART device
    if [ ! -e "$UART_DEVICE" ]; then
        log_warn "UART device not found: $UART_DEVICE"
        log_info "Available serial ports:"
        ls -la /dev/tty* 2>/dev/null | grep -E 'USB|ACM|S[0-9]' || echo "  (none found)"
        
        # If running in a terminal, ask. Otherwise use default.
        if [ -t 0 ]; then
            read -p "Enter UART device [/dev/ttyS0]: " input
            UART_DEVICE="${input:-/dev/ttyS0}"
        else
            UART_DEVICE="/dev/ttyS0"
            log_info "Non-interactive mode: using default $UART_DEVICE"
        fi
    fi
    
    if [ ! -e "$UART_DEVICE" ]; then
        log_error "UART device not accessible: $UART_DEVICE"
        exit 1
    fi
    log_info "✓ UART device ready: $UART_DEVICE"
}

##############################################################################
# MAIN LOOP
##############################################################################
receive_loop() {
    cd "$OEM_DIR"
    
    log_info "Buildspot receiver started at $(date)"
    log_info "Current OEM_DIR: $OEM_DIR"
    log_info "Using UART_DEVICE: $UART_DEVICE"
    
    # Start receiving in loop
    while true; do
        log_info "Ready to receive files via rz... (waiting for PC sz command)"
        
        # Use timeout to avoid hanging indefinitely
        # rz will wait for incoming files
        # STTY settings to ensure UART is in raw mode for binary transfer
        stty -F "$UART_DEVICE" raw speed 115200 -echo -echoe -echok 2>/dev/null || true
        
        # Capture stderr of rz to see serial protocol errors
        if run_with_timeout 600 rz -b -E < "$UART_DEVICE" > "$UART_DEVICE" 2>/tmp/rz_error.log; then
            log_info "rz command finished successfully or received files"
            [ -f /tmp/rz_error.log ] && log_debug "rz output: $(cat /tmp/rz_error.log)"
        else
            local exit_code=$?
            if [ $exit_code -eq 124 ]; then
                log_warn "Session timeout (600s), no files received. Restarting loop..."
                continue
            else
                log_error "rz error (exit code: $exit_code)"
                [ -f /tmp/rz_error.log ] && log_error "rz details: $(cat /tmp/rz_error.log)"
                # Brief sleep to avoid rapid restart on persistent serial errors
                sleep 2
            fi
        fi
        
        # Check if index file was received
        if [ -f ".buildspot.index.gz" ]; then
            log_info "✓ Received index file, decompressing..."
            gunzip -f ".buildspot.index.gz" && \
                mv ".buildspot.index" ".buildspot.index.txt" 2>/dev/null || true
            log_info "✓ Index updated"
        fi
        
        log_info "Waiting for next batch..."
    done
}

##############################################################################
# MAIN
##############################################################################
main() {
    echo ""
    log_info "════════════════════════════════════════════════════"
    log_info "buildspot-recv.sh — UART OEM file receiver"
    log_info "════════════════════════════════════════════════════"
    echo ""
    
    validate_setup
    
    log_info ""
    log_info "Configuration:"
    log_info "  OEM Directory: $OEM_DIR"
    log_info "  UART Device:   $UART_DEVICE"
    log_info ""
    
    receive_loop
}

main "$@"
