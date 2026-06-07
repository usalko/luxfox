# UNOW G0 Smoke Test — Quick Start

## Setup Phase

### LuckFox Terminal 1: Prepare monitor interface
```bash
# Identify your MW300UH adapter (or internal aic8800dc)
iw dev

# Assuming it's wlx088af1287d57, bring up mon0 on channel 6
./media/unow/scripts/unow-mon.sh wlx088af1287d57 mon0 6

# Verify it's up
iw dev mon0 info
```

### Desktop Terminal 1: Prepare monitor interface
```bash
# Kill network managers that might interfere
sudo airmon-ng check kill

# Identify your adapter (e.g., wlx00e04c123456)
iw dev

# Bring it down and create monitor
sudo ip link set wlx00e04c123456 down
sudo iw dev wlx00e04c123456 interface add mon0 type monitor
sudo ip link set mon0 up
sudo iw dev mon0 set channel 6

# Verify
iw dev mon0 info
```

---

## Test 1: LuckFox → Desktop (Desktop RX)

### Desktop Terminal 1: Listen for frames
```bash
# Method A: tcpdump (raw inspection)
sudo tcpdump -i mon0 -e 'wlan[24] == 127' -c 10

# Method B: unow_diag (if built for x86 host)
./media/unow/out/bin/unow_diag --iface mon0 --listen 10 --log-level debug
```

### LuckFox Terminal 2: Send test frames
```bash
cd /home/pascale/projects/63411/luxfox

# Send 10 test frames to broadcast
export UNOW_LOG_LEVEL=debug
./media/unow/out/bin/unow_diag \
  --iface mon0 \
  --node-id 1 \
  --send-text "Test_LuckFox_to_Desktop" \
  --listen 0
```

**Expected Desktop Output:**
- `tcpdump` shows 802.11 Management frames with action category 127
- Frame payload contains "Test_LuckFox_to_Desktop"
- RSSI visible in `unow_diag` output (if available)

---

## Test 2: Desktop → LuckFox (LuckFox RX)

### LuckFox Terminal 1: Listen for frames
```bash
export UNOW_LOG_LEVEL=info
./media/unow/out/bin/unow_diag \
  --iface mon0 \
  --node-id 2 \
  --listen 10
```

### Desktop Terminal 2: Send test frames
```bash
# First, get LuckFox's MAC from Test 1 output (or use broadcast)
LUCKFOX_MAC="aa:bb:cc:dd:ee:ff"  # Replace with actual MAC

# Option A: If unow_diag available on desktop
./media/unow/out/bin/unow_diag \
  --iface mon0 \
  --send-text "Test_Desktop_to_LuckFox" \
  --dst "$LUCKFOX_MAC" \
  --listen 0

# Option B: Craft frames with tcpdump/aireplay (advanced)
# Requires external frame injection utility
```

**Expected LuckFox Output:**
- 10 frames printed with src_mac, payload, RSSI
- No errors in pcap/inject paths
- Log shows "initialized iface=mon0"

---

## Test 3: Automated Smoke Test (Both Directions)

### LuckFox RX Mode
```bash
cd /home/pascale/projects/63411/luxfox
UNOW_DIAG="./media/unow/out/bin/unow_diag" \
  bash media/unow/tests/smoke_test.sh rx mon0 ff:ff:ff:ff:ff:ff 10
```

### Desktop TX Mode (in parallel terminal)
```bash
# Need to compile unow_diag for x86 host first (or use tcpdump + aireplay-ng)
UNOW_DIAG="./unow_diag"  # your local host binary
bash ./tests/smoke_test.sh tx mon0 ff:ff:ff:ff:ff:ff 10
```

---

## Verification Checklist

- [ ] Both `mon0` interfaces are up on channel 6
- [ ] `iw dev mon0 info` shows `monitor` type
- [ ] LuckFox `unow_diag` initializes without errors
- [ ] At least 90% of frames arrive at destination
- [ ] RSSI values are reasonable (< -50 dBm on 1m distance)
- [ ] No "pcap_inject failed" or "pcap_next_ex failed" errors

---

## Troubleshooting

### No frames appear in tcpdump
1. Verify both sides are on same channel: `iw dev mon0 info | grep channel`
2. Check if adapter supports injection: `modinfo rtl8xxxu | grep monitor`
3. Try different channel (1, 11 instead of 6)

### "pcap_activate failed" on LuckFox
1. Verify monitor interface is actually up: `ip link show mon0`
2. Check system logs: `dmesg | tail -20`
3. Ensure Wi-Fi module is loaded: `lsmod | grep wifi`

### High PER (packet loss > 10%)
1. Reduce distance between adapters (should work at 1-2m)
2. Check for interference: `iw dev mon0 survey dump`
3. Switch to quieter channel (1, 6, 11 in 2.4 GHz)
4. Verify TX power: `iw dev mon0 info | grep txpower`

---

## Next Steps After G0 Passes

Once G0 is confirmed (frames flowing both ways with < 5% PER):
1. **G1 throughput test:** Measure k/s and Mbit/s on 1 Mbps legacy rate
2. **G1 bidirectional:** LuckFox ↔ LuckFox via two adapters
3. **Integrate with ULAMA:** Use `unow` as `radio_espnow` transport for link_manager
4. **Mesh relay test:** Multi-hop G2/G3 gates
