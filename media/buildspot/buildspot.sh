#!/bin/bash

##############################################################################
# buildspot.sh — Fast OEM delta-sync via UART0 (lrzsz) with hash index
#
# Usage:
#   ./buildspot.sh [--device /dev/ttyUSB0] [--speed 115200] [--timeout 30]
#
# Examples:
#   ./buildspot.sh
#   ./buildspot.sh --device /dev/ttyUSB0 --speed 115200
#
# This script:
#   1. Reads hash index from LuckFox device (/oem/.buildspot.index.gz)
#   2. Computes SHA256 for all files in ./output/out/oem
#   3. Transfers only changed files via UART (sz/rz from lrzsz)
#   4. Updates index on device
#
# Requirements on both sides:
#   - lrzsz installed (opkg install lrzsz or apt install lrzsz)
#   - UART0 available (/dev/ttyUSB0 by default)
#   - On device: /oem directory exists
##############################################################################

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(cd "$SCRIPT_DIR" && pwd)"
OEM_SOURCE="$PROJECT_ROOT/output/out/oem"

# Configuration (can be overridden by flags)
UART_DEVICE="${UART_DEVICE:-/dev/ttyUSB0}"
UART_SPEED="${UART_SPEED:-115200}"
UART_TIMEOUT="${UART_TIMEOUT:-30}"
DEVICE_OEM_DIR="/oem"
DEVICE_INDEX_FILE=".buildspot.index.gz"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
MAGENTA='\033[0;35m'
CYAN='\033[0;36m'
NC='\033[0m'

# Temp files
TEMP_DIR=$(mktemp -d)
LOCAL_INDEX="$TEMP_DIR/index_local.txt"
DEVICE_INDEX="$TEMP_DIR/index_device.txt"
LOCAL_INDEX_GZ="$TEMP_DIR/index_local.gz"
FILES_TO_SYNC="$TEMP_DIR/sync_list.txt"

trap 'rm -rf "$TEMP_DIR"' EXIT

##############################################################################
# LOGGING
##############################################################################
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

log_stat() {
    echo -e "${CYAN}$1${NC}"
}

##############################################################################
# USAGE
##############################################################################
print_usage() {
    cat <<EOF
${MAGENTA}buildspot.sh${NC} — Sync OEM files to LuckFox via UART + hash index

${CYAN}Usage:${NC}
  $0 [options]

${CYAN}Options:${NC}
  --device DEVICE     UART device (default: /dev/ttyUSB0)
  --speed SPEED       UART speed in baud (default: 115200)
  --timeout SEC       Timeout per file in seconds (default: 30)
  --help              Show this help and exit

${CYAN}Examples:${NC}
  $0
  $0 --device /dev/ttyUSB1 --speed 115200
  $0 --device /dev/ttyACM0 --timeout 60

${CYAN}Defaults:${NC}
  Device:  $UART_DEVICE
  Speed:   $UART_SPEED baud
  Timeout: $UART_TIMEOUT seconds per file

${CYAN}Requirements:${NC}
  - lrzsz installed on both PC and device
    PC:     sudo apt install lrzsz
    Device: opkg install lrzsz
  - UART0 connected and permissions set (user in dialout group)
  - Device must have /oem directory

${MAGENTA}═══════════════════════════════════════════════════${NC}
EOF
}

##############################################################################
# VALIDATE ENVIRONMENT
##############################################################################
validate_environment() {
    log_info "Validating environment..."
    
    # Check lrzsz
    if ! command -v sz &>/dev/null || ! command -v rz &>/dev/null; then
        log_error "lrzsz not installed. Run: sudo apt install lrzsz"
        exit 1
    fi
    log_info "✓ lrzsz found"
    
    # Check OEM source
    if [ ! -d "$OEM_SOURCE" ]; then
        log_error "OEM source not found: $OEM_SOURCE"
        exit 1
    fi
    log_info "✓ OEM source found: $OEM_SOURCE"
    
    # Check UART device
    if [ ! -e "$UART_DEVICE" ]; then
        log_error "UART device not found: $UART_DEVICE"
        log_error "Available serial ports:"
        ls -la /dev/tty* 2>/dev/null | grep -E 'USB|ACM' || echo "  (none found)"
        exit 1
    fi
    log_info "✓ UART device available: $UART_DEVICE"
    
    # Check permissions
    if ! [ -r "$UART_DEVICE" ] || ! [ -w "$UART_DEVICE" ]; then
        log_error "No read/write permissions on $UART_DEVICE"
        log_error "Try: sudo usermod -a -G dialout \$USER"
        exit 1
    fi
    log_info "✓ UART permissions OK"
    
    # Check SHA256
    if ! command -v sha256sum &>/dev/null; then
        log_error "sha256sum not found"
        exit 1
    fi
    log_info "✓ sha256sum found"
}

##############################################################################
# CONFIGURE UART
##############################################################################
configure_uart() {
    log_info "Configuring UART: $UART_DEVICE @ $UART_SPEED baud"
    
    if ! command -v stty &>/dev/null; then
        log_warn "stty not available, skipping UART config"
        return 0
    fi
    
    if stty -F "$UART_DEVICE" "$UART_SPEED" cs8 -cstopb -parenb 2>/dev/null; then
        log_info "✓ UART configured"
    else
        log_warn "Could not configure UART (may still work)"
    fi
}

##############################################################################
# COMPUTE LOCAL FILE INDEX
##############################################################################
compute_local_index() {
    log_info "Computing local file index..."
    
    : > "$LOCAL_INDEX"
    local count=0
    
    # Find all files in OEM source, relative to OEM_SOURCE
    while IFS= read -r -d '' file; do
        local relpath="${file#$OEM_SOURCE/}"
        local hash=$(sha256sum "$file" | awk '{print $1}')
        local mtime=$(stat -c %Y "$file" 2>/dev/null || stat -f %m "$file" 2>/dev/null || echo 0)
        local size=$(stat -c %s "$file" 2>/dev/null || stat -f %z "$file" 2>/dev/null || echo 0)
        
        echo "$relpath;$hash;$mtime;$size" >> "$LOCAL_INDEX"
        ((count++))
    done < <(find "$OEM_SOURCE" -type f -print0)
    
    if [ $count -eq 0 ]; then
        log_warn "No files found in $OEM_SOURCE"
        return 1
    fi
    
    log_info "✓ Computed hashes for $count files"
    return 0
}

##############################################################################
# RECEIVE INDEX FROM DEVICE VIA UART (rz)
##############################################################################
receive_device_index() {
    log_info "Waiting to receive index from device..."
    log_info "  → On device, run: cd $DEVICE_OEM_DIR && rz -b < /dev/ttyUSB0 > /dev/ttyUSB0"
    log_info ""
    
    # Set up terminal for binary transfer
    local old_settings=$(stty -F "$UART_DEVICE" -g 2>/dev/null || true)
    
    # Receive file with timeout
    if timeout "$UART_TIMEOUT" rz -b -E "$DEVICE_INDEX_FILE" \
        < <(cat "$UART_DEVICE") \
        > >(cat > "$UART_DEVICE") 2>&1; then
        
        if [ -f "$DEVICE_INDEX_FILE" ]; then
            # Decompress if gzipped
            if file "$DEVICE_INDEX_FILE" | grep -q gzip; then
                gunzip -c "$DEVICE_INDEX_FILE" > "$DEVICE_INDEX" || {
                    log_warn "Could not decompress device index"
                    : > "$DEVICE_INDEX"
                }
            else
                cp "$DEVICE_INDEX_FILE" "$DEVICE_INDEX"
            fi
            
            log_info "✓ Received device index"
            log_debug "Device index entries: $(wc -l < "$DEVICE_INDEX")"
            return 0
        fi
    fi
    
    log_warn "No index received from device (first sync?)"
    : > "$DEVICE_INDEX"
    return 0
}

##############################################################################
# COMPUTE DELTA (files to sync)
##############################################################################
compute_delta() {
    log_info "Computing delta..."
    
    : > "$FILES_TO_SYNC"
    local new_count=0
    local changed_count=0
    local deleted_count=0
    
    # Create map of device files
    declare -A device_map
    if [ -f "$DEVICE_INDEX" ]; then
        while IFS=';' read -r relpath hash mtime size; do
            [ -z "$relpath" ] && continue
            device_map["$relpath"]="$hash"
        done < "$DEVICE_INDEX"
    fi
    
    # Check local files
    declare -A local_files
    while IFS=';' read -r relpath hash mtime size; do
        [ -z "$relpath" ] && continue
        local_files["$relpath"]="$hash"
        
        local device_hash="${device_map[$relpath]:-}"
        
        if [ -z "$device_hash" ]; then
            # New file
            echo "$relpath" >> "$FILES_TO_SYNC"
            ((new_count++))
        elif [ "$hash" != "$device_hash" ]; then
            # Changed file
            echo "$relpath" >> "$FILES_TO_SYNC"
            ((changed_count++))
        fi
    done < "$LOCAL_INDEX"
    
    # Check for deleted files
    for relpath in "${!device_map[@]}"; do
        if [ -z "${local_files[$relpath]:-}" ]; then
            # File was deleted locally (note but don't remove on device)
            ((deleted_count++))
        fi
    done
    
    local total=$(wc -l < "$FILES_TO_SYNC" | tr -d ' ')
    
    log_stat "╔════════════════════════════════════╗"
    log_stat "║         Sync Statistics            ║"
    log_stat "╠════════════════════════════════════╣"
    log_stat "║  New files:       $new_count"
    log_stat "║  Changed files:   $changed_count"
    log_stat "║  Deleted on PC:   $deleted_count (not removed on device)"
    log_stat "║  ─────────────────────────────────────"
    log_stat "║  Total to sync:   $total"
    log_stat "╚════════════════════════════════════╝"
    
    if [ "$total" -eq 0 ]; then
        log_info "✓ Device index is up to date, nothing to sync"
        return 1
    fi
    
    return 0
}

##############################################################################
# SEND FILES VIA UART (sz)
##############################################################################
send_files() {
    log_info "Sending files via UART..."
    log_info "  Device: $UART_DEVICE"
    log_info "  Speed:  $UART_SPEED baud"
    log_info "  Max ~10 KB/s; 1 MB ≈ 100 seconds"
    log_info ""
    log_info "On device, run: cd $DEVICE_OEM_DIR && rz -b < /dev/ttyUSB0 > /dev/ttyUSB0"
    log_info ""
    
    local count=0
    local total=$(wc -l < "$FILES_TO_SYNC" | tr -d ' ')
    local failed=0
    
    while IFS= read -r relpath; do
        [ -z "$relpath" ] && continue
        
        local src="$OEM_SOURCE/$relpath"
        if [ ! -f "$src" ]; then
            log_warn "[$((++count))/$total] File not found: $relpath"
            ((failed++))
            continue
        fi
        
        local size=$(stat -c %s "$src" 2>/dev/null || stat -f %z "$src" 2>/dev/null)
        local est_time=$((size / 1024 + 5))  # rough estimate in seconds
        
        log_debug "[$((++count))/$total] Sending: $relpath ($size bytes, ~${est_time}s)"
        
        # Create directory structure on device if needed
        local dirpath=$(dirname "$relpath")
        if [ "$dirpath" != "." ]; then
            ssh root@device "mkdir -p '$DEVICE_OEM_DIR/$dirpath'" 2>/dev/null || true
        fi
        
        # Send file with timeout
        if timeout "$((UART_TIMEOUT + est_time))" sz -b "$src" \
            > "$UART_DEVICE" \
            < "$UART_DEVICE" 2>&1; then
            log_info "✓ [$count/$total] $relpath"
        else
            log_error "✗ [$count/$total] Failed: $relpath"
            ((failed++))
        fi
    done < "$FILES_TO_SYNC"
    
    if [ $failed -gt 0 ]; then
        log_warn "Failed to send $failed files"
        return 1
    fi
    
    log_info "✓ All files sent successfully"
    return 0
}

##############################################################################
# UPDATE DEVICE INDEX
##############################################################################
update_device_index() {
    log_info "Updating index on device..."
    
    # Compress index
    gzip -c "$LOCAL_INDEX" > "$LOCAL_INDEX_GZ"
    
    # Send index file to device
    log_info "Sending index file..."
    
    if timeout "$UART_TIMEOUT" sz -b "$LOCAL_INDEX_GZ" \
        > "$UART_DEVICE" \
        < "$UART_DEVICE" 2>&1; then
        log_info "✓ Index file sent"
    else
        log_error "Failed to send index file"
        return 1
    fi
    
    log_info "✓ Device index updated"
    return 0
}

##############################################################################
# PRINT SUMMARY
##############################################################################
print_summary() {
    cat <<EOF

${MAGENTA}════════════════════════════════════════════════════${NC}
${GREEN}✓ Synchronization Complete!${NC}
${MAGENTA}════════════════════════════════════════════════════${NC}

${CYAN}Summary:${NC}
  Source:     $OEM_SOURCE
  Device:     root@device:$DEVICE_OEM_DIR
  Transport:  UART ($UART_DEVICE @ $UART_SPEED baud)
  Method:     lrzsz (ZMODEM)

${CYAN}Next Steps:${NC}
  1. SSH to device and verify files:
     $ ssh root@<device-ip>
     $ ls -la $DEVICE_OEM_DIR/

  2. Set file permissions if needed:
     $ chmod -R 755 $DEVICE_OEM_DIR/

  3. Run next sync:
     $ ./buildspot.sh

${MAGENTA}════════════════════════════════════════════════════${NC}

EOF
}

##############################################################################
# PARSE ARGUMENTS
##############################################################################
parse_arguments() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --device)
                UART_DEVICE="$2"
                shift 2
                ;;
            --speed)
                UART_SPEED="$2"
                shift 2
                ;;
            --timeout)
                UART_TIMEOUT="$2"
                shift 2
                ;;
            --help)
                print_usage
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                print_usage
                exit 1
                ;;
        esac
    done
}

##############################################################################
# MAIN
##############################################################################
main() {
    echo ""
    log_stat "╔════════════════════════════════════════════════════╗"
    log_stat "║  buildspot.sh — OEM Sync via UART + Hash Index    ║"
    log_stat "╚════════════════════════════════════════════════════╝"
    echo ""
    
    parse_arguments "$@"
    validate_environment
    configure_uart
    
    # Step 1: Compute local index
    if ! compute_local_index; then
        log_error "Failed to compute local index"
        exit 1
    fi
    
    # Step 2: Receive device index (can fail gracefully on first sync)
    receive_device_index
    
    # Step 3: Compute delta
    if ! compute_delta; then
        log_info "No changes to sync"
        print_summary
        exit 0
    fi
    
    # Step 4: Send files
    if ! send_files; then
        log_error "Some files failed to send"
        exit 1
    fi
    
    # Step 5: Update device index
    if ! update_device_index; then
        log_error "Failed to update index"
        exit 1
    fi
    
    print_summary
}

##############################################################################
# ENTRY POINT
##############################################################################
main "$@"
