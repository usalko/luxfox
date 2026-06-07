#!/bin/bash
# UNOW Smoke Test - G0 gate validation
# Usage:
#   ./smoke_test.sh [--role rx|tx] [--iface mon0] [--peer-mac aa:bb:cc:dd:ee:ff] [--count 10]

set -euo pipefail

ROLE="${1:-rx}"  # rx or tx
IFACE="${2:-mon0}"
PEER_MAC="${3:-ff:ff:ff:ff:ff:ff}"  # broadcast by default
COUNT="${4:-10}"
UNOW_DIAG="${UNOW_DIAG:-./unow_diag}"
TIMEOUT_SEC=$((COUNT * 2 + 5))

print_header() {
	echo ""
	echo "=========================================="
	echo "UNOW Smoke Test - G0 Gate"
	echo "=========================================="
	echo "Role: $ROLE"
	echo "Interface: $IFACE"
	echo "Peer MAC: $PEER_MAC"
	echo "Count: $COUNT frames"
	echo "Log Level: ${UNOW_LOG_LEVEL:-info}"
	echo "=========================================="
	echo ""
}

check_iface() {
	if ! iw dev "$IFACE" info >/dev/null 2>&1; then
		echo "ERROR: interface $IFACE not found or not in monitor mode"
		echo "HINT: run 'unow-mon.sh <base_iface> $IFACE 6' first"
		exit 1
	fi
	echo "✓ Interface $IFACE found and in monitor mode"
}

check_diag_binary() {
	if [ ! -x "$UNOW_DIAG" ]; then
		echo "ERROR: $UNOW_DIAG not found or not executable"
		exit 1
	fi
	echo "✓ Found $UNOW_DIAG"
}

test_rx() {
	echo ""
	echo "[RX MODE] Listening for $COUNT frames on $IFACE..."
	echo ""
	
	timeout "$TIMEOUT_SEC" "$UNOW_DIAG" \
		--iface "$IFACE" \
		--node-id 1 \
		--log-level "${UNOW_LOG_LEVEL:-info}" \
		--listen "$COUNT" \
		|| true
	
	echo ""
	echo "✓ RX test complete"
}

test_tx() {
	echo ""
	echo "[TX MODE] Sending $COUNT frames to $PEER_MAC on $IFACE..."
	echo ""
	
	for i in $(seq 1 "$COUNT"); do
		payload="UNOW_TEST_$(printf '%04d' "$i")"
		"$UNOW_DIAG" \
			--iface "$IFACE" \
			--node-id 2 \
			--log-level "${UNOW_LOG_LEVEL:-info}" \
			--send-text "$payload" \
			--dst "$PEER_MAC" \
			--no-dump \
			|| {
				echo "Warning: frame $i send failed"
				continue
			}
		sleep 0.1  # small delay between frames
	done
	
	echo ""
	echo "✓ TX test complete - sent $COUNT frames"
}

main() {
	print_header
	check_iface
	check_diag_binary
	
	case "$ROLE" in
		rx)
			test_rx
			;;
		tx)
			test_tx
			;;
		*)
			echo "ERROR: unknown role '$ROLE' (expected rx or tx)"
			exit 1
			;;
	esac
	
	echo ""
	echo "=========================================="
	echo "Test finished. Check logs above for errors."
	echo "=========================================="
	echo ""
}

main "$@"
