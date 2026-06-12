#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
HOST_BIN_DIR="$ROOT_DIR/build/host-tools"
TARGET_BIN_DIR="$ROOT_DIR/out/bin"

usage() {
	cat <<'EOF'
usage:
  run_unow_crsf_smoke.sh host-tx [options]
  run_unow_crsf_smoke.sh luckfox-rx [options]

modes:
  host-tx
    Send fixed CRSF channel frames or live joystick input over UNOW.
  luckfox-rx
    Receive ULAMA frames over UNOW and forward CRSF bytes to UART3 or file.

common examples:
  host-tx fixed pattern:
    ./tests/run_unow_crsf_smoke.sh host-tx \
      --iface mon0 --dst-mac aa:bb:cc:dd:ee:ff --count 200 --rate-hz 50

  host-tx live joystick:
    ./tests/run_unow_crsf_smoke.sh host-tx \
      --iface mon0 --dst-mac aa:bb:cc:dd:ee:ff --joystick /dev/input/js0 --arm

  luckfox receiver to Betaflight UART3:
    ./tests/run_unow_crsf_smoke.sh luckfox-rx \
      --iface mon0 --node 1 --uart /dev/ttyS3 --baud 420000

options for host-tx:
  --iface IFACE         Monitor interface, default mon0
  --dst-mac MAC         Peer MAC for UNOW unicast
  --src-node ID         ULAMA source node, default 10
  --dst-node ID         ULAMA destination node, default 1
  --count N             Number of frames in synthetic mode, default 200
  --rate-hz N           Synthetic send rate, default 50
  --channels CSV        16 CRSF channel values, default preset suitable for smoke
  --joystick PATH       Live joystick device path
  --arm                 Force AUX1 high on startup

options for luckfox-rx:
  --iface IFACE         Monitor interface, default mon0
  --node ID             Local ULAMA node, default 1
  --uart PATH           UART path, default /dev/ttyS3
  --baud RATE           UART baud, default 420000
  --output PATH         Write CRSF bytes to file instead of UART
  --verbose             Print decoded channel summary
EOF
}

run_host_tx() {
	local iface="mon0"
	local dst_mac=""
	local src_node="10"
	local dst_node="1"
	local count="200"
	local rate_hz="50"
	local channels="992,992,172,992,1811,172,172,172,172,172,172,172,172,172,172,172"
	local joystick=""
	local arm_flag=""

	while [[ $# -gt 0 ]]; do
		case "$1" in
			--iface) iface="$2"; shift 2 ;;
			--dst-mac) dst_mac="$2"; shift 2 ;;
			--src-node) src_node="$2"; shift 2 ;;
			--dst-node) dst_node="$2"; shift 2 ;;
			--count) count="$2"; shift 2 ;;
			--rate-hz) rate_hz="$2"; shift 2 ;;
			--channels) channels="$2"; shift 2 ;;
			--joystick) joystick="$2"; shift 2 ;;
			--arm) arm_flag="--arm"; shift ;;
			-h|--help) usage; exit 0 ;;
			*) echo "unknown option for host-tx: $1" >&2; exit 2 ;;
		esac
		done

	if [[ -z "$dst_mac" ]]; then
		echo "host-tx requires --dst-mac" >&2
		exit 2
	fi

	if [[ ! -x "$HOST_BIN_DIR/ulama_js_tx" ]]; then
		make -C "$ROOT_DIR" host >/dev/null
	fi

	if [[ -n "$joystick" ]]; then
		exec "$HOST_BIN_DIR/ulama_js_tx" \
			--transport unow \
			--iface "$iface" \
			--dst-mac "$dst_mac" \
			--src-node "$src_node" \
			--dst-node "$dst_node" \
			--joystick "$joystick" \
			$arm_flag \
			--verbose
	fi

	exec "$HOST_BIN_DIR/ulama_js_tx" \
		--transport unow \
		--iface "$iface" \
		--dst-mac "$dst_mac" \
		--src-node "$src_node" \
		--dst-node "$dst_node" \
		--channels "$channels" \
		--count "$count" \
		--rate-hz "$rate_hz" \
		$arm_flag \
		--verbose
}

run_luckfox_rx() {
	local iface="mon0"
	local node="1"
	local uart="/dev/ttyS3"
	local baud="420000"
	local output=""
	local verbose_flag=""
	local bin="$TARGET_BIN_DIR/ulamad"

	while [[ $# -gt 0 ]]; do
		case "$1" in
			--iface) iface="$2"; shift 2 ;;
			--node) node="$2"; shift 2 ;;
			--uart) uart="$2"; shift 2 ;;
			--baud) baud="$2"; shift 2 ;;
			--output) output="$2"; shift 2 ;;
			--verbose) verbose_flag="--verbose"; shift ;;
			-h|--help) usage; exit 0 ;;
			*) echo "unknown option for luckfox-rx: $1" >&2; exit 2 ;;
		esac
		done

	if [[ ! -x "$bin" ]]; then
		bin="$ROOT_DIR/ulamad"
	fi
	if [[ ! -x "$bin" ]]; then
		echo "ulamad binary not found; run 'make -C media/ulama' first" >&2
		exit 2
	fi

	if [[ -n "$output" ]]; then
		exec "$bin" \
			--transport unow \
			--iface "$iface" \
			--node "$node" \
			--output "$output" \
			$verbose_flag
	fi

	exec "$bin" \
		--transport unow \
		--iface "$iface" \
		--node "$node" \
		--uart "$uart" \
		--baud "$baud" \
		$verbose_flag
}

main() {
	if [[ $# -lt 1 ]]; then
		usage
		exit 2
	fi

	case "$1" in
		host-tx)
			shift
			run_host_tx "$@"
			;;
		luckfox-rx)
			shift
			run_luckfox_rx "$@"
			;;
		-h|--help)
			usage
			;;
		*)
			echo "unknown mode: $1" >&2
			usage
			exit 2
			;;
	esac
}

main "$@"