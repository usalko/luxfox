#!/bin/bash
# =============================================================================
#  SUPER ACK RELIABILITY TEST
#
#  End-to-end test of UNOW ACK/retry over two physical WiFi adapters.
#  Runs unreliable vs reliable at 0%, 25%, 50%, 75% simulated packet loss.
#
#  Usage:
#    sudo ./super_ack_test.sh [wlan0] [wlan1] [channel] [count] [rate]
#
#  Requirements:
#    - Two WiFi adapters in (or switchable to) monitor mode
#    - Root privileges
#    - super_ack_test binary built (see below)
# =============================================================================

set -euo pipefail

IFACE_TX="${1:-wlan0}"
IFACE_RX="${2:-wlan1}"
CHANNEL="${3:-6}"
COUNT="${4:-1000}"
RATE="${5:-150}"
DROP_RATES="0 25 50 75"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNOW_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_BIN="$SCRIPT_DIR/super_ack_test"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

die() { echo -e "${RED}[FATAL]${NC} $1" >&2; exit 1; }

# ---- Build ------------------------------------------------------------------
build_test() {
    echo -e "${CYAN}[BUILD]${NC} Compiling super_ack_test..."
    gcc -o "$TEST_BIN" \
        "$SCRIPT_DIR/super_ack_test.c" \
        "$UNOW_ROOT/src/unow.c" \
        "$UNOW_ROOT/src/unow_diag.c" \
        "$UNOW_ROOT/src/unow_iface.c" \
        "$UNOW_ROOT/src/unow_radiotap.c" \
        -I"$UNOW_ROOT/include" \
        -I"$UNOW_ROOT/src" \
        -D_DEFAULT_SOURCE \
        -g -Wall -Wextra -std=c11 \
        -lpcap -lpthread \
    || die "compilation failed"
    echo -e "${GREEN}[BUILD]${NC} OK"
}

# ---- Interface setup --------------------------------------------------------
setup_iface() {
    local iface="$1"
    local ch="$2"

    echo -e "${CYAN}[SETUP]${NC} $iface -> monitor ch$ch"

    # Bring down, set channel, bring up
    ip link set "$iface" down 2>/dev/null || true
    ip link set "$iface" up   2>/dev/null || true
    iw dev "$iface" set channel "$ch" 2>/dev/null || true

    # Verify monitor mode
    local iftype
    iftype=$(iw dev "$iface" info 2>/dev/null | grep -oP 'type \K\w+' || echo "unknown")
    if [ "$iftype" != "monitor" ]; then
        echo -e "${YELLOW}[WARN]${NC} $iface type=$iftype (expected monitor)"
        echo -e "${YELLOW}[WARN]${NC} Attempting: iw dev $iface set type monitor"
        ip link set "$iface" down
        iw dev "$iface" set type monitor 2>/dev/null || true
        ip link set "$iface" up
        iw dev "$iface" set channel "$ch" 2>/dev/null || true
    fi

    # Get MAC
    local mac
    mac=$(iw dev "$iface" info 2>/dev/null | grep -oP 'addr \K[0-9a-f:]+' || echo "??:??:??:??:??:??")
    echo -e "${GREEN}[SETUP]${NC} $iface OK  mac=$mac  ch=$ch"
}

# ---- Run single test --------------------------------------------------------
# Globals for result collection
declare -a RESULTS=()

run_one() {
    local drop="$1"
    local mode="$2"
    local extra_flag=""

    [ "$mode" = "reliable" ] && extra_flag="--reliable"

    local raw
    raw=$("$TEST_BIN" \
        --iface-tx "$IFACE_TX" \
        --iface-rx "$IFACE_RX" \
        --count "$COUNT" \
        --rate "$RATE" \
        --drop "$drop" \
        --log-level error \
        $extra_flag 2>/dev/null) || true

    local result_line
    result_line=$(echo "$raw" | grep '^\[RESULT\]' || echo "")

    if [ -z "$result_line" ]; then
        RESULTS+=("$drop|$mode|-|-|-|-|-")
        return
    fi

    local tx_sent tx_retries rx_unique rx_dedup per tx_ms
    tx_sent=$(echo "$result_line"  | grep -oP 'tx_sent=\K[0-9]+' || echo "0")
    tx_retries=$(echo "$result_line" | grep -oP 'tx_retries=\K[0-9]+' || echo "0")
    rx_unique=$(echo "$result_line" | grep -oP 'rx_unique=\K[0-9]+' || echo "0")
    rx_dedup=$(echo "$result_line"  | grep -oP 'rx_dedup=\K[0-9]+' || echo "0")
    per=$(echo "$result_line"       | grep -oP 'PER=\K[0-9.]+' || echo "?")
    tx_ms=$(echo "$result_line"     | grep -oP 'tx_ms=\K[0-9]+' || echo "0")

    local throughput="-"
    if [ "$tx_ms" -gt 0 ] 2>/dev/null; then
        throughput=$(awk "BEGIN { printf \"%.0f\", $tx_sent / ($tx_ms / 1000.0) }")
    fi

    [ "$mode" = "unreliable" ] && tx_retries="-"

    RESULTS+=("$drop|$mode|$tx_sent|$rx_unique|${per}%|$tx_retries|$throughput")
}

# ---- Print results table ----------------------------------------------------
print_table() {
    echo ""
    echo -e "${BOLD}========================================================================${NC}"
    echo -e "${BOLD}                   SUPER ACK RELIABILITY TEST                           ${NC}"
    echo -e "${BOLD}========================================================================${NC}"
    echo -e " TX: $IFACE_TX    RX: $IFACE_RX    Channel: $CHANNEL"
    echo -e " Packets: $COUNT   Target rate: $RATE pkt/s"
    echo -e "${BOLD}------------------------------------------------------------------------${NC}"
    printf " %-6s | %-12s | %-7s | %-7s | %-8s | %-8s | %-8s\n" \
           "Drop%" "Mode" "TX" "RX ok" "PER" "Retries" "pkt/s"
    echo -e "${BOLD}------------------------------------------------------------------------${NC}"

    for row in "${RESULTS[@]}"; do
        IFS='|' read -r drop mode tx rx per retries tput <<< "$row"
        local color="$NC"
        # Color PER: green <1%, yellow <10%, red >=10%
        local per_num="${per//%/}"
        if [[ "$per_num" =~ ^[0-9.]+$ ]]; then
            if (( $(awk "BEGIN { print ($per_num < 1.0) }") )); then
                color="$GREEN"
            elif (( $(awk "BEGIN { print ($per_num < 10.0) }") )); then
                color="$YELLOW"
            else
                color="$RED"
            fi
        fi
        printf " %5s%% | %-12s | %7s | %7s | ${color}%8s${NC} | %8s | %8s\n" \
               "$drop" "$mode" "$tx" "$rx" "$per" "$retries" "$tput"
    done

    echo -e "${BOLD}========================================================================${NC}"
    echo ""
}

# ---- Main -------------------------------------------------------------------
main() {
    echo ""
    echo -e "${BOLD}=== SUPER ACK RELIABILITY TEST ===${NC}"
    echo ""

    [ "$(id -u)" -ne 0 ] && die "must run as root (sudo)"

    build_test
    setup_iface "$IFACE_TX" "$CHANNEL"
    setup_iface "$IFACE_RX" "$CHANNEL"

    echo ""
    echo -e "${CYAN}[TEST]${NC} Running test matrix: drop={$DROP_RATES}% x {unreliable, reliable}"
    echo ""

    for drop in $DROP_RATES; do
        echo -e "${CYAN}[TEST]${NC} drop=${drop}% unreliable..."
        run_one "$drop" "unreliable"
        sleep 1

        echo -e "${CYAN}[TEST]${NC} drop=${drop}% reliable..."
        run_one "$drop" "reliable"
        sleep 1
    done

    print_table
}

main "$@"
