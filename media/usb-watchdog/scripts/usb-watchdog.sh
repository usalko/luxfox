#!/bin/sh
# usb-watchdog.sh — monitor Wi-Fi adapter, restart radio stack on USB disconnect
# Runs as a background daemon on LuckFox Pico Ultra
# Detects MW300UH (RTL8192EU) disappearance and restarts ulamad/vcpd

IFACE="${1:-wlan0}"
CHECK_INTERVAL=5
SETTLE_TIME=3
CHANNEL=6

log() {
    logger -t usb-watchdog "$1"
    echo "[usb-watchdog] $1"
}

log "started, monitoring $IFACE every ${CHECK_INTERVAL}s"

while true; do
    sleep "$CHECK_INTERVAL"

    if ip link show "$IFACE" >/dev/null 2>&1; then
        continue
    fi

    log "WARNING: $IFACE disappeared — USB hub reset detected"

    # Stop daemons gracefully
    for svc in S99ulama S98ulama-gw S97vcpd; do
        if [ -x "/etc/init.d/$svc" ]; then
            "/etc/init.d/$svc" stop 2>/dev/null || true
            log "stopped $svc"
        fi
    done

    # Wait for USB re-enumeration
    log "waiting ${SETTLE_TIME}s for USB re-enumeration..."
    sleep "$SETTLE_TIME"

    # Wait until interface reappears (max 30s)
    waited=0
    while ! ip link show "$IFACE" >/dev/null 2>&1; do
        sleep 1
        waited=$((waited + 1))
        if [ "$waited" -ge 30 ]; then
            log "ERROR: $IFACE did not reappear after 30s"
            break
        fi
    done

    if ! ip link show "$IFACE" >/dev/null 2>&1; then
        log "ERROR: giving up on $IFACE, will retry next cycle"
        continue
    fi

    log "$IFACE reappeared after ${waited}s, restoring monitor mode"

    # Restore monitor mode
    ip link set "$IFACE" down
    ip link set "$IFACE" up
    iw dev "$IFACE" set channel "$CHANNEL"

    # Re-disable autosuspend for new device
    for dev in /sys/bus/usb/devices/*/power/control; do
        echo on > "$dev" 2>/dev/null || true
    done

    # Restart daemons (reverse order)
    for svc in S97vcpd S98ulama-gw S99ulama; do
        if [ -x "/etc/init.d/$svc" ]; then
            "/etc/init.d/$svc" start 2>/dev/null || true
            log "restarted $svc"
        fi
    done

    log "stack recovered"
done
