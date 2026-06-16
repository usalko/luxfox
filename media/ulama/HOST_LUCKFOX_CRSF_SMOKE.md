# Host + LuckFox CRSF Smoke

This runbook is the shortest path to test the full chain:

`joystick (/dev/input/js0 on host) -> ULAMA -> UNOW -> ulamad on LuckFox -> /dev/ttyS3 -> Betaflight CRSF RX`

It is split into three stages so failures are easier to isolate.

## 0. Your interfaces

Host Wi-Fi base interface:

```text
wlx088af1422c79
MAC 08:8a:f1:42:2c:79
```

LuckFox Wi-Fi base interface:

```text
wlan0
MAC 08:8a:f1:28:7d:57
```

LuckFox UART:

```text
/dev/ttyS3
```

Host joystick:

```text
/dev/input/js0
```

The commands below use those values directly.

## 1. Prerequisites

On the host install the desktop dependencies once:

```bash
sudo apt install libpcap-dev iw tcpdump
```

On Betaflight make sure UART3 is configured as:

```text
Ports -> UART3 -> Serial RX = ON
Receiver -> Serial Receiver Provider = CRSF
```

Then save and reboot the flight controller.

## 2. Build on the host

From the repo root:

```bash
cd /home/pascale/projects/63411/luxfox/media/ulama
make host
make host-unow
make
```

Expected artifacts:

```text
build/host-tools/ulamad
build/host-tools/ulama_js_tx
build/host-unow-tools/ulama_js_tx
out/bin/ulamad
```

## 3. Copy to LuckFox

Automatic deploy:

```bash
cd /home/pascale/projects/63411/luxfox/media/ulama
chmod +x deploy-to-luckfox.sh
./deploy-to-luckfox.sh root@<LUCKFOX_IP>
```

This copies:

```text
/usr/bin/ulamad
/etc/ulama.conf
/etc/init.d/S99ulama
/usr/bin/scripts/unow-mon.sh
/usr/bin/scripts/unow-down.sh
```

Manual fallback:

```bash
scp out/bin/ulamad root@<LUCKFOX_IP>:/usr/bin/ulamad
scp defaults/ulama.conf root@<LUCKFOX_IP>:/etc/ulama.conf
scp scripts/S99ulama root@<LUCKFOX_IP>:/etc/init.d/S99ulama
scp ../unow/scripts/unow-mon.sh root@<LUCKFOX_IP>:/usr/bin/scripts/unow-mon.sh
scp ../unow/scripts/unow-down.sh root@<LUCKFOX_IP>:/usr/bin/scripts/unow-down.sh
ssh root@<LUCKFOX_IP> 'chmod +x /usr/bin/ulamad /etc/init.d/S99ulama /usr/bin/scripts/unow-mon.sh /usr/bin/scripts/unow-down.sh'
```

## 4. Put both adapters into monitor mode

### Host

```bash
sudo bash /home/pascale/projects/63411/luxfox/media/unow/scripts/unow-mon.sh wlx088af1422c79 mon0 6
iw dev mon0 info
ip link show mon0
```

### LuckFox

```bash
ssh root@<LUCKFOX_IP>
bash /usr/bin/scripts/unow-mon.sh wlan0 mon0 6
iw dev mon0 info
ip link show mon0
```

If either side does not show `type monitor`, stop there and send me the full output.

## 5. Stage A: radio-only receive on LuckFox

Before touching UART3, first prove that LuckFox receives CRSF bytes over UNOW.

### LuckFox terminal

```bash
rm -f /tmp/crsf.bin /tmp/ulamad-radio.log
UNOW_LOG_LEVEL=debug /usr/bin/ulamad \
  --transport unow \
  --iface mon0 \
  --node 1 \
  --output /tmp/crsf.bin \
  --count 20 \
  --verbose \
  2>&1 | tee /tmp/ulamad-radio.log
```

### Host terminal

```bash
cd /home/pascale/projects/63411/luxfox/media/ulama
UNOW_LOG_LEVEL=debug ./tests/run_unow_crsf_smoke.sh host-tx \
  --iface mon0 \
  --dst-mac 08:8a:f1:28:7d:57 \
  --count 20 \
  --rate-hz 20 \
  2>&1 | tee /tmp/ulama-host-fixed.log
```

### Verify on LuckFox

```bash
ls -l /tmp/crsf.bin
hexdump -C /tmp/crsf.bin | head
tail -n 50 /tmp/ulamad-radio.log
```

Success criteria:

- `ulamad` prints `seq=... rssi=...`
- `/tmp/crsf.bin` exists and is non-empty
- no `receive failed` / `drop: bad ulama frame` flood

## 6. Stage B: UART3 forward without joystick

Once Stage A works, switch output to real UART3.

### LuckFox terminal

```bash
UNOW_LOG_LEVEL=debug /usr/bin/ulamad \
  --transport unow \
  --iface mon0 \
  --node 1 \
  --uart /dev/ttyS3 \
  --baud 420000 \
  --verbose \
  2>&1 | tee /tmp/ulamad-uart.log
```

### Host terminal

```bash
cd /home/pascale/projects/63411/luxfox/media/ulama
UNOW_LOG_LEVEL=debug ./tests/run_unow_crsf_smoke.sh host-tx \
  --iface mon0 \
  --dst-mac 08:8a:f1:28:7d:57 \
  --count 200 \
  --rate-hz 50 \
  --arm \
  2>&1 | tee /tmp/ulama-host-uart.log
```

Now check the Betaflight Receiver tab. With the fixed pattern the channels should move deterministically.

### Keepalive behaviour

`ulamad` saves the last valid CRSF frame internally. Once the first frame is
accepted, it **retransmits it at 150 Hz** (~6.67 ms interval) even when no new
ULAMA frames arrive. This prevents the flight controller from entering failsafe
due to a gap in the CRSF stream (e.g. brief packet loss over UNOW).

To verify keepalive is running, stop the host-tx sender after a few frames and
watch the `[stats]` line in `/tmp/ulamad-uart.log`:

```text
[stats] accepted=42 keepalive=763 (uptime=10s)
```

`keepalive` should keep incrementing at ~150 per second even when `accepted`
stops growing. If `keepalive` stays at 0, no frame was received yet.

## 7. Stage C: real joystick on host

Only do this after Stage B is stable.

### LuckFox terminal

Keep the same `ulamad` process from Stage B running.

### Host terminal

```bash
cd /home/pascale/projects/63411/luxfox/media/ulama
UNOW_LOG_LEVEL=debug ./tests/run_unow_crsf_smoke.sh host-tx \
  --iface mon0 \
  --dst-mac 08:8a:f1:28:7d:57 \
  --joystick /dev/input/js0 \
  --arm \
  2>&1 | tee /tmp/ulama-host-joystick.log
```

Expected mapping in the current MVP:

- axis 0 -> CRSF channel 1
- axis 1 -> CRSF channel 2
- axis 2 -> CRSF channel 3 (throttle)
- axis 3 -> CRSF channel 4
- button 0 toggles arm on CRSF channel 5
- buttons 1.. map to auxiliary channels 6+

## 8. Logs to save if something fails

### Host

```bash
iw dev
iw dev mon0 info
ip link show mon0
tail -n 100 /tmp/ulama-host-fixed.log
tail -n 100 /tmp/ulama-host-uart.log
tail -n 100 /tmp/ulama-host-joystick.log
sudo tcpdump -i mon0 -e -s 256 -xx 'wlan type mgt subtype action' -c 20 | tee /tmp/ulama-host-tcpdump.log
```

### LuckFox

```bash
iw dev
iw dev mon0 info
ip link show mon0
tail -n 100 /tmp/ulamad-radio.log
tail -n 100 /tmp/ulamad-uart.log
ls -l /tmp/crsf.bin
hexdump -C /tmp/crsf.bin | head -n 20
dmesg | tail -n 100
```

## 9. Fast diagnosis map

- Host sender fails immediately with `ENOTSUP` or `failed to build UNOW-enabled host sender`:
  install `libpcap-dev` and rebuild `make host-unow`.
- `mon0` does not appear on either side:
  the driver/adapter monitor-mode setup is not ready yet.
- Stage A logs show no frames on LuckFox:
  this is still a UNOW/radio problem, not UART3/Betaflight.
- Stage A works but Stage B shows nothing in Betaflight:
  likely UART3 wiring or Betaflight port/protocol configuration.
- Stage B works with fixed pattern but Stage C does not:
  then the remaining problem is joystick event capture/mapping on the host.