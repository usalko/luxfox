#!/bin/bash
# ==============================================================================
# unow-mon.sh — Puts a Wi-Fi interface into monitor mode on a specific channel
#
# Usage:
#   ./unow-mon.sh <base-iface> <monitor-iface> <channel>
#
# Example:
#   ./unow-mon.sh wlan1 mon0 6
#
# This script handles two main cases:
#   1. Drivers that support adding a virtual monitor interface (e.g., rtl8xxxu)
#   2. Drivers that require renaming the base interface (e.g., aic8800dc)
# ==============================================================================

set -euo pipefail

# --- Configuration ---
BASE_IFACE="${1:-wlan1}"
MON_IFACE="${2:-mon0}"
CHAN="${3:-6}"

# --- Colors for logging ---
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
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

# --- Main Logic ---
log_info "Setting up monitor mode: base=${BASE_IFACE}, monitor=${MON_IFACE}, channel=${CHAN}"

# 1. Kill interfering processes (like wpa_supplicant)
if command -v airmon-ng >/dev/null 2>&1; then
    log_info "Stopping network managers with airmon-ng..."
    airmon-ng check kill || true
else
    log_warn "airmon-ng not found, network managers might interfere."
fi

# 2. Ensure base interface exists before we start
if ! ip link show "${BASE_IFACE}" >/dev/null 2>&1; then
    log_error "Base interface '${BASE_IFACE}' not found. Available interfaces:"
    iw dev | grep "Interface" || echo " (none found)"
    exit 1
fi

# 3. Bring the base interface down
log_info "Taking interface '${BASE_IFACE}' down..."
ip link set "${BASE_IFACE}" down || log_warn "Could not take ${BASE_IFACE} down (maybe already down)."

# 4. Attempt to add a new monitor interface (Method A)
log_info "Attempting to add virtual interface '${MON_IFACE}'..."
if iw dev "${BASE_IFACE}" interface add "${MON_IFACE}" type monitor 2>/dev/null; then
    log_info "Successfully added virtual interface '${MON_IFACE}'."
    
    # Configure the new monitor interface
    ip link set "${MON_IFACE}" up
    log_info "Setting channel to ${CHAN} on '${MON_IFACE}'..."
    iw dev "${MON_IFACE}" set channel "${CHAN}"
    
    # Verify
    if iw dev "${MON_IFACE}" info | grep -q "type monitor"; then
        log_info "✅ Success! Monitor mode is active on '${MON_IFACE}'."
        exit 0
    else
        log_warn "Interface '${MON_IFACE}' was created but is not in monitor mode. Cleaning up."
        iw dev "${MON_IFACE}" del || true
    fi
fi

# 5. If Method A failed, try renaming the interface (Method B)
log_warn "Could not add virtual interface. Trying to rename '${BASE_IFACE}' to '${MON_IFACE}' instead."

# Check if MON_IFACE already exists from a failed previous run
if ip link show "${MON_IFACE}" >/dev/null 2>&1 && [ "${BASE_IFACE}" != "${MON_IFACE}" ]; then
    log_warn "Interface '${MON_IFACE}' already exists. Trying to use it directly."
    # Fall through to configure it
elif [ "${BASE_IFACE}" != "${MON_IFACE}" ]; then
    log_info "Renaming '${BASE_IFACE}' to '${MON_IFACE}'..."
    ip link set "${BASE_IFACE}" name "${MON_IFACE}" || {
        log_error "Failed to rename '${BASE_IFACE}' to '${MON_IFACE}'. Aborting."
        exit 1
    }
fi

# 6. Configure the (now renamed) interface for monitor mode
log_info "Setting '${MON_IFACE}' to monitor mode..."
iw dev "${MON_IFACE}" set type monitor || {
    log_error "Failed to set '${MON_IFACE}' to monitor type. Your driver may not support it."
    exit 1
}

ip link set "${MON_IFACE}" up
log_info "Setting channel to ${CHAN} on '${MON_IFACE}'..."
iw dev "${MON_IFACE}" set channel "${CHAN}"

# 7. Final verification
if iw dev "${MON_IFACE}" info | grep -q "type monitor"; then
    log_info "✅ Success! Monitor mode is active on '${MON_IFACE}'."
    exit 0
else
    log_error "Failed to enable monitor mode on '${MON_IFACE}' after multiple attempts."
    exit 1
fi