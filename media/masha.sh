#!/bin/bash

##############################################################################
# masha.sh — Start/stop/manage radiod and ulama-gw daemon pair
#
# Usage:
#   ./masha.sh start [OPTIONS]     — Start both radiod and ulama-gw
#   ./masha.sh stop                — Stop both processes
#   ./masha.sh restart [OPTIONS]   — Restart both processes
#   ./masha.sh status              — Show status and logs
#   ./masha.sh logs                — Tail logs
#
# Options for 'start' and 'restart':
#   --iface IFACE                  Default: wlan0
#   --channel CH                   Default: 6
#   --tx-rate-mbps RATE            Default: 6
#   --node NODE_ID                 Default: 254
#   --cascade-in ADDR:PORT         Default: 127.0.0.1:5601
#   --cascade-out ADDR:PORT        Default: 127.0.0.1:5600
#   --cascade-host HOST            Default: localhost (sets cascade-{in,out} to HOST:PORT)
#
# Example:
#   ./masha.sh start --iface wlan0 --channel 6 --tx-rate-mbps 6 --node 254
#
##############################################################################

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$SCRIPT_DIR"

# Paths to binaries
RADIOD_BIN="${PROJECT_ROOT}/radiod/build/host-unow-tools/radiod"
ULAMA_GW_BIN="${PROJECT_ROOT}/ulama-gw/build/host-unow-tools/ulama_gw"

# Log file
LOG_DIR="/tmp/masha"
RADIOD_LOG="${LOG_DIR}/radiod.log"
ULAMA_GW_LOG="${LOG_DIR}/ulama_gw.log"

# PID files
RADIOD_PID="${LOG_DIR}/radiod.pid"
ULAMA_GW_PID="${LOG_DIR}/ulama_gw.pid"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_debug() { echo -e "${BLUE}[DEBUG]${NC} $1"; }

##############################################################################
# Parse arguments
##############################################################################

IFACE="wlan0"
CHANNEL="6"
TX_RATE_MBPS="6"
NODE_ID="254"
CASCADE_IN="127.0.0.1:5601"
CASCADE_OUT="127.0.0.1:5600"
CASCADE_HOST=""

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --iface)
                IFACE="$2"
                shift 2
                ;;
            --channel)
                CHANNEL="$2"
                shift 2
                ;;
            --tx-rate-mbps)
                TX_RATE_MBPS="$2"
                shift 2
                ;;
            --node)
                NODE_ID="$2"
                shift 2
                ;;
            --cascade-in)
                CASCADE_IN="$2"
                shift 2
                ;;
            --cascade-out)
                CASCADE_OUT="$2"
                shift 2
                ;;
            --cascade-host)
                CASCADE_HOST="$2"
                CASCADE_IN="${CASCADE_HOST}:5601"
                CASCADE_OUT="${CASCADE_HOST}:5600"
                shift 2
                ;;
            *)
                log_error "Unknown option: $1"
                exit 1
                ;;
        esac
    done
}

##############################################################################
# Check prerequisites
##############################################################################

check_binaries() {
    if [ ! -x "$RADIOD_BIN" ]; then
        log_error "radiod binary not found at $RADIOD_BIN"
        log_warn "Run: cd $PROJECT_ROOT && make -C radiod host-unow"
        exit 1
    fi
    if [ ! -x "$ULAMA_GW_BIN" ]; then
        log_error "ulama_gw binary not found at $ULAMA_GW_BIN"
        log_warn "Run: cd $PROJECT_ROOT && make -C ulama-gw host-unow"
        exit 1
    fi
}

check_interface() {
    if ! ip link show "$IFACE" >/dev/null 2>&1; then
        log_error "Interface $IFACE not found"
        exit 1
    fi
}

##############################################################################
# Start/stop functions
##############################################################################

start_radiod() {
    log_info "Starting radiod..."
    
    if pgrep -f "$RADIOD_BIN" > /dev/null 2>&1; then
        log_warn "radiod already running, stopping first..."
        pkill -f "$RADIOD_BIN" || true
        sleep 0.5
    fi
    
    # Start radiod with monitor mode setup
    mkdir -p "$LOG_DIR"
    
    # radiod should set up monitor mode itself, but ensure it's ready
    sudo "$RADIOD_BIN" \
        --iface "$IFACE" \
        --channel "$CHANNEL" \
        --tx-rate-mbps "$TX_RATE_MBPS" \
        --node "$NODE_ID" \
        >"$RADIOD_LOG" 2>&1 &
    
    local radiod_pid=$!
    echo "$radiod_pid" > "$RADIOD_PID"
    
    sleep 1
    
    if ! ps -p "$radiod_pid" > /dev/null 2>&1; then
        log_error "radiod failed to start. Check: tail -f $RADIOD_LOG"
        exit 1
    fi
    
    log_info "radiod started (PID: $radiod_pid)"
}

start_ulama_gw() {
    log_info "Starting ulama-gw..."
    
    if pgrep -f "$ULAMA_GW_BIN" > /dev/null 2>&1; then
        log_warn "ulama-gw already running, stopping first..."
        pkill -f "$ULAMA_GW_BIN" || true
        sleep 0.5
    fi
    
    mkdir -p "$LOG_DIR"
    
    # ulama-gw connects to radiod via IPC (default transport should be radiod now)
    # Add explicit transport=radiod to ensure connection
    sudo "$ULAMA_GW_BIN" \
        --cascade-in "$CASCADE_IN" \
        --cascade-out "$CASCADE_OUT" \
        --transport radiod \
        --iface "$IFACE" \
        --channel "$CHANNEL" \
        --tx-rate-mbps "$TX_RATE_MBPS" \
        --node "$NODE_ID" \
        >"$ULAMA_GW_LOG" 2>&1 &
    
    local gw_pid=$!
    echo "$gw_pid" > "$ULAMA_GW_PID"
    
    sleep 1
    
    if ! ps -p "$gw_pid" > /dev/null 2>&1; then
        log_error "ulama-gw failed to start. Check: tail -f $ULAMA_GW_LOG"
        exit 1
    fi
    
    log_info "ulama-gw started (PID: $gw_pid)"
}

stop_radiod() {
    log_info "Stopping radiod..."
    if pgrep -f "$RADIOD_BIN" > /dev/null 2>&1; then
        sudo pkill -f "$RADIOD_BIN" || pkill -f "$RADIOD_BIN" || true
        rm -f "$RADIOD_PID"
        sleep 0.5
        log_info "radiod stopped"
    else
        log_warn "radiod not running"
    fi
}

stop_ulama_gw() {
    log_info "Stopping ulama-gw..."
    if pgrep -f "$ULAMA_GW_BIN" > /dev/null 2>&1; then
        sudo pkill -f "$ULAMA_GW_BIN" || pkill -f "$ULAMA_GW_BIN" || true
        rm -f "$ULAMA_GW_PID"
        sleep 0.5
        log_info "ulama-gw stopped"
    else
        log_warn "ulama-gw not running"
    fi
}

show_status() {
    echo ""
    echo -e "${BLUE}═══ Masha Daemon Pair Status ═══${NC}"
    echo ""
    
    if pgrep -f "$RADIOD_BIN" > /dev/null 2>&1; then
        local pid=$(pgrep -f "$RADIOD_BIN" | head -1)
        echo -e "${GREEN}✓ radiod${NC} (PID: $pid)"
    else
        echo -e "${RED}✗ radiod${NC} (not running)"
    fi
    
    if pgrep -f "$ULAMA_GW_BIN" > /dev/null 2>&1; then
        local pid=$(pgrep -f "$ULAMA_GW_BIN" | head -1)
        echo -e "${GREEN}✓ ulama-gw${NC} (PID: $pid)"
    else
        echo -e "${RED}✗ ulama-gw${NC} (not running)"
    fi
    
    echo ""
    echo -e "${BLUE}Configuration:${NC}"
    echo "  Interface:    $IFACE"
    echo "  Channel:      $CHANNEL"
    echo "  TX Rate:      ${TX_RATE_MBPS} Mbps"
    echo "  Node ID:      $NODE_ID"
    echo "  Cascade In:   $CASCADE_IN"
    echo "  Cascade Out:  $CASCADE_OUT"
    
    echo ""
    echo -e "${BLUE}Logs:${NC}"
    echo "  radiod:       $RADIOD_LOG"
    echo "  ulama-gw:     $ULAMA_GW_LOG"
    echo ""
}

show_logs() {
    log_info "Showing logs (radiod on left, ulama-gw on right)..."
    echo "Press Ctrl+C to stop"
    sleep 1
    
    # Try to use 'multitail' if available, otherwise fall back to 'tail -f'
    if command -v multitail > /dev/null 2>&1; then
        multitail -l "sudo tail -f $RADIOD_LOG" -l "sudo tail -f $ULAMA_GW_LOG"
    else
        log_warn "multitail not installed, showing radiod log (use 'tail -f $ULAMA_GW_LOG' in another terminal for ulama-gw)"
        sudo tail -f "$RADIOD_LOG"
    fi
}

##############################################################################
# Main
##############################################################################

main() {
    local cmd="${1:-status}"
    shift 2>/dev/null || true
    
    case "$cmd" in
        start)
            check_binaries
            check_interface
            parse_args "$@"
            start_radiod
            # Give radiod time to open IPC socket
            sleep 1
            start_ulama_gw
            show_status
            ;;
        stop)
            stop_ulama_gw
            stop_radiod
            show_status
            ;;
        restart)
            check_binaries
            check_interface
            parse_args "$@"
            stop_ulama_gw
            stop_radiod
            sleep 1
            start_radiod
            sleep 1
            start_ulama_gw
            show_status
            ;;
        status)
            show_status
            ;;
        logs)
            show_logs
            ;;
        *)
            log_error "Unknown command: $cmd"
            echo ""
            echo "Usage: $0 {start|stop|restart|status|logs} [OPTIONS]"
            echo ""
            echo "Commands:"
            echo "  start        Start radiod and ulama-gw"
            echo "  stop         Stop both processes"
            echo "  restart      Restart both processes"
            echo "  status       Show current status"
            echo "  logs         Tail both log files"
            echo ""
            echo "Options for start/restart:"
            echo "  --iface IFACE              (default: wlan0)"
            echo "  --channel CH               (default: 6)"
            echo "  --tx-rate-mbps RATE        (default: 6)"
            echo "  --node NODE_ID             (default: 254)"
            echo "  --cascade-in ADDR:PORT     (default: 127.0.0.1:5601)"
            echo "  --cascade-out ADDR:PORT    (default: 127.0.0.1:5600)"
            echo "  --cascade-host HOST        (sets cascade endpoints to HOST:PORT)"
            exit 1
            ;;
    esac
}

main "$@"
