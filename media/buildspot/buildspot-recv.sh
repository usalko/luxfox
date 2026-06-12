#!/bin/bash

##############################################################################
# buildspot-recv.sh — Device-side helper for receiving OEM files via UART
#
# Deployment path: /oem/usr/bin/buildspot-recv.sh
# Service script:   /etc/init.d/S99buildspot-recv
#
# This script:
#   1. Waits to receive files from PC via rz (lrzsz)
#   2. Stores files in /oem directory
#   3. Updates index
##############################################################################

set -e

OEM_DIR="/oem"
UART_DEVICE="/dev/ttyUSB0"

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
    
    log_info ""
    log_info "═════════════════════════════════════════════════════"
    log_info "Waiting to receive files from PC..."
    log_info "═════════════════════════════════════════════════════"
    log_info ""
    log_info "On PC, run:"
    log_info "  ./buildspot.sh --device /dev/ttyUSB0"
    log_info ""
    log_info "Press Ctrl+C to stop waiting"
    log_info ""
    
    # Start receiving in loop
    while true; do
        log_info "Ready to receive files via rz..."
        
        # Use timeout to avoid hanging indefinitely
        # rz will wait for incoming files
        timeout 600 rz -b -E < "$UART_DEVICE" > "$UART_DEVICE" 2>&1 || {
            local exit_code=$?
            if [ $exit_code -eq 124 ]; then
                # Timeout - just continue
                log_warn "Timeout waiting for files (10 min), waiting again..."
                continue
            elif [ $exit_code -ne 0 ]; then
                log_warn "rz error (exit code: $exit_code)"
            fi
        }
        
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
