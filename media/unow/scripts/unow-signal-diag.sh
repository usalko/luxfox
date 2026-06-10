#!/bin/sh
# ==============================================================================
# unow-signal-diag.sh
#
# Periodically measures Wi-Fi signal level for a custom BSSID and writes a
# rotated log file suitable for long-range drone diagnostics.
#
# Usage:
#   ./scripts/unow-signal-diag.sh [--once] [--iface wlan0] [--bssid 55:4E:4F:57:00:01]
#
# Notes:
#   - The script intentionally uses a fixed custom BSSID to identify the peer
#     drone in the diagnostic network.
#   - Logs are rotated automatically when they exceed 500 MiB.
#   - The script is intended to be wrapped by /etc/init.d on the target device.
# ==============================================================================

set -eu

BASE_IFACE="${BASE_IFACE:-wlan0}"
CUSTOM_BSSID="${CUSTOM_BSSID:-55:4E:4F:57:00:01}"
LOG_DIR="${LOG_DIR:-/var/log/unow}"
LOG_FILE="${LOG_FILE:-${LOG_DIR}/wifi_signal_diag.log}"
SAMPLE_PERIOD="${SAMPLE_PERIOD:-1}"
MAX_LOG_BYTES="${MAX_LOG_BYTES:-524288000}"
RUN_ONCE="0"

usage() {
    cat <<'EOF'
Usage: unow-signal-diag.sh [--once] [--iface wlan0] [--bssid 55:4E:4F:57:00:01]

Options:
  --once             Run one sample and exit.
  --iface IFACE      Base Wi-Fi interface to use (default: wlan0).
  --bssid BSSID      Custom BSSID to watch for (default: 55:4E:4F:57:00:01).
  --log-dir PATH     Log directory (default: /var/log/unow).
  --log-file PATH    Log file path (default: /var/log/unow/wifi_signal_diag.log).
  --period SEC       Sampling period in seconds (default: 1).
  --help             Show this help.
EOF
}

log() {
    printf '[%s] %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$*"
}

rotate_log_if_needed() {
    mkdir -p "$(dirname "$LOG_FILE")"

    if [ ! -f "$LOG_FILE" ]; then
        return 0
    fi

    size_bytes=$(wc -c <"$LOG_FILE" 2>/dev/null || echo 0)
    if [ "$size_bytes" -ge "$MAX_LOG_BYTES" ]; then
        log "Rotating log file (size=${size_bytes} bytes, limit=${MAX_LOG_BYTES})"
        mv "$LOG_FILE" "$LOG_FILE.1" 2>/dev/null || true
        gzip -f "$LOG_FILE.1" 2>/dev/null || true
    fi
}

collect_sample() {
    if ! command -v iw >/dev/null 2>&1; then
        log "ERROR: iw is not available"
        return 1
    fi

    if ! ip link show "$BASE_IFACE" >/dev/null 2>&1; then
        log "ERROR: interface '$BASE_IFACE' is not present"
        return 1
    fi

    # Make sure the interface is up and uses the custom BSSID we want to observe.
    ip link set "$BASE_IFACE" up 2>/dev/null || true
    iw dev "$BASE_IFACE" set type managed 2>/dev/null || true
    iw dev "$BASE_IFACE" set bssid "$CUSTOM_BSSID" 2>/dev/null || true

    scan_output=$(iw dev "$BASE_IFACE" scan dump 2>/dev/null || true)
    if [ -z "$scan_output" ]; then
        log "WARN: no scan output available from '$BASE_IFACE'"
        return 0
    fi

    sample=$(printf '%s\n' "$scan_output" | awk -v target="$CUSTOM_BSSID" '
        BEGIN { mac=""; rssi="" }
        $1 == "BSSID:" { mac = $2 }
        $1 == "signal:" { rssi = $2 }
        (mac == target && rssi != "") { print mac, rssi; exit }
    ')

    if [ -n "$sample" ]; then
        peer_mac=$(printf '%s\n' "$sample" | awk '{print $1}')
        rssi=$(printf '%s\n' "$sample" | awk '{print $2}')
        printf '%s peer_mac=%s rssi=%s custom_bssid=%s\n' \
            "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$peer_mac" "$rssi" "$CUSTOM_BSSID" >> "$LOG_FILE"
        log "sample peer_mac=$peer_mac rssi=$rssi custom_bssid=$CUSTOM_BSSID"
    else
        printf '%s peer_mac=none rssi=unknown custom_bssid=%s\n' \
            "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$CUSTOM_BSSID" >> "$LOG_FILE"
        log "sample peer_mac=none rssi=unknown custom_bssid=$CUSTOM_BSSID"
    fi
}

while [ $# -gt 0 ]; do
    case "$1" in
        --once) RUN_ONCE=1; shift ;;
        --iface) BASE_IFACE="${2:-$BASE_IFACE}"; shift 2 ;;
        --bssid) CUSTOM_BSSID="${2:-$CUSTOM_BSSID}"; shift 2 ;;
        --log-dir) LOG_DIR="${2:-$LOG_DIR}"; shift 2 ;;
        --log-file) LOG_FILE="${2:-$LOG_FILE}"; shift 2 ;;
        --period) SAMPLE_PERIOD="${2:-$SAMPLE_PERIOD}"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

LOG_FILE="${LOG_FILE:-${LOG_DIR}/wifi_signal_diag.log}"

log "starting unow signal diagnostic on iface=$BASE_IFACE bssid=$CUSTOM_BSSID log=$LOG_FILE interval=${SAMPLE_PERIOD}s"
rotate_log_if_needed

while true; do
    rotate_log_if_needed
    collect_sample || true
    if [ "$RUN_ONCE" = "1" ]; then
        exit 0
    fi
    sleep "$SAMPLE_PERIOD"
done
