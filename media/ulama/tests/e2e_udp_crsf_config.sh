#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/host-tools"
TMP_DIR="$(mktemp -d)"
PORT="19561"
READY_FILE="$TMP_DIR/ready"
EXPECTED_FILE="$TMP_DIR/expected.bin"
ACTUAL_FILE="$TMP_DIR/actual.bin"
CONFIG_FILE="$TMP_DIR/ulama.conf"

cleanup() {
	if [[ -n "${RX_PID:-}" ]]; then
		kill "$RX_PID" 2>/dev/null || true
		wait "$RX_PID" 2>/dev/null || true
	fi
	rm -rf "$TMP_DIR"
}
trap cleanup EXIT

make -C "$ROOT_DIR" host >/dev/null

cat > "$CONFIG_FILE" <<EOF
transport=udp
listen=127.0.0.1:$PORT
output=$ACTUAL_FILE
count=1
ready_file=$READY_FILE
EOF

"$BUILD_DIR/ulamad" --config "$CONFIG_FILE" >/dev/null 2>&1 &
RX_PID=$!

for _ in $(seq 1 100); do
	if [[ -f "$READY_FILE" ]]; then
		break
	fi
	sleep 0.05
done

if [[ ! -f "$READY_FILE" ]]; then
	echo "receiver did not become ready via config" >&2
	exit 1
fi

"$BUILD_DIR/ulama_js_tx" \
	--transport udp \
	--peer "127.0.0.1:$PORT" \
	--src-node 10 \
	--dst-node 1 \
	--channels "992,1100,300,1400,1811,172,172,172" \
	--count 1 \
	--crsf-out "$EXPECTED_FILE" \
	>/dev/null

wait "$RX_PID"
RX_PID=

cmp -s "$EXPECTED_FILE" "$ACTUAL_FILE"
echo "e2e_udp_crsf_config: ok"