#!/bin/bash
#
# buildspot-sync-ssh.sh — Fast file sync via SSH + rsync
#
# Synchronizes files from local output/out/oem to remote /oem using SSH.
# Supports delta sync and progress reporting.
#
# Usage:
#   ./buildspot-sync-ssh.sh [options]
#
# Options:
#   --host HOST         Device hostname/IP (default: 192.168.1.100)
#   --user USER         SSH user (default: root)
#   --port PORT         SSH port (default: 22)
#   --key FILE          SSH private key (default: ~/.ssh/id_rsa)
#   --source DIR        Local source directory (default: output/out/oem)
#   --target DIR        Remote target directory (default: /oem)
#   --timeout SEC       Connection timeout (default: 30)
#   --help              Show this help
#

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../" && pwd)"

HOST="${HOST:-192.168.1.100}"
USER="${USER:-root}"
PORT="${PORT:-22}"
KEY="${KEY:-$HOME/.ssh/id_rsa}"
SOURCE="${SOURCE:-$PROJECT_ROOT/output/out/oem}"
TARGET="${TARGET:-/oem}"
TIMEOUT="${TIMEOUT:-30}"

log_info() {
    echo "[buildspot-sync] $1"
}

log_error() {
    echo "[buildspot-sync] ERROR: $1" >&2
}

show_help() {
    grep "^#" "$0" | tail -n +3 | sed 's/^# *//'
}

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --host)
                HOST="$2"
                shift 2
                ;;
            --user)
                USER="$2"
                shift 2
                ;;
            --port)
                PORT="$2"
                shift 2
                ;;
            --key)
                KEY="$2"
                shift 2
                ;;
            --source)
                SOURCE="$2"
                shift 2
                ;;
            --target)
                TARGET="$2"
                shift 2
                ;;
            --timeout)
                TIMEOUT="$2"
                shift 2
                ;;
            --help)
                show_help
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                show_help
                exit 1
                ;;
        esac
    done
}

validate_source() {
    if [ ! -d "$SOURCE" ]; then
        log_error "Source directory not found: $SOURCE"
        return 1
    fi
    
    local file_count
    file_count=$(find "$SOURCE" -type f 2>/dev/null | wc -l)
    if [ "$file_count" -eq 0 ]; then
        log_error "No files found in source: $SOURCE"
        return 1
    fi
    
    log_info "Source: $SOURCE ($file_count files)"
    return 0
}

check_ssh_key() {
    if [ ! -f "$KEY" ]; then
        log_error "SSH key not found: $KEY"
        log_error "Generate one with: ssh-keygen -t rsa -b 2048 -f $KEY -N ''"
        return 1
    fi
    
    if [ ! -r "$KEY" ]; then
        log_error "SSH key not readable: $KEY"
        return 1
    fi
    
    chmod 600 "$KEY"
    return 0
}

check_connectivity() {
    log_info "Checking SSH connectivity to $USER@$HOST:$PORT..."
    
    if ! timeout "$TIMEOUT" ssh \
        -o StrictHostKeyChecking=accept-new \
        -o ConnectTimeout="$TIMEOUT" \
        -o BatchMode=yes \
        -i "$KEY" \
        -p "$PORT" \
        "$USER@$HOST" \
        "echo 'SSH connected'; test -d $TARGET && echo 'Target directory exists'" >/dev/null 2>&1; then
        log_error "Failed to connect to $USER@$HOST:$PORT"
        log_error "Make sure:"
        log_error "  1. Device has SSH server running (Dropbear or OpenSSH)"
        log_error "  2. Your public key is in device's ~/.ssh/authorized_keys"
        log_error "  3. Device has Ethernet connected and IP address assigned"
        return 1
    fi
    
    log_info "SSH connection verified"
    return 0
}

sync_files() {
    log_info "Starting rsync (delta sync with progress)..."
    
    # Use rsync for efficient delta sync
    # -a: archive mode (preserves permissions, timestamps)
    # -v: verbose
    # --progress: show progress
    # --delete: delete files on target that are not on source (optional, use with care)
    # --compress: compress data in transit
    # -e ssh: use SSH transport
    
    rsync -av \
        --progress \
        --compress \
        --rsh="ssh -i $KEY -p $PORT -o StrictHostKeyChecking=accept-new -o ConnectTimeout=$TIMEOUT" \
        "$SOURCE/" \
        "$USER@$HOST:$TARGET/" || {
        log_error "rsync failed"
        return 1
    }
    
    log_info "Sync completed successfully"
    return 0
}

verify_sync() {
    log_info "Verifying sync on remote..."
    
    local local_count remote_count
    local_count=$(find "$SOURCE" -type f 2>/dev/null | wc -l)
    remote_count=$(ssh \
        -i "$KEY" \
        -p "$PORT" \
        -o StrictHostKeyChecking=accept-new \
        "$USER@$HOST" \
        "find $TARGET -type f 2>/dev/null | wc -l" 2>/dev/null || echo "0")
    
    log_info "Local files: $local_count"
    log_info "Remote files: $remote_count"
    
    if [ "$local_count" -gt 0 ] && [ "$remote_count" -ge "$local_count" ]; then
        log_info "✓ Verification successful"
        return 0
    else
        log_error "Verification failed: file count mismatch"
        return 1
    fi
}

main() {
    parse_args "$@"
    
    log_info "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    log_info "Buildspot SSH Sync Tool"
    log_info "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    log_info "Host: $USER@$HOST:$PORT"
    log_info "Source: $SOURCE"
    log_info "Target: $TARGET"
    
    validate_source || exit 1
    check_ssh_key || exit 1
    check_connectivity || exit 1
    sync_files || exit 1
    verify_sync || exit 1
    
    log_info "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    log_info "All operations completed successfully!"
    log_info "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
}

main "$@"
