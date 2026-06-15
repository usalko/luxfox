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

# AGGRESSIVE PRE-CLEANUP: Kill any existing monitor interface
if ip link show "${MON_IFACE}" >/dev/null 2>&1; then
    log_warn "Removing stale '${MON_IFACE}' interface..."
    iw dev "${MON_IFACE}" del 2>/dev/null || {
        # If iw fails, try ip link
        ip link set "${MON_IFACE}" down 2>/dev/null || true
        sleep 0.1
    }
    sleep 0.2
fi

# Verify BASE_IFACE exists and get its MAC
if ! ip link show "${BASE_IFACE}" >/dev/null 2>&1; then
    log_error "Base interface '${BASE_IFACE}' not found!"
    iw dev | grep "Interface" || echo "  (no interfaces found)"
    exit 1
fi

# Store base interface MAC EARLY (before any modifications)
BASE_MAC=$(ip link show "${BASE_IFACE}" 2>/dev/null | grep link | awk '{print $2}')
if [ -z "${BASE_MAC}" ]; then
    log_error "Could not retrieve MAC address from ${BASE_IFACE}"
    exit 1
fi
log_info "Base interface MAC: ${BASE_MAC}"

DRIVER_MODULE=""
if [ -e "/sys/class/net/${BASE_IFACE}/device/driver/module" ]; then
    DRIVER_MODULE=$(basename "$(readlink -f "/sys/class/net/${BASE_IFACE}/device/driver/module")")
fi
log_info "Driver module for '${BASE_IFACE}': ${DRIVER_MODULE:-unknown}"

FORCE_BASE_MONITOR=0
if [ "${DRIVER_MODULE}" = "8192eu" ] || [ "${DRIVER_MODULE}" = "rtl8192eu" ]; then
    FORCE_BASE_MONITOR=1
    log_warn "Driver '${DRIVER_MODULE}' reports RX on the base netdev, not on a virtual monitor interface."
    log_warn "Skipping virtual monitor interface creation and switching the base netdev into monitor mode."
fi

# Diagnostic: Show wlan0 current state
log_info "Current state of '${BASE_IFACE}':"
ip link show "${BASE_IFACE}" 2>/dev/null | head -2 || log_warn "  Could not query"
iw dev "${BASE_IFACE}" info 2>/dev/null | head -3 || log_warn "  Could not get iw info"

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
sleep 0.1

# 3b. Verify BASE_IFACE is in managed mode (not monitor)
log_info "Verifying '${BASE_IFACE}' is in managed mode..."
iw dev "${BASE_IFACE}" set type managed 2>/dev/null || log_warn "Could not force managed mode"
sleep 0.1

# 4. Attempt to add a new monitor interface (Method A) - PRIMARY method
if [ "${FORCE_BASE_MONITOR}" -eq 0 ]; then
    log_info "Attempting to add virtual interface '${MON_IFACE}' (Method A)..."
    if iw dev "${BASE_IFACE}" interface add "${MON_IFACE}" type monitor 2>/dev/null; then
        log_info "✓ Successfully created '${MON_IFACE}' as virtual interface."
        sleep 0.1
        
        # CRITICAL: Set MAC address BEFORE bringing interface up
        log_info "Setting MAC to ${BASE_MAC} on fresh '${MON_IFACE}'..."
        ip link set "${MON_IFACE}" address "${BASE_MAC}" 2>/dev/null || log_warn "Could not set MAC immediately after creation"
        PREUP_MAC=$(ip link show "${MON_IFACE}" 2>/dev/null | grep link | awk '{print $2}')
        log_info "  MAC before bringing up: ${PREUP_MAC}"
        
        # Bring up and set channel
        ip link set "${MON_IFACE}" up
        sleep 0.1
        
        log_info "Setting channel to ${CHAN}..."
        iw dev "${MON_IFACE}" set channel "${CHAN}"
        sleep 0.1
        
        # Check final state
        FINAL_MAC=$(ip link show "${MON_IFACE}" 2>/dev/null | grep link | awk '{print $2}')
        FINAL_TYPE=$(iw dev "${MON_IFACE}" info 2>/dev/null | grep type | awk '{print $NF}')
        log_info "Final: type=${FINAL_TYPE}, MAC=${FINAL_MAC}"
        
        if [ "${FINAL_TYPE}" = "monitor" ] && [ "${FINAL_MAC}" != "00:00:00:00:00:00" ]; then
            log_info "✅ Success! Monitor mode active on '${MON_IFACE}' with correct MAC."
            exit 0
        elif [ "${FINAL_TYPE}" = "monitor" ]; then
            log_warn "Monitor mode active but MAC is 00:00:00:00:00:00. Continuing anyway..."
            # Continue to Method B to try fixing MAC
        else
            log_warn "Interface created but not in monitor mode. Trying Method B..."
            iw dev "${MON_IFACE}" del 2>/dev/null || true
        fi
    fi
else
    log_info "Skipping Method A for driver '${DRIVER_MODULE}'."
fi

# 5. Method B - Rename approach
log_info "Method A inconclusive. Trying Method B (rename approach)..."

# Ensure BASE_IFACE and MON_IFACE are different
if [ "${BASE_IFACE}" = "${MON_IFACE}" ]; then
    log_error "Cannot rename interface to itself!"
    exit 1
fi

# Completely remove any existing MON_IFACE first
if ip link show "${MON_IFACE}" >/dev/null 2>&1; then
    log_warn "Removing existing '${MON_IFACE}'..."
    ip link set "${MON_IFACE}" down 2>/dev/null || true
    iw dev "${MON_IFACE}" del 2>/dev/null || true
    sleep 0.2
fi

# Rename BASE_IFACE to MON_IFACE
log_info "Renaming '${BASE_IFACE}' to '${MON_IFACE}'..."
ip link set "${BASE_IFACE}" name "${MON_IFACE}" || {
    log_error "Failed to rename '${BASE_IFACE}' to '${MON_IFACE}'"
    exit 1
}
sleep 0.1

# 6. Configure renamed interface for monitor mode
log_info "Setting '${MON_IFACE}' to monitor mode..."
iw dev "${MON_IFACE}" set type monitor || {
    log_error "Failed to set monitor type"
    exit 1
}
sleep 0.1

# Try to set MAC BEFORE bringing up
log_info "Setting MAC to ${BASE_MAC}..."
ip link set "${MON_IFACE}" address "${BASE_MAC}" 2>/dev/null || log_warn "Could not set MAC"

# Bring up
ip link set "${MON_IFACE}" up
sleep 0.1

# Set channel
log_info "Setting channel to ${CHAN}..."
iw dev "${MON_IFACE}" set channel "${CHAN}"
sleep 0.1

# Final check
FINAL_MAC=$(ip link show "${MON_IFACE}" 2>/dev/null | grep link | awk '{print $2}')
FINAL_TYPE=$(iw dev "${MON_IFACE}" info 2>/dev/null | grep type | awk '{print $NF}')
log_info "Final state: type=${FINAL_TYPE}, MAC=${FINAL_MAC}"

# 7. Final verification
if iw dev "${MON_IFACE}" info 2>/dev/null | grep -q "type monitor"; then
    if [ "${FINAL_MAC}" = "00:00:00:00:00:00" ]; then
        log_warn "⚠️  Monitor mode active but MAC is 00:00:00:00:00:00"
        log_warn "  This may be a rtl8192eu driver limitation."
        log_warn "  ULAMA will attempt software MAC filtering as workaround."
    fi
    log_info "✅ Success! Monitor mode is active on '${MON_IFACE}' (MAC: ${FINAL_MAC}, channel: ${CHAN})."
    exit 0
else
    log_error "Failed to enable monitor mode on '${MON_IFACE}' after all attempts."
    exit 1
fi