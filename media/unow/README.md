# UNOW

UNOW is an ESP-NOW-like L2 transport over raw 802.11 monitor-mode injection for LuckFox Pico Ultra and desktop Linux peers. It is designed as a drop-in radio backend for ULAMA, so link management, retries, mesh routing, TTL, deduplication, and fragmentation stay in the upper ULAMA layers.

This README consolidates the old build, requirements, testing, and roadmap notes into one document.

## Current Status

- ARM and host build scripts are in place.
- `unow_diag` builds for LuckFox and is used for smoke tests and diagnostics.
- Monitor-mode helper scripts are present.
- G0 hardware validation is the current gate: bidirectional inject/capture on real adapters still needs to be proven on hardware.
- Desktop libtins peer, TX worker queue, and full callback wiring are still in progress.

## Repository Layout

```text
media/unow/
├── build.sh                 # Main build helper
├── build-artifacts.sh       # Low-level host/ARM build steps
├── deploy-to-luckfox.sh     # SSH deployment helper
├── README.md                # This document
├── RUNME_G0_TEST.sh         # Quick G0 test launcher
├── Makefile                 # Integration with media build
├── include/unow/            # Public headers
├── src/                     # Device-side implementation
├── scripts/                 # Monitor-mode helpers
├── tests/                   # Smoke tests and host-side helpers
├── out/                     # Local build outputs
└── host/                    # Desktop-side peer code (planned)
```

## Quick Start

### 1. Check Dependencies

```bash
cd media/unow
./build.sh --check-deps
```

Host packages typically required:

```bash
sudo apt install build-essential libpcap-dev iw tcpdump
```

Optional desktop test tools:

```bash
sudo apt install aircrack-ng libtins-dev
```

### 2. Build

Build for both host and LuckFox:

```bash
cd media/unow
./build.sh -t all
```

Common variants:

```bash
./build.sh -t host
./build.sh -t arm
./build.sh -c -t all
./build.sh -t all -v
```

### 3. Deploy to LuckFox

Automatic deploy over SSH:

```bash
cd media/unow
./deploy-to-luckfox.sh 192.168.1.100
```

This copies the ARM binary, libraries, helper scripts, and init service files to the device.

Manual copy fallback:

```bash
scp ../../output/out/media_out/bin/arm/unow_diag root@192.168.1.100:/usr/bin/
scp ../../output/out/media_out/lib/arm/libunow.so root@192.168.1.100:/usr/lib/
scp ../../output/out/media_out/lib/arm/libunow.a root@192.168.1.100:/usr/lib/
```

### 4. Run a Basic G0 Smoke Test

On LuckFox:

```bash
ssh root@192.168.1.100
iw dev
bash /usr/bin/scripts/unow-mon.sh <wifi-adapter> mon0 6
/usr/bin/unow_diag --iface mon0 --node-id 1 --listen 10
```

On desktop:

```bash
cd media/unow
bash RUNME_G0_TEST.sh desktop tx
```

## Build Outputs

After a successful build, artifacts are copied both to the local `out/` tree and to the project distribution output.

```text
media/unow/
├── out/
│   ├── arm/
│   │   ├── bin/unow_diag
│   │   ├── lib/libunow.so
│   │   ├── lib/libunow.a
│   │   └── include/unow/
│   └── host/
│       └── ...

output/out/media_out/
├── bin/
│   ├── arm/unow_diag
│   └── x86_64/...
├── lib/
│   ├── arm/libunow.so
│   ├── arm/libunow.a
│   └── x86_64/...
└── include/unow/
```

Use these paths when integrating with the firmware image or when copying binaries by hand.

## Toolchain and Runtime Requirements

### Host Build Requirements

- `gcc`, `make`, `build-essential`
- `libpcap-dev`
- `iw`, `tcpdump`
- `aircrack-ng` for optional helper workflows
- `libtins-dev` for the planned desktop reference peer

### LuckFox / ARM Requirements

- Working cross-compiler from `media/Makefile.param`
- Driver support for monitor mode and injection
- `libpcap` available in the target filesystem
- A Wi-Fi adapter that can do monitor-mode injection reliably

Check the media toolchain setup from the project root:

```bash
source media/Makefile.param
echo "$RK_MEDIA_CROSS"
```

If the cross-compiler is not in `PATH`, add it before building for ARM.

## Architecture Constraints and Decisions

UNOW is intentionally narrow in scope. It is only the one-hop radio transport.

| Topic | Decision |
|---|---|
| Transport level | UNOW is a connectionless L2 datagram pipe over raw 802.11 action frames. |
| Payload format | Carry native ULAMA L1 frames as the action-frame payload instead of inventing a second custom packet format. |
| Reliability | No retransmit logic in UNOW. Reliability stays in ULAMA L2 ARQ. |
| Mesh logic | No relay, TTL, or dedup in UNOW. Those remain in ULAMA mesh/router layers. |
| Source MAC | Read the real interface MAC and write it into `addr2`; do not use interface names as MAC addresses. |
| RSSI | Parse RSSI from RadioTap and pass it up through the receive callback. |
| Filtering | Use a BPF prefilter for our OUI/subtype to avoid flooding userspace with unrelated frames. |
| Device implementation | LuckFox side stays in C with `libpcap` or `AF_PACKET`; libtins is reserved for desktop. |
| Self-reception | Drop frames whose source MAC matches the local interface MAC. |
| Injection rate | Set fixed legacy rate in RadioTap when the driver supports it. |

## Wire Format

UNOW uses a vendor-specific 802.11 action frame as the carrier.

```text
RadioTap (TX: RATE | TX_FLAGS, RX: read dbm_signal)
└─ Dot11 Management/Action
   addr1 = destination MAC or ff:ff:ff:ff:ff:ff
   addr2 = self MAC
   addr3 = UNOW BSSID
   Action body:
     category       = 127 (vendor-specific)
     OUI[3]         = UNOW_OUI
     vendor_subtype = UNOW_DATA
     payload[]      = ULAMA L1 frame
```

Target payload sizing must remain compatible with the ESP-NOW-like limits expected by the ULAMA radio shim.

## API Compatibility Goal

UNOW mirrors the `radio_espnow` API so that the upper ULAMA layers do not need large changes.

```c
int  unow_init(uint8_t node_id, const char *iface);
int  unow_send(const uint8_t dst_mac[6], const uint8_t *payload, size_t len);
int  unow_add_peer(const uint8_t mac[6]);
bool unow_peer_known(void);
bool unow_get_peer_mac(uint8_t out[6]);
void unow_set_rx_callback(radio_espnow_rx_cb_t cb, void *ctx);
void unow_set_control_callback(radio_espnow_control_cb_t cb, void *ctx);
void unow_get_stats(radio_espnow_stats_t *out);
```

## Testing Gates

| Gate | Purpose | Exit Criteria |
|---|---|---|
| G0 | Raw inject/capture in both directions | Our frames are visible in `tcpdump`; PER is measured on hardware |
| G1 | `unow_send` to RX callback on LuckFox peers | 1000 frames, PER < 2%, RSSI present |
| G1b | LuckFox C implementation to desktop libtins peer | Bidirectional exchange works |
| G1c | BPF filter on a busy channel | Userspace load stays acceptable; unrelated traffic is filtered out |
| G1d | Throughput benchmark | Ceiling is measured at a fixed legacy rate |

### G0 Detailed Procedure

#### Identify Adapters

On LuckFox:

```bash
iw dev
```

On desktop:

```bash
iw dev
lsusb | grep -i realtek
```

#### Put Both Sides on the Same Channel

On LuckFox:

```bash
IFACE=wlan1
unow-mon.sh "$IFACE" mon0 6
iw dev mon0 info
ip link show mon0
```

On desktop:

```bash
IFACE=wlx00e04c123456
sudo airmon-ng check kill
sudo ip link set "$IFACE" down
sudo iw dev "$IFACE" interface add mon0 type monitor
sudo ip link set mon0 up
sudo iw dev mon0 set channel 6
iw dev mon0 info
```

#### LuckFox to Desktop

Desktop listener:

```bash
sudo tcpdump -i mon0 -e -c 5
```

LuckFox sender:

```bash
./media/unow/out/bin/unow_diag \
  --iface mon0 \
  --node-id 1 \
  --log-level debug \
  --send-text "HelloDesktop" \
  --listen 0
```

#### Desktop to LuckFox

LuckFox listener:

```bash
./media/unow/out/bin/unow_diag \
  --iface mon0 \
  --node-id 2 \
  --log-level info \
  --listen 5
```

Desktop side should then inject matching UNOW frames and confirm they appear in the device logs.

### Success Criteria

- Bidirectional visibility of our action frames
- At least 90% of a 10-packet test reaches the peer with valid RSSI
- No fatal `pcap_inject` or RX parser failures
- Baseline single-hop latency below 100 ms for smoke tests

### Log Levels

```bash
UNOW_LOG_LEVEL=trace ./media/unow/out/bin/unow_diag --listen 10
```

Supported levels:

- `error`
- `warn`
- `info`
- `debug`
- `trace`

## Build and Deploy Scripts

| Script | Purpose |
|---|---|
| `build.sh` | Main entry point for dependency checks, host build, ARM build, cleaning, and summary output |
| `build-artifacts.sh` | Lower-level compilation flow for host and ARM binaries/libraries |
| `deploy-to-luckfox.sh` | SSH deployment of ARM binary, libraries, scripts, and service assets |
| `RUNME_G0_TEST.sh` | Convenience wrapper for quick G0 experiments |
| `scripts/unow-mon.sh` | Puts an interface into monitor mode on a fixed channel |
| `scripts/unow-down.sh` | Leaves monitor mode and restores interface state |

## Troubleshooting

### `libpcap-dev not found`

```bash
sudo apt install libpcap-dev
./build.sh -c -t all
```

### `Makefile.param not found`

- Run the build from `media/unow`
- Make sure `media/Makefile.param` exists

### `RK_MEDIA_CROSS not set`

```bash
source ../Makefile.param
echo "$RK_MEDIA_CROSS"
```

If needed:

```bash
export RK_MEDIA_CROSS=arm-buildroot-linux-uclibcgnueabihf
./build.sh -t arm
```

### `Cross-compiler not found`

```bash
export PATH="/path/to/toolchain/bin:$PATH"
./build.sh -t arm
```

### SSH Deploy Fails

```bash
ssh-copy-id root@192.168.1.100
./deploy-to-luckfox.sh 192.168.1.100
```

Or fall back to manual `scp`.

### `tcpdump` Sees Nothing

- Verify both peers are on the same channel
- Verify monitor mode really came up on both sides
- Check region restrictions with `iw reg get`
- Try a different adapter if the driver does not support injection reliably

### High Packet Loss

- Move to a quieter 2.4 GHz channel
- Reduce distance between adapters for initial tests
- Verify driver TX power and antenna setup
- Test with a fixed legacy rate that the driver actually honors

## Roadmap and Open Work

### G0 Hardware Validation

- [x] Monitor-mode helper scripts exist
- [x] `unow_diag` builds and links for LuckFox
- [ ] Run bidirectional smoke tests on real LuckFox and desktop hardware
- [ ] Measure PER, RSSI, and latency on 100+ packets
- [ ] Record the working channel and fixed rate

### API and Core Runtime

- [x] `radio_espnow` compatibility header and aliasing are in place
- [~] Preserve ESP-NOW semantics for peer discovery and broadcast handling
- [~] Fill `radio_espnow_stats_t` completely

### Device Implementation

- [~] Interface bring-up logic needs to be fully idempotent in code
- [x] `pcap` open path exists
- [x] TX frame builder exists
- [ ] Add TX worker queue with pacing and eviction behavior
- [x] BPF prefilter exists
- [~] RX worker parses RadioTap, OUI, and self-reception
- [ ] Wire RX callback delivery to upper layers completely

### Desktop Peer

- [ ] Implement desktop reference peer with libtins
- [ ] Verify wire compatibility with the LuckFox C implementation
- [ ] Keep desktop peer free of mesh-layer logic

### Build Integration

- [x] Media Makefile integration exists
- [ ] Make `libpcap` an explicit Buildroot dependency instead of relying on `tcpdump`
- [ ] Add host-side unit tests for wire and RadioTap parsing

### Scripts and Diagnostics

- [x] `unow-mon.sh` generalized monitor-mode setup
- [x] `unow-down.sh` generalized teardown path
- [x] `unow-signal-diag.sh` exists
- [x] Init service for signal diagnostics exists
- [ ] Validate stable peer RSSI and custom BSSID handling on real hardware

## Risks

- `rtl8xxxu` injection may still fail on real hardware; G0 is the hard gate.
- Some drivers ignore RadioTap rate fields.
- There is no native 802.11 MAC ACK path in this monitor/inject design.
- A noisy channel can dominate observed PER.
- Self-reception and double-reception must stay filtered both by MAC and ULAMA sequence deduplication.

Plan B if MW300UH injection is unstable:

- Try the internal `aic8800dc` path if it supports the same workflow
- Try a better-supported adapter such as `mt76` or `ath9k_htc`

## Target Structure

```text
media/unow/
  Makefile
  include/unow/
    radio_unow.h
    unow_wire.h
  src/
    unow_iface.c
    unow_tx.c
    unow_rx.c
    unow_radiotap.c
    unow.c
  scripts/
    unow-mon.sh
    unow-down.sh
  host/unow-peer/
  tests/
```

## Related Work

- ULAMA port roadmap: [`media/ulama/TODO.md`](../ulama/TODO.md)
- UNOW monitor-mode helpers: [`media/unow/scripts/unow-mon.sh`](scripts/unow-mon.sh)
- UNOW wire constants: [`media/unow/include/unow/unow_wire.h`](include/unow/unow_wire.h)
