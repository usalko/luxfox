# UNOW Smoke Testing Guide

## Gate 0 (G0): Raw Inject/Capture Validation

**Goal:** Prove that inject + capture works bidirectionally on both LuckFox and Desktop via MW300UH (RTL8192EU) adapters. This is the **hardest risk** — if this fails, nothing else matters.

### Prerequisites

#### On LuckFox Pico Ultra
- `unow_diag` binary in `/usr/bin/` (or `./media/unow/out/bin/unow_diag`)
- `unow-mon.sh` script available
- Network interface for MW300UH recognized (typically `wlx...` or similar)

#### On Ubuntu Desktop
- Second Wi-Fi adapter connected (MW300UH RTL8192EU or compatible)
- `libpcap-dev` installed: `sudo apt install libpcap-dev`
- `iw` and `tcpdump` available: `sudo apt install wireless-tools dnsmasq`
- Optional: `airmon-ng` (from `aircrack-ng`): `sudo apt install aircrack-ng`

### Test Steps

#### Step 1: Identify Adapters

**On LuckFox:**
```bash
iw dev
```
Look for your Wi-Fi interface, e.g., `wlx088af1287d57` or internal `aic8800dc`.

**On Desktop (Ubuntu):**
```bash
iw dev
lsusb | grep -i realtek
```
Identify the MW300UH adapter, e.g., `wlx00e04c123456`.

#### Step 2: Prepare Interfaces (Both Sides)

**On LuckFox:**
```bash
# Option A: If using external MW300UH adapter
IFACE=wlan1  # Replace with your adapter
unow-mon.sh "$IFACE" mon0 6

# Verify
iw dev mon0 info
ip link show mon0
```

**On Desktop:**
```bash
IFACE=wlx00e04c123456  # Replace with your adapter
# Kill interfering processes
sudo airmon-ng check kill

# Bring up monitor mode
sudo ip link set "$IFACE" down
sudo iw dev "$IFACE" interface add mon0 type monitor
sudo ip link set mon0 up
sudo iw dev mon0 set channel 6

# Verify
iw dev mon0 info
```

#### Step 3a: LuckFox → Desktop (RX on Desktop)

**On Desktop (Terminal 1):**
```bash
# Listen for UNOW frames
sudo tcpdump -i mon0 -e -c 5
```

**On LuckFox (Terminal):**
```bash
# Send a test packet
./media/unow/out/bin/unow_diag \
  --iface mon0 \
  --node-id 1 \
  --log-level debug \
  --send-text "HelloDesktop" \
  --listen 0
```

**Expected on Desktop:**
- `tcpdump` should show 802.11 frames with payload containing "HelloDesktop"
- Frame structure: `[RadioTap][Dot11 Management/Action][UNOW OUI+payload]`

#### Step 3b: Desktop → LuckFox (RX on LuckFox)

**On LuckFox (Terminal 1):**
```bash
# Listen for 5 UNOW frames with timeout
./media/unow/out/bin/unow_diag \
  --iface mon0 \
  --node-id 2 \
  --log-level info \
  --listen 5
```

**On Desktop (Terminal):**
```bash
# Send a test packet to LuckFox's MAC (or broadcast)
# First, get LuckFox's MAC:
# (You'll see it in the debug output from step 3a)

# Build the command (example with broadcast):
sudo tcpdump -i mon0 -w /tmp/unow_test.pcap &
CAPTURE_PID=$!

sleep 1

# Inject via unow_diag (if available on desktop)
# OR manually craft a frame with aireplay-ng:
sudo aireplay-ng --test mon0

sleep 2
sudo kill $CAPTURE_PID
```

**Expected on LuckFox:**
- `unow_diag --listen 5` prints frames received with RSSI
- Log output shows src_mac, payload, and RSSI values

### Troubleshooting

| Issue | Diagnosis | Fix |
|---|---|---|
| `iw dev ... info` fails | Interface not in monitor mode | Re-run `unow-mon.sh` or `iw set monitor control otherbss` |
| `tcpdump` sees no frames | Adapter not injecting or on wrong channel | Verify `iw dev mon0 set channel 6` on both sides; check `sudo iw reg get` for region restrictions |
| Frames seen in `tcpdump` but not parsed by `unow_diag` | UNOW OUI/BSSID mismatch | Check constants in `media/unow/include/unow/unow_wire.h`; run with `--log-level trace` |
| `pcap_inject failed` | Driver doesn't support injection on RTL8192EU | Try `aic8800dc` (LuckFox internal) or alternative adapter (mt76, ath9k_htc) |
| High packet loss (PER > 5%) | Channel congestion or low TX power | Switch to less busy channel (1, 6, 11 for 2.4 GHz); check `iw dev mon0 info` for TX power |

### Success Criteria (G0 Passing)

✅ **Bidirectional frames visible in `tcpdump` and parsed by `unow_diag`**
- At least 90% of 10 test packets reach destination with RSSI reading
- No fatal errors in inject or receive paths

✅ **Performance baseline**
- Single-hop latency < 100 ms (including CPU overhead)
- No frame corruption visible in payload

### Log Level Reference

Use `--log-level` to control verbosity:
- `error`: Only fatal issues
- `warn`: Warnings + errors
- `info`: Key events (default)
- `debug`: Frame-level details
- `trace`: RadioTap hex dumps, BPF filter details

Example with environment:
```bash
UNOW_LOG_LEVEL=trace ./media/unow/out/bin/unow_diag --listen 10
```

---

## Gate 1 (G1): Round-Trip Kiosk Test (LuckFox ↔ LuckFox via Desktop)

**Prerequisites:** G0 passes.

**Goal:** Full P2P exchange: LuckFox sends, receives, measures throughput and PER.

**Steps:**
1. Two LuckFox units, each with MW300UH on same channel
2. Run `unow_diag --listen 1000 --node-id A` on one
3. Run `unow_diag --send-text "Test$(seq 1000)" --node-id B` on the other
4. Measure: packets received, latency, RSSI min/max/avg

---

## Environment Variables

```bash
export UNOW_LOG_LEVEL=debug       # or warn, info, trace
export UNOW_IFACE=mon0            # (not yet wired, for future use)
```

---

## Related Files

- [media/unow/include/unow/unow_wire.h](../include/unow/unow_wire.h) — Wire format constants
- [media/unow/scripts/unow-mon.sh](../scripts/unow-mon.sh) — Monitor mode setup
- [media/unow/TODO.md](../TODO.md) — Full development roadmap
