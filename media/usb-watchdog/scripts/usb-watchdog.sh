#!/bin/sh
# usb-watchdog.sh — detect USB Wi-Fi adapter re-enumeration, restore radio stack
#
# Problem: USB hub on LuckFox periodically resets under load (power budget).
# The adapter reappears as a new phy (phy#1 → phy#2 → ...) but ulamad holds
# a dead pcap handle to the old phy. Just checking "wlan0 exists" is not enough.
#
# Solution: track phy number + monitor mode + channel. On any change — full
# recovery: stop daemons, restore monitor mode, restart daemons.

IFACE="${1:-wlan0}"
CHECK_INTERVAL="${2:-3}"
CHANNEL="${3:-6}"
SETTLE_TIME=2

# Current known-good state
KNOWN_PHY=""
RECOVERY_COUNT=0

log() {
    logger -t usb-watchdog "$1"
    echo "[usb-watchdog] $(date '+%H:%M:%S') $1"
}

# Get phy number for interface (e.g. "wiphy 2" → "2", empty if iface missing)
get_phy() {
    iw dev "$IFACE" info 2>/dev/null | awk '/wiphy/{print $2}'
}

# Get interface type (monitor/managed/etc, empty if iface missing)
get_type() {
    iw dev "$IFACE" info 2>/dev/null | awk '/type/{print $2}'
}

# Get current channel (empty if not set)
get_channel() {
    iw dev "$IFACE" info 2>/dev/null | awk '/channel/{print $2}'
}

# Check if interface is healthy: exists + monitor mode + correct channel
is_healthy() {
    local phy type ch
    phy=$(get_phy)
    [ -z "$phy" ] && return 1

    type=$(get_type)
    [ "$type" != "monitor" ] && return 1

    ch=$(get_channel)
    [ "$ch" != "$CHANNEL" ] && return 1

    # Phy changed from what we knew — device was re-enumerated
    if [ -n "$KNOWN_PHY" ] && [ "$phy" != "$KNOWN_PHY" ]; then
        return 1
    fi

    return 0
}

stop_daemons() {
    for svc in S99ulama S98ulama-gw S97vcpd; do
        if [ -x "/etc/init.d/$svc" ]; then
            "/etc/init.d/$svc" stop 2>/dev/null || true
        fi
    done
}

start_daemons() {
    for svc in S97vcpd S98ulama-gw S99ulama; do
        if [ -x "/etc/init.d/$svc" ]; then
            "/etc/init.d/$svc" start 2>/dev/null || true
        fi
    done
}

restore_monitor_mode() {
    ip link set "$IFACE" down 2>/dev/null
    ip link set "$IFACE" up 2>/dev/null
    iw dev "$IFACE" set channel "$CHANNEL" 2>/dev/null

    # Verify
    local type ch
    type=$(get_type)
    ch=$(get_channel)
    if [ "$type" = "monitor" ] && [ "$ch" = "$CHANNEL" ]; then
        return 0
    fi

    log "monitor mode restore failed (type=$type ch=$ch), retrying..."
    sleep 1
    ip link set "$IFACE" down 2>/dev/null
    iw dev "$IFACE" set type monitor 2>/dev/null || true
    ip link set "$IFACE" up 2>/dev/null
    iw dev "$IFACE" set channel "$CHANNEL" 2>/dev/null
}

disable_usb_autosuspend() {
    for dev in /sys/bus/usb/devices/*/power/control; do
        echo on > "$dev" 2>/dev/null || true
    done
}

do_recovery() {
    local reason="$1"
    RECOVERY_COUNT=$((RECOVERY_COUNT + 1))
    log "RECOVERY #$RECOVERY_COUNT: $reason"

    stop_daemons

    # If interface is gone, wait for re-enumeration
    if ! ip link show "$IFACE" >/dev/null 2>&1; then
        log "waiting for $IFACE to reappear..."
        local waited=0
        while ! ip link show "$IFACE" >/dev/null 2>&1; do
            sleep 1
            waited=$((waited + 1))
            if [ "$waited" -ge 30 ]; then
                log "ERROR: $IFACE did not reappear after 30s"
                return 1
            fi
        done
        log "$IFACE reappeared after ${waited}s"
        sleep "$SETTLE_TIME"
    fi

    restore_monitor_mode
    disable_usb_autosuspend

    KNOWN_PHY=$(get_phy)
    log "new phy=$KNOWN_PHY type=$(get_type) ch=$(get_channel)"

    start_daemons
    log "stack recovered (total recoveries: $RECOVERY_COUNT)"
}

# --- Main ---

log "started: iface=$IFACE channel=$CHANNEL interval=${CHECK_INTERVAL}s"

# Record initial state
if ip link show "$IFACE" >/dev/null 2>&1; then
    KNOWN_PHY=$(get_phy)
    log "initial phy=$KNOWN_PHY type=$(get_type) ch=$(get_channel)"

    # Fix state at startup if needed
    if ! is_healthy; then
        do_recovery "initial state not healthy"
    fi
else
    log "WARNING: $IFACE not present at startup, waiting..."
fi

while true; do
    sleep "$CHECK_INTERVAL"

    if is_healthy; then
        continue
    fi

    # Determine reason
    phy=$(get_phy)
    type=$(get_type)
    ch=$(get_channel)

    if [ -z "$phy" ]; then
        do_recovery "$IFACE disappeared"
    elif [ -n "$KNOWN_PHY" ] && [ "$phy" != "$KNOWN_PHY" ]; then
        do_recovery "phy changed ($KNOWN_PHY → $phy) — USB re-enumeration"
    elif [ "$type" != "monitor" ]; then
        do_recovery "not in monitor mode (type=$type)"
    elif [ "$ch" != "$CHANNEL" ]; then
        do_recovery "wrong channel ($ch, want $CHANNEL)"
    else
        do_recovery "unknown unhealthy state"
    fi
done
