#!/bin/bash

##############################################################################
# deploy-to-luckfox.sh — Deploy ARM artifacts to LuckFox device
#
# Usage:
#   ./deploy-to-luckfox.sh <luckfox-ip-or-host> [destination-dir]
#
# Examples:
#   ./deploy-to-luckfox.sh 192.168.1.100
#   ./deploy-to-luckfox.sh root@luckfox.local
#   ./deploy-to-luckfox.sh 192.168.1.100 /opt/unow
#
# This script:
#   1. Builds ARM artifacts if not already built
#   2. Transfers binaries and libraries to LuckFox
#   3. Sets up permissions
#   4. Prints connection instructions
##############################################################################

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
UNOW_ROOT="$SCRIPT_DIR"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Configuration
LUCKFOX_HOST="${1:-}"
DEST_DIR="${2:-/usr/bin}"
SSH_USER="root"
SSH_PORT="22"

# Logging
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

print_usage() {
    cat <<EOF
UNOW Deploy to LuckFox

Usage:
  $0 <luckfox-host> [destination-dir]

Arguments:
  luckfox-host      IP address or hostname of LuckFox device
                    (can include user: root@192.168.1.100)
  destination-dir   Where to install binary (default: /usr/bin)

Examples:
  $0 192.168.1.100
  $0 root@luckfox.local
  $0 192.168.1.100 /opt/unow

Environment:
  SSH_PORT          SSH port (default: 22)

EOF
}

##############################################################################
# VALIDATE INPUTS
##############################################################################
validate_inputs() {
    if [ -z "$LUCKFOX_HOST" ]; then
        log_error "LuckFox host not specified"
        print_usage
        exit 1
    fi
    
    # Parse user@host if present
    if [[ "$LUCKFOX_HOST" == *"@"* ]]; then
        SSH_USER="${LUCKFOX_HOST%%@*}"
        LUCKFOX_HOST="${LUCKFOX_HOST##*@}"
    fi
    
    log_info "Target: ssh://$SSH_USER@$LUCKFOX_HOST:$SSH_PORT"
    log_info "Destination: $DEST_DIR"
}

##############################################################################
# CHECK CONNECTIVITY
##############################################################################
check_connectivity() {
    log_info "Checking connectivity to LuckFox..."
    
    if ! ssh -p "$SSH_PORT" "$SSH_USER@$LUCKFOX_HOST" "echo 'SSH OK'" &>/dev/null; then
        log_error "Cannot connect to $SSH_USER@$LUCKFOX_HOST:$SSH_PORT"
        log_error "Make sure:"
        log_error "  1. LuckFox is powered on and connected to network"
        log_error "  2. SSH is enabled on LuckFox"
        log_error "  3. You have correct IP/hostname"
        log_error "  4. SSH key is configured (or use sshpass)"
        exit 1
    fi
    
    log_info "✓ SSH connection successful"
}

##############################################################################
# BUILD ARM ARTIFACTS IF NEEDED
##############################################################################
build_arm_if_needed() {
    if [ ! -f "$UNOW_ROOT/out/arm/bin/unow_diag" ]; then
        log_warn "ARM artifacts not found, building..."
        if [ ! -x "$UNOW_ROOT/build.sh" ]; then
            log_error "build.sh not found or not executable"
            exit 1
        fi
        bash "$UNOW_ROOT/build.sh" -t arm || {
            log_error "ARM build failed"
            exit 1
        }
    else
        log_info "✓ ARM artifacts already built"
    fi
}

##############################################################################
# CREATE DESTINATION DIR ON LUCKFOX
##############################################################################
create_dest_dir() {
    log_info "Creating destination directory on LuckFox..."
    
    ssh -p "$SSH_PORT" "$SSH_USER@$LUCKFOX_HOST" \
        "mkdir -p '$DEST_DIR' && chmod 755 '$DEST_DIR'" || {
        log_error "Failed to create directory on LuckFox"
        exit 1
    }
    
    log_info "✓ Destination directory ready"
}

##############################################################################
# TRANSFER BINARIES
##############################################################################
transfer_binary() {
    local src="$1"
    local name="$(basename "$src")"
    
    if [ ! -f "$src" ]; then
        log_warn "File not found: $src"
        return 1
    fi
    
    log_info "Transferring $name..."
    scp -P "$SSH_PORT" "$src" "$SSH_USER@$LUCKFOX_HOST:$DEST_DIR/$name" || {
        log_error "Failed to transfer $name"
        return 1
    }
    
    # Make executable
    ssh -p "$SSH_PORT" "$SSH_USER@$LUCKFOX_HOST" \
        "chmod +x '$DEST_DIR/$name'" || {
        log_warn "Could not set executable bit on $name"
    }
    
    log_info "✓ $name transferred"
}

##############################################################################
# TRANSFER LIBRARIES
##############################################################################
transfer_libraries() {
    local lib_dest="/usr/lib"
    
    log_info "Creating library directory on LuckFox..."
    ssh -p "$SSH_PORT" "$SSH_USER@$LUCKFOX_HOST" \
        "mkdir -p '$lib_dest'" || {
        log_warn "Could not create $lib_dest on LuckFox"
    }
    
    for lib in "$UNOW_ROOT/out/arm/lib"/*; do
        if [ -f "$lib" ]; then
            local libname="$(basename "$lib")"
            log_info "Transferring $libname..."
            scp -P "$SSH_PORT" "$lib" "$SSH_USER@$LUCKFOX_HOST:$lib_dest/$libname" || {
                log_warn "Failed to transfer $libname (optional)"
            }
        fi
    done
    
    log_info "✓ Libraries transferred"
}

##############################################################################
# TRANSFER SCRIPTS
##############################################################################
transfer_scripts() {
    local scripts_src="$UNOW_ROOT/scripts"
    local scripts_dest="$DEST_DIR/scripts"
    
    if [ ! -d "$scripts_src" ]; then
        log_warn "Scripts directory not found: $scripts_src"
        return 0
    fi
    
    log_info "Creating scripts directory on LuckFox..."
    ssh -p "$SSH_PORT" "$SSH_USER@$LUCKFOX_HOST" \
        "mkdir -p '$scripts_dest'" || {
        log_warn "Could not create $scripts_dest on LuckFox"
    }
    
    for script in "$scripts_src"/*.sh; do
        if [ -f "$script" ]; then
            local sname="$(basename "$script")"
            log_info "Transferring script $sname..."
            scp -P "$SSH_PORT" "$script" "$SSH_USER@$LUCKFOX_HOST:$scripts_dest/$sname" || {
                log_warn "Failed to transfer $sname"
            }
            ssh -p "$SSH_PORT" "$SSH_USER@$LUCKFOX_HOST" \
                "chmod +x '$scripts_dest/$sname'" 2>/dev/null || true
        fi
    done
    
    log_info "✓ Scripts transferred"
}

##############################################################################
# TRANSFER INIT.D SERVICE
##############################################################################
transfer_init_service() {
    local init_dest="/etc/init.d"
    local default_dest="/etc/default"

    log_info "Installing init.d service on LuckFox..."

    ssh -p "$SSH_PORT" "$SSH_USER@$LUCKFOX_HOST" \
        "mkdir -p '$init_dest' '$default_dest'" || {
        log_warn "Could not create init.d/default directories on LuckFox"
        return 0
    }

    ssh -p "$SSH_PORT" "$SSH_USER@$LUCKFOX_HOST" "cat > '$default_dest/unow-signal-diag' <<EOF
UNOW_SIGNAL_DIAG_SCRIPT=$DEST_DIR/scripts/unow-signal-diag.sh
UNOW_SIGNAL_DIAG_IFACE=wlan0
UNOW_SIGNAL_DIAG_BSSID=55:4E:4F:57:00:01
UNOW_SIGNAL_DIAG_LOG_DIR=/var/log/unow
UNOW_SIGNAL_DIAG_PIDFILE=/var/run/unow-signal-diag.pid
EOF
chmod 644 '$default_dest/unow-signal-diag'" || {
        log_warn "Could not install /etc/default/unow-signal-diag"
    }

    scp -P "$SSH_PORT" "$UNOW_ROOT/scripts/S99unow-signal-diag" \
        "$SSH_USER@$LUCKFOX_HOST:$init_dest/S99unow-signal-diag" || {
        log_warn "Failed to transfer init.d service"
        return 0
    }

    ssh -p "$SSH_PORT" "$SSH_USER@$LUCKFOX_HOST" \
        "chmod 755 '$init_dest/S99unow-signal-diag'" || true

    log_info "✓ Init.d service installed"
}

##############################################################################
# VERIFY DEPLOYMENT
##############################################################################
verify_deployment() {
    log_info "Verifying deployment..."
    
    if ssh -p "$SSH_PORT" "$SSH_USER@$LUCKFOX_HOST" \
        "test -x '$DEST_DIR/unow_diag' && echo 'OK'" &>/dev/null; then
        log_info "✓ Binary is executable"
    else
        log_warn "Cannot verify binary is executable"
    fi
    
    if ssh -p "$SSH_PORT" "$SSH_USER@$LUCKFOX_HOST" \
        "'$DEST_DIR/unow_diag' --version 2>/dev/null || '$DEST_DIR/unow_diag' --help 2>/dev/null | head -3" &>/dev/null; then
        log_info "✓ Binary responds to commands"
    fi
}

##############################################################################
# PRINT NEXT STEPS
##############################################################################
print_next_steps() {
    cat <<EOF

${BLUE}════════════════════════════════════════════════════${NC}
${BLUE}✓ Deployment Complete!${NC}
${BLUE}════════════════════════════════════════════════════${NC}

Binary location:   $DEST_DIR/unow_diag
Libraries:         /usr/lib/libunow.so, libunow.a (if transferred)
Scripts:           $DEST_DIR/scripts/

${GREEN}Next Steps:${NC}

1. SSH to LuckFox:
   $ ssh $SSH_USER@$LUCKFOX_HOST

2. Identify your Wi-Fi adapter:
   $ iw dev

3. Set up monitor mode on channel 6:
   $ bash $DEST_DIR/scripts/unow-mon.sh <wifi-adapter> mon0 6

4. Start the diagnostic daemon on LuckFox:
   $ /etc/init.d/S99unow-signal-diag start

5. Watch logs at:
   $ tail -f /var/log/unow/wifi_signal_diag.log

6. Run G0 smoke test (receiver):
   $ $DEST_DIR/unow_diag --iface mon0 --node-id 1 --listen 10

7. On your Desktop (another terminal):
   $ bash media/unow/RUNME_G0_TEST.sh desktop tx

See media/unow/TESTING.md for full test guide.

${BLUE}════════════════════════════════════════════════════${NC}

EOF
}

##############################################################################
# MAIN
##############################################################################
main() {
    log_info "=========================================="
    log_info "UNOW Deploy to LuckFox"
    log_info "=========================================="
    
    validate_inputs
    check_connectivity
    build_arm_if_needed
    create_dest_dir
    
    transfer_binary "$UNOW_ROOT/out/arm/bin/unow_diag"
    transfer_libraries
    transfer_scripts
    transfer_init_service

    verify_deployment
    print_next_steps
    
    log_info "Done!"
}

# Handle arguments
if [[ "$1" == "-h" ]] || [[ "$1" == "--help" ]]; then
    print_usage
    exit 0
fi

main "$@"
