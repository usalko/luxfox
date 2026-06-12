#!/bin/bash
# ==============================================================================
# UNOW G0 Smoke Test — RUNME_G0_TEST.sh
# ==============================================================================
# Quick-start script for bidirectional inject/capture validation
# Usage:
#   1. On LuckFox:  bash RUNME_G0_TEST.sh luckfox rx
#   2. On Desktop:  bash RUNME_G0_TEST.sh desktop tx
# ==============================================================================

set -euo pipefail

HOST="${1:-unknown}"  # "luckfox" or "desktop"
ROLE="${2:-unknown}"  # "rx" or "tx"

print_banner() {
	cat <<'EOF'
┌─────────────────────────────────────────────────────────────────┐
│                  UNOW G0 SMOKE TEST QUICK-START                 │
└─────────────────────────────────────────────────────────────────┘
EOF
}

print_luckfox_rx() {
	cat <<'EOF'

╔═══════════════════════════════════════════════════════════════════╗
║ LuckFox RX Mode — Listening for frames                           ║
╚═══════════════════════════════════════════════════════════════════╝

STEP 1: Bring up monitor interface on LuckFox
─────────────────────────────────────────────────────────────────

  Identify your Wi-Fi adapter:
    $ iw dev

  Then run monitor setup (change 'wlx088af1287d57' to YOUR adapter):
    $ bash media/unow/scripts/unow-mon.sh wlx088af1287d57 mon0 6

  Verify mon0 is up and on channel 6:
    $ iw dev mon0 info
    $ iw dev mon0 link

STEP 2: Start listening on mon0
─────────────────────────────────────────────────────────────────

  Run this UNOW listener (default: listen for 10 frames, timeout 30s):
    $ export UNOW_LOG_LEVEL=info
    $ ./media/unow/out/bin/unow_diag --iface mon0 --node-id 1 --listen 10

  (Keep this running in a terminal!)

STEP 3: Prepare Desktop for TX (see other terminal)
─────────────────────────────────────────────────────────────────

  Wait for Desktop to send frames...
  Expected: LuckFox terminal will print:
    [UNOW_INFO] Frame received: src=xx:xx:xx:xx:xx:xx len=NNN rssi=-50 dBm

STEP 4: Check results
─────────────────────────────────────────────────────────────────

  If you see 9+ frames with RSSI > -80 dBm:  ✓ G0 PASSED
  If not:  check /tmp/unow_g0_log.txt for errors

EOF
}

print_desktop_tx() {
	cat <<'EOF'

╔═══════════════════════════════════════════════════════════════════╗
║ Desktop TX Mode — Sending frames to LuckFox                      ║
╚═══════════════════════════════════════════════════════════════════╝

STEP 1: Bring up monitor interface on Desktop
─────────────────────────────────────────────────────────────────

  Identify your MW300UH adapter:
    $ lsusb | grep -i realtek
    $ iw dev

  Kill interfering network managers:
    $ sudo airmon-ng check kill

  Bring down and create monitor mode:
    $ sudo ip link set wlx00e04c123456 down
    $ sudo iw dev wlx00e04c123456 interface add mon0 type monitor
    $ sudo ip link set mon0 up
    $ sudo iw dev mon0 set channel 6

  Verify:
    $ iw dev mon0 info
    $ iw dev mon0 link

STEP 2: Send test frames to LuckFox
─────────────────────────────────────────────────────────────────

  Make sure LuckFox is listening first! (see other terminal)

  Then send 10 test frames to broadcast address:
    $ export UNOW_LOG_LEVEL=debug
    $ ./media/unow/out/bin/unow_diag \\
      --iface mon0 \\
      --node-id 2 \\
      --send-text "G0_TEST_Frame" \\
      --dst ff:ff:ff:ff:ff:ff \\
      --listen 0

  Expected: unow_diag shows:
    [UNOW_INFO] Initialized iface=mon0, self_mac=xx:xx:xx:xx:xx:xx
    [UNOW_DEBUG] Injected frame 0, len=XX

STEP 3: Verify LuckFox received frames
─────────────────────────────────────────────────────────────────

  Check LuckFox terminal for output:
    [UNOW_INFO] Frame received: src=xx:xx:xx:xx:xx:xx len=39 rssi=-50 dBm

  If you see 9+ frames on LuckFox:  ✓ G0 PASSED (LuckFox→Desktop works!)

STEP 4: Cleanup
─────────────────────────────────────────────────────────────────

  After test, tear down monitor mode:
    $ bash media/unow/scripts/unow-down.sh mon0
    $ sudo airmon-ng check start

EOF
}

print_desktop_rx() {
	cat <<'EOF'

╔═══════════════════════════════════════════════════════════════════╗
║ Desktop RX Mode — Using tcpdump (alternative)                    ║
╚═══════════════════════════════════════════════════════════════════╝

If unow_diag is not compiled for x86, use tcpdump to inspect frames:

  $ sudo tcpdump -i mon0 -e 'wlan[24] == 127' -c 10

Expected output (vendor action frames with category 127):
  ...
  SA:xx:xx:xx:xx:xx:xx > DA:ff:ff:ff:ff:ff:ff ... [mgmt subtype: ACTION (13)]
  ...

EOF
}

print_luckfox_tx() {
	cat <<'EOF'

╔═══════════════════════════════════════════════════════════════════╗
║ LuckFox TX Mode (optional reverse test)                          ║
╚═══════════════════════════════════════════════════════════════════╝

To test LuckFox → Desktop direction:

STEP 1: Setup monitor on both sides (see STEP 1 in RX/TX sections)

STEP 2: Start Desktop RX
  (Use tcpdump or desktop unow_diag in --listen mode)

STEP 3: Send from LuckFox
  $ ./media/unow/out/bin/unow_diag --iface mon0 --node-id 1 --send-text "LF→Desktop"

STEP 4: Check Desktop tcpdump for frames

EOF
}

main() {
	print_banner
	
	case "$HOST:$ROLE" in
		luckfox:rx)
			print_luckfox_rx
			;;
		luckfox:tx)
			print_luckfox_tx
			;;
		desktop:rx)
			print_desktop_rx
			;;
		desktop:tx)
			print_desktop_tx
			;;
		*)
			echo "ERROR: Unknown host:role combination '$HOST:$ROLE'"
			echo ""
			echo "Usage:"
			echo "  LuckFox RX:     bash RUNME_G0_TEST.sh luckfox rx"
			echo "  LuckFox TX:     bash RUNME_G0_TEST.sh luckfox tx"
			echo "  Desktop RX:     bash RUNME_G0_TEST.sh desktop rx"
			echo "  Desktop TX:     bash RUNME_G0_TEST.sh desktop tx"
			exit 1
			;;
	esac
	
	cat <<'EOF'

╔═══════════════════════════════════════════════════════════════════╗
║ Notes:                                                            ║
║  • Both sides must be on the same Wi-Fi channel (e.g., 6)         ║
║  • Keep adapters ~1m apart                                       ║
║  • Log output goes to stderr (redirect with 2>&1)                ║
║  • For more details, see media/unow/README.md                    ║
╚═══════════════════════════════════════════════════════════════════╝

EOF
}

main "$@"
