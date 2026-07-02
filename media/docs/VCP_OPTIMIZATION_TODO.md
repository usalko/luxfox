# VCP Video Delivery Optimization TODO

## Problem Statement

NAL delivery from vcpd (LuckFox device) to ulama-gw (host) suffers from
disproportionately high frame drop rate despite excellent radio conditions.

### Observed Metrics (from ulama-gw stats, 5-second intervals)

| Metric | Typical Range | Interpretation |
|--------|---------------|----------------|
| RSSI | -40..-42 dBm | Excellent signal, not a factor |
| video_rx | ~1400 pkt/5s | ~280 pps average |
| LTS unique | ~1350-1430 | Successfully received unique packets |
| LTS dup | 4-27 | Duplicate packets (retx or multipath) |
| LTS lost | 0-58 | **0-4% raw packet loss** |
| NAL ok | 205-245 | Complete NALs delivered |
| NAL drop | 18-38 | **7-15% NAL drop rate** |
| NACK sent | 122-184 | NACK requests generated |
| video_out | 212-335 Kbit/s | Delivered video bitrate |

**Core problem**: 2-4% packet loss is amplified to 7-15% NAL drop rate because
a single missing packet in a multi-packet NAL forces the entire NAL to be
discarded.

---

## Root Cause Analysis

### Cause 1: No inter-packet pacing (CRITICAL)

**Location**: `vcpd/tools/vcpd.c:572-576`

```c
for (size_t i = 0; i < npkts; i++) {
    lts_retx_buf_store(ctx->retx_buf, &lts_pkts[i]);
    send_ulama_video(&ctx, lts_pkts[i].data, lts_pkts[i].len);
}
```

All LTS packets of a NAL are injected into the radio back-to-back with zero
delay. A typical P/B frame at 512 kbps with LTS_ENC_MAX_PAYLOAD=216 bytes
produces 5-20 LTS packets per NAL. These are burst-sent via pcap_inject()
in under 1 ms.

**Why this matters**: Monitor-mode Wi-Fi (unow) has no hardware TX queue
management. Unlike real ESP-NOW on ESP32 which has a 32-frame hardware TX queue
with MAC-level pacing and backpressure, pcap_inject() submits frames directly
to the kernel's netdev queue. A burst of 15+ frames can overflow the driver's
TX ring buffer or cause the radio to drop frames during channel contention.

**Reference**: In the original ULAMA project
(`../../ULAMA/firmware/components/link/link_manager.c`), adaptive pacing
was implemented:
- Queue high threshold: 8 packets -> increase pace delay
- Queue low threshold: 2 packets -> decrease pace delay
- Max pace delay: 3 ms per frame
- This mechanism prevented burst losses entirely

**Expected impact**: Resolving this alone could eliminate 60-80% of current NAL
drops, as burst losses correlate with multi-packet NALs (P/B frames).

### Cause 2: Video sent unreliable — no MAC-level retry

**Location**: `vcpd/tools/vcpd.c:156-180` (`send_ulama_video`)

Video packets use `ulama_transport_tx_send()` which maps to
`radio_espnow_send()` — a single `pcap_inject()` with no ACK or retry.

`radio_espnow_send_reliable()` exists in `unow/src/unow.c:177-276` and
provides:
- Sequence-numbered frames (UNOW_VENDOR_SUBTYPE_DATA_SEQ)
- Up to 4 attempts (1 + UNOW_ACK_MAX_RETRY=3)
- ACK wait timeout: UNOW_ACK_TIMEOUT_US=3000 us (3 ms) per attempt
- Fallback to unreliable if all attempts fail (returns ESP_OK regardless)

**Why video doesn't use it**: Likely a latency concern — reliable send adds up
to 12 ms per packet in worst case. At 280 pps that would consume the entire
time budget. But selective use (e.g. only for LAST_OF_FRAME packets) could be
viable.

**Reference**: Real ESP-NOW on ESP32 provides transparent MAC-level ACK/retry
without application-visible latency. The ULAMA firmware didn't need explicit
reliable send because the hardware handled it.

### Cause 3: NACK mechanism is structurally too slow

**Locations**:
- NACK generation: `ulama-gw/tools/ulama_gw.c:379-430`
- Gap detection: `ulama-gw/src/lts_decoder.c:157-171`
- NAL drop on gap: `ulama-gw/tools/ulama_gw.c:328-330`
- NACK handling on vcpd: `vcpd/tools/vcpd.c:182-198`

The NACK→retransmit→reassembly pipeline has a timing problem:

1. **Gap detected** in LTS decoder reorder buffer
2. **NACK sent** (rate-limited: NACK_MIN_INTERVAL_MS=20 ms)
3. NACK travels over radio to vcpd (~1-3 ms)
4. vcpd looks up packet in retx buffer (LTS_RETX_SLOTS=512) and resends (~1 ms)
5. Retransmitted packet travels back over radio (~1-3 ms)
6. **Total RTT**: ~25-30 ms minimum

But the NAL assembler (`emit_lts_to_cascade`, line 328) drops the NAL
**immediately** when a gap is detected in the emitted packet stream:

```c
if (a->active && pkt->pkt_seq != a->expect_seq) {
    drop_nal(ctx, a->expect_seq, pkt->pkt_seq);
}
```

The LTS decoder emit deadline is LTS_EMIT_DEADLINE_MS=200 ms, but the NAL
assembler doesn't wait — it sees the gap in the already-emitted stream and
drops. By the time the retransmitted packet arrives and gets emitted, the NAL
is already gone.

**Net effect**: NACKs serve primarily to recover packets for the reorder buffer,
but if the NAL assembler has already moved past the gap, the recovered packet
is useless. The 120-184 NACKs per 5-second interval mostly go to waste.

### Cause 4: No redundancy for P/B frames

**Location**: `vcpd/tools/vcpd.c:541-564`

VPS/SPS/PPS parameter NALs are duplicated PARAM_NAL_DUP_COUNT=3 times before
each IDR frame, reducing their loss probability from 50% to 12.5% at PER=50%.

Regular NALs (I/P/B slices) have zero redundancy. The only recovery mechanism
is the NACK-based retransmission, which as shown above, is structurally too
slow to save most multi-packet NALs.

### Cause 5: NAL size amplifies loss probability

A NAL spanning N packets survives only if ALL N packets arrive. With
independent packet loss rate p:

| NAL packets | Loss probability at p=2% | Loss probability at p=4% |
|-------------|--------------------------|--------------------------|
| 1 (params) | 2.0% | 4.0% |
| 5 (small P) | 9.6% | 18.5% |
| 10 (medium P)| 18.3% | 33.5% |
| 20 (large B) | 33.2% | 55.8% |

This explains why 2-4% packet loss causes 7-15% NAL drop: the average NAL
is 5-7 packets, and larger P/B frames are hit hardest.

---

## Execution Plan

### Phase 1: Pacing + Diagnostics (Paths A + E)

**Goal**: Add inter-packet pacing to vcpd and simultaneously instrument both
sides to measure the effect and characterize remaining losses.

#### Task 1.1: Add diagnostic logging to ulama-gw ✅ DONE (commit a2a09ea)

**File**: `ulama-gw/tools/ulama_gw.c`

Add counters and periodic logging for:

- **Dropped NAL size distribution**: record packet count of each dropped NAL
  - Add fields to `gw_stats_t`:
    ```c
    uint32_t nal_drop_1pkt;    // single-packet NAL drops
    uint32_t nal_drop_2_5pkt;  // 2-5 packet NAL drops
    uint32_t nal_drop_6_15pkt; // 6-15 packet NAL drops
    uint32_t nal_drop_16plus;  // 16+ packet NAL drops
    ```
  - In `drop_nal()`: calculate `pkt_count = got - a->first_seq` (with 16-bit
    wrap handling) and increment the appropriate bucket

- **Gap pattern (burst vs random)**: log sequences of consecutive lost packets
  - In `drop_nal()`: log `expected` and `got` values to stderr when verbose
  - Add field `uint32_t burst_gaps` (gaps of 2+ consecutive lost packets)
    vs `uint32_t single_gaps` (isolated single packet losses)

- **NACK effectiveness**: track how many retransmitted packets arrive in time
  - Add field `uint32_t retx_arrived` (packets with LTS_FLAG_RETX that were
    inserted into the reorder buffer successfully, i.e. lts_decoder_insert
    returned false)
  - Compare `retx_arrived` to `nack_sent` in stats output

- **Extended stats line** (append to existing `[stats]` format):
  ```
  | drop_by_size 1/%u 2-5/%u 6-15/%u 16+/%u | gaps burst=%u single=%u | retx_ok=%u
  ```

#### Task 1.2: Add diagnostic logging to vcpd ✅ DONE (commit a2a09ea)

**File**: `vcpd/tools/vcpd.c`

- **NACK response stats**: already has `nack_retx_count`; add:
  - `uint32_t nack_retx_miss` — NACKed seq not found in retx buffer
    (already logged in verbose mode, just count it)
  - Log in shutdown message and periodic stderr output

- **Packets per NAL histogram** (verbose mode): already logged as
  `"NAL type=%u %zu bytes -> %zu LTS pkts"` — sufficient for analysis

#### Task 1.3: Implement inter-packet pacing in vcpd ✅ DONE (commit a2a09ea)

**File**: `vcpd/tools/vcpd.c`, around lines 566-576

Add configurable inter-packet delay:

- New CLI option: `--pace-us N` (default 300, range 0-5000)
  - Add to option parsing (line ~283) and context struct
  - 0 = no pacing (current behavior, for A/B testing)

- **Primary video send loop** (line 572-576):
  ```c
  for (size_t i = 0; i < npkts; i++) {
      lts_retx_buf_store(ctx->retx_buf, &lts_pkts[i]);
      if (lts_pkts[i].len <= ULAMA_FRAME_MAX_PAYLOAD)
          send_ulama_video(&ctx, lts_pkts[i].data, lts_pkts[i].len);
      if (ctx->pace_us > 0 && i + 1 < npkts)
          usleep(ctx->pace_us);
  }
  ```

- **Parameter NAL send loop** (line 547-559): also add pacing between
  duplicate sends of VPS/SPS/PPS (same `usleep(ctx->pace_us)`)

- Latency budget check: at 280 pps with average 7 pkts/NAL and 300 us pacing:
  - Per NAL: 6 gaps * 300 us = 1.8 ms
  - Per second: 40 NALs * 1.8 ms = 72 ms total pacing overhead
  - Well within budget (25 ms per NAL at 40 NALs/s = 25 ms available)

#### Task 1.4: Test matrix ✅ DONE

Run the following test configurations and collect stats for 60 seconds each:

| Test | --pace-us | Description |
|------|-----------|-------------|
| Baseline | 0 | Current behavior, with new diagnostics |
| Pace 100 | 100 | Light pacing |
| Pace 300 | 300 | Moderate pacing (recommended start) |
| Pace 500 | 500 | Conservative pacing |
| Pace 1000 | 1000 | Heavy pacing |

**Collect and compare**:
- NAL drop rate (% of total NALs)
- LTS lost count
- Gap pattern (burst vs single)
- NACK count
- video_out bitrate (must not decrease significantly)
- Subjective video quality (visual artifacts)

#### Task 1.5: Analyze results and tune ✅ DONE

Results: pacing has minimal effect (~2.7 pp improvement at best).
Losses are radio-level (50/50 burst/single), NOT TX queue overflow.
NACK retransmissions arrive (~275/interval) but too late for NAL assembler.
Decision: keep pace=300 as default, proceed to Phase 2 (reliable send).

| pace-us | NAL drop% | LTS lost | burst | single | retx_ok |
|---------|-----------|----------|-------|--------|---------|
| 0       | 16.5%     | 29.8     | 24.9  | 19.3   | 272     |
| 100     | 16.6%     | 35.7     | 25.5  | 19.4   | 268     |
| 300     | 13.8%     | 25.0     | 21.8  | 16.6   | 275     |
| 500     | 14.7%     | 28.1     | 21.2  | 20.3   | 281     |
| 1000    | 16.4%     | 41.8     | 23.6  | 23.8   | 278     |

---

### Phase 2: Reliable send for video (Path B)

**Goal**: Add MAC-level reliability to video packets using unow's existing
`radio_espnow_send_reliable()`, either for all video or selectively.

**Prerequisite**: Phase 1 results showing that pacing alone doesn't eliminate
losses (random single-packet losses remain).

#### Task 2.1: Add transport-level reliable send option ✅ DONE (commit 5938534)

**File**: `ulama/include/ulama/transport.h` and implementation

- Add a flag or new function: `ulama_transport_tx_send_reliable()`
- This must route through `radio_espnow_send_reliable()` instead of
  `radio_espnow_send()`

#### Task 2.2: Selective reliable send in vcpd ✅ DONE (commit 5938534)

**File**: `vcpd/tools/vcpd.c`

Three strategies to test (new CLI option `--reliable`):

- **Mode 0** (default): all unreliable (current behavior)
- **Mode 1** (last-only): only LAST_OF_FRAME packet sent reliable
  - Rationale: if the last packet of a NAL arrives, the preceding packets
    likely also arrived (they were sent earlier in the burst)
  - Cost: 1 reliable send per NAL = max 12 ms extra per NAL
- **Mode 2** (all-reliable): every video packet sent reliable
  - Cost: up to 12 ms * N packets per NAL — may exceed latency budget
  - Only viable at low packet counts or with reduced ACK timeout
- **Mode 3** (adaptive): reliable for NALs > N packets, unreliable for small
  - Threshold: `--reliable-threshold N` (default 3)

#### Task 2.3: Tune UNOW ACK parameters ✅ DONE (commit dbff73e)

**File**: `unow/src/unow_internal.h`, `unow/src/unow.c`, `unow/include/unow/radio_unow.h`

Added `unow_set_ack_params()` API. Runtime-configurable via vcpd CLI:
- `--ack-timeout 1500` (default, was 3000)
- `--ack-retry 1` (default, was 3)
Worst-case per-packet time: 3 ms (was 12 ms).

Phase 2 test results (before ACK tuning):
| Mode | NAL drop% | video_rx/5s | video_out |
|------|-----------|-------------|-----------|
| reliable=0 | 13.6% | 1379 | 266 Kb/s |
| reliable=1 | 25.0% | 1179 | 169 Kb/s |
| reliable=2 | 3.2% | 458 | 89 Kb/s |
| reliable=3 | 4.2% | 465 | 87 Kb/s |

Conclusion: reliable=2/3 cuts drops to 3-4% but kills throughput 3x.
ACK tuning (1500us/1 retry) should recover throughput while keeping low drop.

#### Task 2.4: Test and measure ✅ DONE

ACK tuning results (all with --reliable 2):

| ACK config | NAL drop% | video_rx | vout Kb/s |
|------------|-----------|----------|-----------|
| 3000us/3retry (old) | 3.2% | 458 | 89 |
| 1500us/1retry | 26.9% | 954 | 146 |
| 1000us/1retry | 41.2% | 970 | 105 |
| **2000us/2retry** | **13.3%** | **687** | **137** |

Winner: ack=2000/2 — drop rate matches baseline, throughput 50% of unreliable.
New defaults: --reliable 2 --ack-timeout 2000 --ack-retry 2

---

### Phase 3: Forward Error Correction (Path D)

**Goal**: Add XOR-based FEC to recover single packet losses without
retransmission, eliminating NACK round-trip latency entirely.

**Prerequisite**: Phase 1+2 results showing residual losses that can't be
solved by pacing + reliable send alone, or if reliable send adds too much
latency.

#### Task 3.1: Design FEC scheme ✅ DONE

**Parameters**:
- Group size K (data packets per FEC group): 4-8
- Redundancy R (FEC packets per group): 1-2
- Encoding: simple XOR of K data packets produces 1 FEC packet
  - Can recover any single loss in the group
  - For R=2, use Reed-Solomon or interleaved XOR for 2-loss recovery

**Wire format**: New LTS flag `LTS_FLAG_FEC` (0x08)
- FEC packets carry XOR of group payload + bitmask identifying group members
- FEC header: group_start_seq (2 bytes) + group_size (1 byte) = 3 bytes overhead

**Bandwidth overhead**: K=4, R=1 -> 25% overhead; K=8, R=1 -> 12.5% overhead

#### Task 3.2: Implement FEC encoder in vcpd ✅ DONE (commit 8b21b5d)

**New file**: `vcpd/src/lts_fec_enc.c`

```
lts_fec_encoder_t:
  - Accumulates K data packets
  - After K-th packet, computes XOR parity packet
  - Emits parity packet with LTS_FLAG_FEC flag
  - Resets accumulator
```

**Integration** in `vcpd/tools/vcpd.c`:
- After each `lts_retx_buf_store()` + `send_ulama_video()`, feed packet to
  FEC encoder
- When FEC encoder emits parity packet, send it via `send_ulama_video()`
- New CLI option: `--fec K` (0=disabled, 4-8=group size)

#### Task 3.3: Implement FEC decoder in ulama-gw ✅ DONE (commit 8b21b5d)

**New file**: `ulama-gw/src/lts_fec_dec.c`

```
lts_fec_decoder_t:
  - Collects data packets by group (identified by seq range)
  - When parity packet arrives (LTS_FLAG_FEC), stores it
  - If exactly 1 data packet missing from group and parity available:
    recovers missing packet via XOR
  - Inserts recovered packet into LTS decoder as if received normally
```

**Integration** in `ulama-gw/tools/ulama_gw.c`:
- After `lts_decoder_insert()`, also feed packet to FEC decoder
- After gap detection, check FEC decoder for recoverable gaps before
  generating NACK
- Add stats: `fec_recovered`, `fec_unrecoverable`

#### Task 3.4: Test FEC effectiveness ✅ DONE

FEC decoder fixed (ring buffer + XOR payload-only). Results:

| Config | NAL drop% | vout Kb/s | FEC +recovered -unrec |
|--------|-----------|-----------|----------------------|
| reliable=0 fec=4 | 47.6% | 76 | +81 -268 |
| reliable=2 fec=4 | 14.8% | 97 | +28 -37 |
| reliable=2 fec=0 | **11.5%** | **137** | — |

Conclusion: FEC works (81 recovered/interval) but 25% bandwidth overhead
is counterproductive on this channel. FEC disabled by default (--fec 0).
Best config remains: --reliable 2 --ack-timeout 2000 --ack-retry 2.

---

### Phase 4: NAL assembler gap tolerance (Path C)

**Goal**: Instead of dropping entire NAL on any packet gap, tolerate small
gaps (1-2 missing packets) and deliver partial NAL to decoder.
H.265 can decode partial slices with minor artifacts — much better
than losing the entire frame.

#### Task 4.1: Add gap tolerance to NAL assembler ✅ DONE (commit 40ad659)

**Test results (Task 4.4):**

| Config | NAL drop% | vout Kb/s | gap_skip |
|--------|-----------|-----------|----------|
| gap_tol=0 reliable=2 | 26.4% | 87 | 0 |
| **gap_tol=2 reliable=2** | **8.0%** | **116** | 13.4 |

Gap tolerance reduces NAL drop by 3.3x. Best overall result: 8.0% drop.

**File**: `ulama-gw/tools/ulama_gw.c`, function `emit_lts_to_cascade()`

Current behavior (line 328-330):
```c
if (a->active && pkt->pkt_seq != a->expect_seq) {
    drop_nal(ctx, a->expect_seq, pkt->pkt_seq);
}
```

New behavior:
```c
if (a->active && pkt->pkt_seq != a->expect_seq) {
    if (!a->gap_detected) {
        a->gap_detected = true;
        a->gap_ts = now_ms();
        // Don't drop yet — stash this packet, wait for retx
        // Buffer out-of-order packets in a small side buffer
    } else if (now_ms() - a->gap_ts > NAL_GAP_GRACE_MS) {
        drop_nal(ctx, a->expect_seq, pkt->pkt_seq);
    }
}
```

**New fields in `nal_assembler_t`**:
```c
bool gap_detected;
uint64_t gap_ts;
uint16_t gap_seq;  // the missing sequence number
uint8_t pending_buf[NAL_ASSEMBLE_MAX];
size_t pending_len;
uint16_t pending_seq;
```

**New constant**: `NAL_GAP_GRACE_MS` (default 50 ms, CLI: `--nal-grace-ms`)

#### Task 4.2: Buffer out-of-order packets during grace period

When gap is detected but grace period hasn't expired:
- Continue receiving packets into a "pending" area
- When the missing packet arrives (via retx), insert it in correct position
  and resume normal assembly
- If grace period expires without the missing packet: drop NAL + pending
- Limit: only buffer 1 gap per NAL (if second gap appears, drop immediately)

**Complexity warning**: This adds significant state to the NAL assembler.
Must handle:
- Multiple gaps (drop on second gap)
- Sequence wraparound (16-bit seq)
- Memory limits (pending buffer size)
- Grace period vs overall latency budget

#### Task 4.3: Coordinate with NACK timing

For grace period to work, NACKs must be sent AND retransmissions must arrive
within NAL_GAP_GRACE_MS:

- Current NACK_MIN_INTERVAL_MS=20 ms — must be less than grace period
- NACK RTT ~25-30 ms — grace of 50 ms gives ~20 ms margin
- Consider reducing NACK_MIN_INTERVAL_MS to 10 ms

#### Task 4.4: Test grace period effectiveness

- Test with grace=0 (current), 30, 50, 80 ms
- Measure: NAL drop rate, end-to-end latency increase, NALs saved by grace
- Add stat: `nal_saved_by_grace` — NALs that had gap but were completed
  after retransmit arrived during grace period

---

### Phase 5: Reliable NACKs + LTS MTU experiment

**Reliable NACK send** (commit 1a119af7c): NACKs sent via reliable transport
with MAC-level ACK+retry. This was the breakthrough — at mtu=216, NAL drop
went from 8-26% to **0.1%** because every NACK is guaranteed to arrive.

**LTS MTU experiment** (commit 527b26ff4): tested mtu=216/500/1000/1500.

| MTU | NAL drop% | NAL ok/5s | video_rx | lost | vout Kb/s |
|-----|-----------|-----------|----------|------|-----------|
| **216** | **0.1%** | **198** | 482 | **0** | **100** |
| 500 | 0.0% | 26 | 103 | 391 | 1 |
| 1000 | 0.0% | 5 | 78 | 417 | 0 |
| 1500 | 0.0% | 120 | 177 | 218 | 6 |

Conclusion: mtu=216 optimal. Small packets + reliable NACKs + reliable
video send = near-perfect delivery. Larger frames suffer higher PER
in monitor mode. Default: --lts-mtu 216.

---

### Phase 6: Channel Bandwidth Benchmark (commit 660111b83)

Added `--benchmark KBPS` to vcpd and `--nack-disable` to ulama-gw
for measuring raw channel capacity.

**Benchmark results (2026-06-27):**

| Config | TX pps | RX pps | Loss% | RX Kbit/s |
|--------|--------|--------|-------|-----------|
| unreliable mtu=216 @1000 | 578 | **95** | 83.6% | **165** |
| reliable mtu=216 @1000 | 145 | **69** | 52.4% | **122** |
| unreliable mtu=500 @1000 | 250 | **61** | 75.6% | **248** |
| unreliable mtu=216 @5000 | 1787 | **94** | 94.7% | **166** |

**Critical discovery: channel has a fixed ~95 pps ceiling at mtu=216.**
Sending more than ~95 pps is wasted — the radio/driver drops the excess.
This is a hardware limitation of the 8192eu WiFi adapter in monitor mode.

**mtu=500 delivers 50% more throughput** (248 vs 165 Kbit/s) because each
received packet carries more payload. Fewer packets but more data per packet.

**Reliable send overhead**: ACK blocking limits TX to 145 pps (vs 578
unreliable), but only 69 pps arrive — worse throughput than unreliable.
However, reliable + NACK recovery achieves 0% drop at the cost of throughput.

**Optimal bitrate for video**: ~200-250 Kbit/s at mtu=500, or ~150 Kbit/s
at mtu=216. Encoder `--bitrate` must not exceed channel capacity.

---

### Phase 6.5: MPP Local Throughput Validation (2026-07-01)

This phase revisited the **local** `vcpd` capture/decode/encode path after the
radio-side work above, because new field logs showed that the device-side MPP
pipeline itself could become a bottleneck independently of the channel.

#### Findings before fixes

Initial `vcpd [mpp]` logs showed:

| Stage | Observed fps |
|-------|--------------|
| cap   | ~9.6 fps |
| dec   | ~9.6 fps |
| enc   | ~9.6 fps |

This first suggested that easycap or V4L2 capture itself was limited. That
turned out to be a false conclusion.

#### Root cause 1: serialized V4L2 -> VDEC -> VENC pipeline

The original [vcpd/src/video_mpp.c](../vcpd/src/video_mpp.c) camera path used a
single encode thread that serially performed:

`VIDIOC_DQBUF -> VDEC_SendStream -> VDEC_GetFrame -> VENC_SendFrame -> VENC_GetStream -> VIDIOC_QBUF`

Because V4L2 re-queue happened only after downstream processing, capture was
artificially backpressured by decode/encode. The observed `cap==dec==enc` did
**not** prove the source was limited; it only proved all stages were coupled.

#### Fix 1: decouple capture and encode with a second ring

Implemented in [vcpd/src/video_mpp.c](../vcpd/src/video_mpp.c):

- Added a second ring buffer at `/dev/shm/vcpd_mjpeg_ring`
- Added `mpp_capture_thread`: `V4L2 DQBUF -> MJPEG ring -> QBUF`
- Added `mpp_encode_thread`: `MJPEG ring -> VDEC -> VENC -> output ring`
- Preserved the existing ring+pipe selector architecture

After this change, logs became:

| Stage | Observed fps |
|-------|--------------|
| cap   | ~25.0 fps |
| dec   | ~19.3 fps |
| enc   | ~19.3 fps |

Conclusion: easycap/V4L2 capture was healthy; the previous ~10 fps ceiling was
an artifact of our threading architecture.

#### Verification: direct raw MJPEG test

Added `--test-mjpeg FILE` mode to `vcpd`, bypassing VDEC/VENC and dumping raw
V4L2 MJPEG frames to a file. The captured file was verified with `ffplay`:

```text
Stream #0:0: Video: mjpeg (Baseline), yuvj422p, 320x240, 25 fps
```

This confirmed that the source path can indeed provide **25 fps MJPEG**.

#### Root cause 2: per-frame heap churn + non-blocking VDEC/VENC handoff

After capture was fixed, the remaining local bottleneck was the handoff
`MJPEG ring -> malloc/memcpy -> CreateMB -> VDEC_SendStream`, plus a fully
non-blocking `VDEC/VENC` loop that treated normal backpressure as drops.

#### Fix 2: batched submit slots + bounded blocking timeouts

Implemented in [vcpd/src/video_mpp.c](../vcpd/src/video_mpp.c):

- Replaced per-frame `malloc/free` with a fixed pool of 8 in-flight submit slots
- Removed one extra memcpy from the hot path
- Submit MJPEG frames to VDEC in small bursts (`MJPEG_SUBMIT_BURST=6`)
- Switched to short blocking timeouts, aligned with the SDK demo
  [samples/example/demo/sample_demo_v4l2_mjpeg_vdec_venc.c](../samples/example/demo/sample_demo_v4l2_mjpeg_vdec_venc.c):
  - `VDEC_SendStream`: 5 ms
  - `VDEC_GetFrame`: 2 ms
  - `VENC_SendFrame`: 5 ms
  - `VENC_GetStream`: 2 ms
- Expanded diagnostics to `sub=ok/busy/full/drop`

#### Final result after fixes

Field logs on device after deploy:

```text
cap=125 dec=125 enc=125 sub=125/0/0/0 nals=125 ... fps cap=25.0 dec=25.0 enc=25.0
```

Interpretation:

- `cap=dec=enc=25 fps` — the local MPP pipeline now sustains the full target rate
- `sub=125/0/0/0` — no VDEC submit backpressure (`busy/full`) and no hard submit drops
- `rdrop=0 cdrop=0` — neither ring is overflowing

#### Decision

At this point **do not escalate to heavier local techniques yet**:

- no immediate need for more aggressive batching,
- no need to replace `/dev/shm` rings (already `tmpfs`, i.e. RAM-backed),
- no need yet for deeper zero-copy / bind-only MPP redesign on the device side.

The device-local `vcpd` pipeline is no longer the limiting factor for
`320x240@25`. Further optimization effort should return to the transport / host
side unless new evidence shows another local regression.

#### Final conclusion from Phase 6.5

The follow-up field logs after the last device build removed the remaining doubt:

```text
cap=125 dec=125 enc=125 sub=125/0/0/0 ... fps cap=25.0 dec=25.0 enc=25.0
```

This means the current `vcpd` implementation on device now sustains the full
`320x240@25` pipeline locally:

- capture is at target rate,
- MJPEG decode is at target rate,
- HEVC encode is at target rate,
- no VDEC submit backpressure,
- no local ring overruns.

So the active bottleneck has definitively moved away from device-local MPP code
and back to the **transport / host / radio delivery path**.

From this point onward, transport-layer work has priority over deeper local MPP
techniques. Any future local optimization (bind/zero-copy/etc.) should be treated
as optional engineering cleanup, not as the current critical path.

#### Active next focus: transport layer

The next optimization cycle should focus on:

1. Host/radio behavior under the new real `25 fps` source rate.
2. Whether the transport path still exhibits a pps ceiling / fragment survival
  issue once fed by a true 25 fps producer.
3. Gateway-side frame assembly / reorder / keyframe survival under the higher
  sustained source cadence.
4. Whether the previously derived channel assumptions (`~95 pps ceiling`, MTU
  tradeoffs, ACK strategy) still hold when the source is no longer locally
  throttled to ~19 fps or ~10 fps.

#### First transport-layer finding after restoring true 25 fps source

With device-local `vcpd` fixed to sustain `25 fps`, transport logs now show the
radio path becoming the dominant bottleneck immediately:

- Device-side `vcpd` is clean:
  `cap=dec=enc=125`, `sub=125/0/0/0`, `rdrop=0`, `cdrop=0`
- Drone-side `radiod` under stream load:
  `tx_pkt≈350-370/5s`, `prio[T≈25 V≈330-345]`, `air%=8.4-11.7`, `rx%=91-92%`
- Host-side `ulama-gw` under the same load:
  `video_rx≈180-228/5s`, `frames_rx≈1-34/5s`, `frags_rx≈129-158/5s`,
  `reorder_skip≈14-29`, `kf_lost≈1-4`, `fps_out≈0.1-6.7`

Interpretation:

1. The transport path is now the clear bottleneck.
2. Host receives plenty of video traffic (`video_rx` / `frags_rx` are non-zero),
   but full frame survival and in-order delivery are poor.
3. `ulama-gw` is still self-loading the uplink aggressively with host→drone
  control traffic: `ctrl_tx≈310-337/5s`, i.e. roughly **62-67 pps**.
4. In half-duplex monitor-mode Wi-Fi this host uplink control stream competes
   directly with the drone→host video stream and likely contributes materially to
   the observed keyframe loss / reorder skips.

#### Immediate transport-side action (step 1)

The first low-risk transport mitigation is to **reduce idle control replay rate**
in [ulama-gw/tools/ulama_gw.c](../ulama-gw/tools/ulama_gw.c):

- Old: `GW_CTRL_KEEPALIVE_MS = 20` (50 Hz idle replay)
- New: `GW_CTRL_KEEPALIVE_MS = 200` (5 Hz idle replay)

Rationale:

- The radiod CTRL watchdog is on the order of seconds, not tens of milliseconds.
- 5 Hz leaves a large safety margin for link liveness.
- Cutting host idle replay from ~64 pps to ~5 pps removes a major source of
  self-inflicted bidirectional contention without changing video format, MTU,
  or reliability logic.

#### Result of step 1

Field logs after this change showed that transport quality improved only
partially and, more importantly, revealed that the original diagnosis was
incomplete:

- Host `ctrl_tx` stayed high at `~260-280/5s`, not `~25/5s`.
- Therefore, the majority of host→drone control traffic was **not** coming from
  `maybe_send_ctrl_keepalive()` anymore.
- It was coming from `handle_cascade_rx()` re-forwarding control payloads that
  were already arriving from cascade-core at roughly `50-55 Hz`.

This means the real issue is broader than heartbeat frequency: unchanged control
payloads from cascade-core were being forwarded over radio again and again,
despite the gateway already caching the last control state for heartbeat replay.

#### Immediate transport-side action (step 2)

Deduplicate unchanged control payloads in
[ulama-gw/tools/ulama_gw.c](../ulama-gw/tools/ulama_gw.c):

- still cache every latest control payload,
- forward immediately only when the payload actually changes,
- rely on the existing low-rate gateway heartbeat to maintain liveness for
  unchanged control state.

This is safe because downstream already keeps its own last CRSF frame and
replays it for FC keepalive; repeating identical control payloads at radio rate
does not add semantic value, only contention.

#### Updated transport priority order

1. Reduce host CTRL keepalive pressure (done: 50 Hz -> 5 Hz).
2. Deduplicate unchanged host control payloads before radio forwarding (done).
3. Re-test and compare `ctrl_tx`, `video_rx`, `frames_rx`, `kf_rx`, `reorder_skip`,
   and `fps_out`.
4. Only after this, revisit heavier transport knobs: MTU, reliability policy,
   ACK tuning, keyframe redundancy, or scheduler behavior.

#### A/B result (2026-07-02): interference confirmed, control regressed

Running the deduped/5 Hz control build against the previous 50 Hz build proved
BOTH the hypothesis and its cost:

Video (much better):
- `ctrl_tx` dropped ~260-280/5s -> ~27-28/5s (5 Hz).
- `fps_out` rose ~0.1-6.7 -> **8-19**.
- `kf_lost` ~1-4 -> mostly **0** (occasional 1); `video_out` up to ~206 Kbit/s.

=> The host uplink control stream was indeed a dominant cause of the drone's
downlink video loss on this half-duplex channel.

Control (regressed):
- Operator reported laggy/"sticky" control.
- Drone `radiod` logged repeated `CTRL link LOST — no CTRL frames for 2000 ms.
  VIDEO TX suppressed` about every 7-10 s, each lasting ~2 s.

Root cause of the regression:
- With dedup, a **held** stick (constant payload) is no longer forwarded per
  cascade update; it is only refreshed by the 5 Hz heartbeat.
- In the drone's quasi-TDMA RX window (no cross-node sync), a 5 Hz control
  stream is easily missed for >2000 ms under burst loss, so the CTRL watchdog
  trips and **suppresses video** — hurting control latency AND video.

This is the exact control-rate vs video-quality conflict that a real TDMA
schedule resolves. It is not fixable by tuning a single rate.

#### Interim mitigation (before TDMA)

`GW_CTRL_KEEPALIVE_MS`: 200 -> **30** (~33 Hz held-stick refresh), dedup kept
(changes still forward immediately). Goal: responsive control + stay well within
the 2000 ms CTRL watchdog, while remaining lighter than the old 50 Hz
forward-every-duplicate. This is a stopgap; the durable fix remains Phase 8 TDMA.

Next A/B to compare at 33 Hz: `ctrl_tx` (~150-170/5s expected), absence of
`CTRL link LOST` on the drone, control responsiveness, and whether `fps_out`
holds up vs the 5 Hz run.

#### A/B result (33 Hz): rate is NOT the lever — real bug found

The 33 Hz run falsified the rate hypothesis and exposed the actual defect:

- `ctrl_tx` rose ~27/5s -> ~167/5s (33 Hz, as intended).
- Drone STILL logged `CTRL link LOST — no CTRL frames for 2000 ms` repeatedly.
- Video got WORSE, not better: `fps_out` ~8-19 (5 Hz run) -> ~1-11 (33 Hz run),
  because the extra host uplink again collided with the drone downlink.

Raising control 6.6x did not feed the drone watchdog at all. Root cause (found
in code):

- Drone `radiod` feeds its CTRL watchdog only for CTRL frames whose `dst_node`
  equals its own id (1) or broadcast `0xFF`
  ([radiod/src/rx_dispatcher.c](../radiod/src/rx_dispatcher.c): `ctrl_for_us`).
- The gateway keepalive built its frame with `.dst = 0`
  ([ulama-gw/tools/ulama_gw.c](../ulama-gw/tools/ulama_gw.c),
  `maybe_send_ctrl_keepalive`). `gw_addr_u16_to_u8(0)` -> `dst_node = 0`.
- `0 != 1` and `0 != 0xFF`, so **every keepalive was ignored by the drone's
  watchdog**. Only rare genuine control changes (which carry cascade-core's real
  dst) fed it, so a held stick starved the watchdog and tripped CTRL LOST after
  2 s — which suppresses video, coupling control loss into video loss.

This is why neither 5 Hz nor 33 Hz fixed control: both replay held-stick state
via the mis-addressed keepalive.

Fix: cache the real control frame's `src`/`dst` and reuse them for the keepalive
(so keepalives are addressed to the drone exactly like real control). Kept the
33 Hz refresh + dedup. Rebuilt `ulama-gw`.

Expected next run: drone stops logging `CTRL link LOST` while a stick is held,
control stops lagging, and video no longer gets suppression-induced dropouts.
This does NOT remove the need for Phase 8 TDMA (uplink/downlink still contend on
half-duplex), but it removes a real correctness bug on top of that contention.

#### A/B result (keepalive dst fix): control FIXED, watchdog quiet

The addressing fix worked. Operator confirmed "control was fine". Drone `radiod`
log across the whole run has **zero** `CTRL link LOST` lines (previously one
every ~7-10 s), so video TX is no longer suppression-cycled:

- `ctrl_tx` steady ~167/5s (33 Hz), no watchdog trips.
- `radiod` `prio[V=330-358]` steady, `qdrop=0`, no RX-only fallback.
- Host video: `fps_out` ~2-11 with dips; `kf_lost` ~1-4; `reorder_skip` ~10-27.

Interpretation: the two coupled control/video bugs are resolved (mis-addressed
keepalive + suppression feedback). What remains is the raw half-duplex
contention + adapter PER: video still loses fragments, especially when the
operator actively moves sticks (visible at 07:11:17-32 where `ctrl_tx` rises to
189-210 and `fps_out` drops to ~2.5-3.6).

#### Follow-up: roll back the keepalive rate (33 Hz was a wrong-cause patch)

The 33 Hz keepalive was raised earlier believing 5 Hz was too slow for control.
That was a misdiagnosis — the real fault was the dst=0 addressing. With
addressing fixed, a held stick only needs to keep a 2000 ms watchdog fed, and
real moves already go change-driven immediately.

So `GW_CTRL_KEEPALIVE_MS`: 30 -> **100** (10 Hz):
- ~20x safety margin vs the 2000 ms watchdog even under burst loss,
- steady-state uplink cut ~3x vs 33 Hz (from ~167/5s toward ~50/5s),
- responsiveness unaffected (real moves are change-driven),
- less uplink contention with downlink video → expect steadier `fps_out`.

Next A/B: confirm drone still shows no `CTRL link LOST`, control stays crisp,
and `fps_out` improves/steadies vs the 33 Hz run. If video is still not enough,
that is the definitive cue to implement Phase 8 TDMA (no more keepalive tuning).




#### If local issues reappear, next diagnostics would be

1. `MJPEG -> VDEC only` benchmark (disable HEVC encode)
2. `test-pattern -> VENC only` benchmark
3. Investigate `RK_MPI_SYS_Bind(VDEC -> VENC)` feasibility for a more direct
   hardware pipeline

But these are **not currently required** for the observed `25 fps` result.

---

### Phase 8: TX Synchronization Gap + TDMA-by-default Plan (2026-07-02)

#### The gap (verified in code)

A review of the actual TX paths shows the "master" (host, node 254) does **not**
follow any TX schedule, because it never goes through a scheduler at all:

- Host runs `ulama-gw --transport unow`. In
  [ulama-gw/tools/ulama_gw.c](../ulama-gw/tools/ulama_gw.c) the transport init has
  only two branches: `unow` and (else) `udp`. There is **no `radiod` branch**.
  So `ulama-gw` injects control frames directly via `pcap_inject()` (unow),
  with no TX scheduler, no slots, no coordination — it transmits whenever
  cascade-core hands it a frame (~50 Hz).
- Drone runs `vcpd`/`ulamad` through `radiod`, but `radiod` is started **without
  `-S`**: logs show `relay=off`, no `SYNC protocol enabled` line, and stats have
  no `sync[role=...]` suffix. So `radiod` only runs the **quasi-TDMA** loop
  (`TX burst -> RX window`), which the design doc itself describes as operating
  **without inter-node synchronization**.

Net effect (asymmetric, unsynchronized):

```
Drone:  vcpd/ulamad -> radiod (quasi-TDMA, sync OFF) -> wifi
Host:   ulama-gw    -> unow direct inject (no scheduler) -> wifi
```

So the master does not "fail to transmit in its slot" — it has **no slots and no
schedule**. And enabling `-S` on the drone alone is useless: by the election
rule (master = highest node_id = 254 = host), the drone would wait forever for a
master beacon that the host never sends, or elect itself while the host still
ignores beacons and slot boundaries.

#### Why this matters for packet loss

- `radiod` on the drone shows `qdrop=0`, empty queues, `air% ~ 8-12%` — the
  radio is **not saturated**; losses are not queue overflow.
- Host receives many fragments (`frags_rx ~ 130-158/5s`) but assembles few full
  frames (`frames_rx ~ 1-40/5s`) — sporadic per-fragment corruption.
- Host simultaneously injects ~50-64 pps of uplink control into the same
  half-duplex channel carrying the drone's downlink video, with **no CSMA/CA
  arbitration between the two nodes**.

Uncoordinated half-duplex TX from both ends is very likely a major loss
contributor. Caveat: it is probably **not the only** cause — the `rtl8192eu` in
monitor mode has a documented high raw per-fragment PER even at low airtime, so
TDMA will remove collisions but a residual PER floor may remain.

#### Goal: SYNC/TDMA must be the DEFAULT mode

SYNC/TDMA is already fully implemented in `radiod`
([radiod/tools/radiod.c](../radiod/tools/radiod.c): `master_cycle`, `slave_cycle`,
`candidate_cycle`, `radio_sync_tick`; superframe = SYNC beacon + DL + guard + UL
slots; PTP-like clock sync; bully election by node_id). It is only gated off by
`cfg.sync_enabled = false`. The missing pieces are: (1) host participation, and
(2) flipping the default on with a safe fallback.

Relevant constants: `SYNC_BEACON_INTERVAL_US = 12000` (12 ms superframe),
`SYNC_MAX_SLOTS = 4`, default `dl=2000us ul=2000us guard=300us`.

Topology note: video flows **drone -> host**. If the host (254) is master, the
drone (1) is a slave, so:
- DL (master host -> all) = control only (small, ~26-byte CRSF)
- UL slot for node 1 (drone) = the heavy flow (video + telemetry)
Therefore slot sizing must be asymmetric: shrink DL, enlarge the drone UL slot.

#### Plan — Variant A (recommended): host runs radiod, gw speaks radiod IPC

This reuses the existing, tested `radiod` sync engine on both ends instead of
duplicating TDMA logic inside `ulama-gw`.

Step A1 — Add a `radiod` transport branch to `ulama-gw`
- File: [ulama-gw/tools/ulama_gw.c](../ulama-gw/tools/ulama_gw.c)
- The transport primitives already exist in
  [ulama/src/common/transport.c](../ulama/src/common/transport.c):
  `ulama_transport_tx_init_radiod()` / `ulama_transport_rx_init_radiod()`
  (they connect to the radiod IPC unix socket via `radiod_acquire_fd`).
- Add an `else if (tk == ULAMA_TRANSPORT_KIND_RADIOD)` branch that calls those
  init functions (mirror how `vcpd` selects radiod transport).
- Keep `unow`/`udp` branches for bench/back-compat.

Step A2 — Run `radiod` on the host
- Start `radiod --iface wlan0 --node-id 254 --sync` on the host, same monitor
  interface `ulama-gw` uses today.
- `ulama-gw` then runs `--transport radiod` and stops touching pcap directly.
- Result: both control (host DL) and video (drone UL) go through the scheduler
  and land in disjoint superframe slots.

Step A3 — Make SYNC the default in `radiod`
- File: [radiod/tools/radiod.c](../radiod/tools/radiod.c)
- Flip `cfg->sync_enabled` default from `false` to `true` in `config_defaults()`.
- Add an explicit opt-out flag (e.g. `--no-sync`) for bench/regression use, so
  the old standalone quasi-TDMA path stays reachable.
- Keep election automatic: highest node_id wins, so host(254)=master,
  drone(1)=slave with no manual role config.

Step A4 — Recompute superframe/slot budget for the real 25 fps video
- Heavy path is the drone UL slot, not DL.
- Shrink DL (`--dl-us`) toward control-only size; enlarge the drone UL
  (`--ul-us`) so a full 25 fps video frame's fragments fit within one or a few
  superframes at 6 Mbps.
- Validate against `SYNC_BEACON_INTERVAL_US`; adjust beacon interval if the
  superframe no longer closes in time.

Step A5 — Field-validate SYNC (it has NEVER run live)
- Confirm on host stats: `sync[role=MASTER ...]`; on drone: `sync[role=SLAVE ...]`
  and `radio_sync_is_synced` true (beacon-fed watchdog).
- Compare against the current run: expect `reorder_skip` and `kf_lost` to drop
  and `fps_out` to rise, since drone video UL no longer collides with host
  control DL.

#### Plan — Variant B (not recommended): embed TDMA in the unow gw path

Re-implement election/superframe/slotting directly in `ulama-gw`'s unow path.
Rejected as default: duplicates the radiod sync engine, doubles maintenance, and
diverges the two nodes' timing implementations. Only consider if running a second
radiod process on the host proves infeasible.

#### Risks / open items

- SYNC has unit tests but **zero live field time** — treat first bring-up as
  experimental; keep `--no-sync` fallback one flag away.
- 1 Mbps is incompatible with video-in-slot (2 ms UL = 250 bytes < 1 fragment);
  we are at 6 Mbps now, but slot math must be redone, not assumed.
- Two radiod instances: ensure only one process owns the monitor iface; the host
  `ulama-gw` must go through radiod IPC, not open pcap in parallel.
- Host has no battery RTC concerns (unlike drone), but PTP offset filtering
  should still be sanity-checked on the host master.

#### Acceptance criteria for "TDMA on by default"

1. Fresh boot of both nodes, no extra flags, yields host=MASTER / drone=SLAVE.
2. `ulama-gw` transmits only through radiod (no direct pcap inject).
3. Drone video UL and host control DL occupy disjoint slots (no overlap in logs).
4. Measurable improvement vs current run in `kf_lost`, `reorder_skip`, `fps_out`.
5. `--no-sync` still reproduces the old standalone behavior for A/B.

#### Implementation status (2026-07-02) — Variant A applied

Done in this session:
- **A1** — `ulama-gw` now has a `ULAMA_TRANSPORT_KIND_RADIOD` branch
  ([ulama-gw/tools/ulama_gw.c](../ulama-gw/tools/ulama_gw.c)):
  `ulama_transport_tx_init_radiod(..., "ulama_gw_tx")` /
  `_rx_init_radiod(..., "ulama_gw_rx")`; usage string is now `udp|unow|radiod`.
- **A3** — `cfg->sync_enabled` default flipped to `true` in `config_defaults()`;
  added `--no-sync` (long-opt code 1001 → `cfg.sync_enabled = false`) plus usage
  text. `-S/--sync` is now "(default: ON)".
- **Bootstrap deadlock fix** (mandatory prerequisite, see below) —
  `slave_cycle` now announces an unslotted-but-synced node via `DELAY_REQ`.
- **Live SYNC timing fix** — `clock_sync` no longer calls `settimeofday()` from
  inside `radiod`. On real hardware the drone *did* hear the master's first
  beacon (`clock_sync: stepped system clock ...` in field logs), but that wall-
  clock jump could move the same process's TDMA deadline/sleep base mid-flight,
  causing the slave to miss its bootstrap `DELAY_REQ` / UL activity after the
  first sync event.
- Built clean: host `radiod` + host `ulama_gw` (x86, `host-unow`), target
  `radiod` + `vcpd` (ARM). Only a pre-existing `dst_node` maybe-uninitialized
  warning in `rx_dispatcher.c` (unrelated to these edits).

Deferred:
- **A4** (asymmetric DL/UL slot budget for 25 fps video-in-slot) — NOT tuned yet;
  running on stock `dl=2000 ul=2000 guard=300`. Revisit after first live sync.
- **A2/A5** — deployment + live validation is a field step (see runbook below).

#### CRITICAL: SYNC bootstrap deadlock (why sync never worked live)

Root cause found while enabling the default: the onboarding path was a
chicken-and-egg that no unit test exercised.
- A slave sends `DELAY_REQ` (its "I exist, give me a slot" announcement) only
  inside its own UL slot, i.e. only when `my_slot_index != 0xFF`.
- The master allocates a slot for a slave **only after** receiving that slave's
  `DELAY_REQ` (`radio_sync_on_delay_req_rx` → `alloc_slave` →
  `radio_sync_update_slot_map`).
- ⇒ A fresh slave has no slot, so it never announces, so it never gets a slot.
  The drone (slave) could therefore never obtain a UL slot and never send video.
- Compounding it: `clock.delay_req_pending` is cleared only on `DELAY_RESP` or on
  a master change ([radiod/src/clock_sync.c](../radiod/src/clock_sync.c)), so a
  single lost bootstrap `DELAY_REQ` would latch the flag and re-deadlock.

Fix (applied in `slave_cycle`): when synced but `my_slot_index == 0xFF`, in the
idle post-DL guard region, clear the stale `delay_req_pending` flag and inject a
`DELAY_REQ` (staggered by `own_node_id * 200us`). This lets the master learn of
the slave and assign a UL slot; a lost announce is retried every superframe.

Additional live-only root cause found after first field bring-up: `radiod`
computes TDMA deadlines with `gettimeofday()`/`usleep`, while `clock_sync` used
to call `settimeofday()` after enough SYNC samples. That means the slave could
receive a good SYNC, then immediately move its own wall clock by seconds/hours
and invalidate the scheduler's current superframe math. The sync engine already
tracks master/local offset explicitly, so stepping the host system clock from
inside the radio daemon is unnecessary and was removed.

#### Field bring-up runbook (first-ever live SYNC — treat as experimental)

1. Redeploy **all four** rebuilt binaries: host `radiod` + host `ulama_gw`
   (x86 `host-unow`), and target `radiod` + `vcpd` (ARM) on the drone.
2. Host: run `radiod --iface <mon> --node-id 254` (sync now ON by default) and
   `ulama-gw --transport radiod`. Only radiod may own the monitor iface.
3. Drone: run `radiod --node-id 1` (sync ON by default); `vcpd`/`ulamad` use
   `--transport radiod`.
4. Expect host stats `sync[role=MASTER]`, drone `sync[role=SLAVE]` + synced true,
   drone `my_slot_index != 0xFF` within a few superframes (bootstrap announce).
5. Fallback if anything regresses: add `--no-sync` on both radiod instances to
   restore the previous standalone quasi-TDMA behavior instantly.

---

### Phase 7: Async Reliable Send (2026-06-27)

**Problem**: `radio_espnow_send_reliable()` was **blocking** — it held the
global mutex and polled `pcap_next_ex()` for ACKs, silently consuming all
non-ACK frames (including NACKs from the gateway). This caused two issues:

1. **NACK starvation**: ~55% of NACKs were consumed and discarded during
   ACK-wait loops, making NACK-based retransmission ineffective.
2. **ACK timeout too short for large MTU**: at 1 Mbps, a 1000-byte packet
   takes ~8.4ms wire time. With 2ms ACK timeout, ACKs could never arrive
   in time, making reliable mode effectively unreliable while still blocking.

**Solution**: Rewrote `radio_espnow_send_reliable()` as **non-blocking async**:
- Sends packet once via `pcap_inject()`, stores in async slot buffer (32 slots)
- Returns immediately (no blocking, no mutex held during wait)
- ACKs processed transparently in `unow_diag_recv()` alongside other frames
- Timeouts and retransmits handled by `unow_async_tick_locked()` called from recv loop
- If async slots full, falls back to unreliable send

**Files changed**:
- `unow/src/unow_internal.h` — added `unow_async_slot_t`, buffer in context
- `unow/src/unow.c` — rewrote `radio_espnow_send_reliable()`, added tick/ack helpers
- `unow/src/unow_diag.c` — ACK frames now fed to async matcher, tick called each recv

**Default ACK timeout increased**: 3000us → 8000us (safe since no longer blocking)
**Default vcpd ACK timeout**: 2000us → 8000us

---

## Final Optimal Configuration

**vcpd** (device):
- `--reliable 2` — async MAC-level ACK+retry on all video packets
- `--ack-timeout 8000` — 8ms ACK wait (non-blocking, safe for MTU up to 1000)
- `--ack-retry 2` — 2 retries (3 attempts total)
- `--pace-us 300` — 300us inter-packet pacing
- `--fec 0` — FEC disabled (25% overhead counterproductive)
- `--lts-mtu 500` — larger packets = more throughput per received packet

**ulama-gw** (host):
- `--gap-tolerance 2` — tolerate up to 2 missing packets per NAL

**Channel limits** (8192eu monitor mode, RSSI -36..-44 dBm):
- Max ~95 pps at mtu=216 (~165 Kbit/s goodput)
- Max ~61 pps at mtu=500 (~248 Kbit/s goodput)
- Set `--bitrate` to ≤200 at mtu=500 to avoid saturating channel

---

## Key Constants Reference

| Constant | Value | Location |
|----------|-------|----------|
| UNOW_DOT11_MAX_MSDU | 2304 bytes | unow/include/unow/radio_unow.h:9 (802.11 spec) |
| UNOW_ACTION_VENDOR_OVERHEAD | 5 bytes | unow/include/unow/radio_unow.h:10 |
| ULAMA_ESPNOW_MAX_PAYLOAD | 2299 bytes | unow/include/unow/radio_unow.h:11 (2304-5) |
| ULAMA_FRAME_HEADER_SIZE | 14 bytes | ulama/include/ulama/ulama_frame.h:9 |
| ULAMA_FRAME_MAX_PAYLOAD | 2285 bytes | ulama/include/ulama/ulama_frame.h:10 (2299-14) |
| LTS_ENC_HEADER_SIZE | 4 bytes | vcpd/include/vcpd/lts_encoder.h:7 |
| LTS_ENC_MAX_PAYLOAD | 2281 bytes | vcpd/include/vcpd/lts_encoder.h:8 (2285-4) |
| LTS_HEADER_SIZE | 4 bytes | ulama-gw/include/ulama_gw/lts_decoder.h:7 |
| LTS_MAX_PAYLOAD | 2281 bytes | ulama-gw/include/ulama_gw/lts_decoder.h:8 (2285-4) |
| LTS_REORDER_WINDOW | 128 slots | ulama-gw/include/ulama_gw/lts_decoder.h:18 |
| LTS_EMIT_DEADLINE_MS | 200 ms | ulama-gw/include/ulama_gw/lts_decoder.h:19 |
| LTS_RETX_SLOTS | 512 | vcpd/include/vcpd/lts_encoder.h:18 |
| UNOW_ACK_TIMEOUT_US | 8000 us | unow/src/unow_internal.h:21 |
| UNOW_ACK_MAX_RETRY | 2 | unow/src/unow_internal.h:22 |
| UNOW_ASYNC_SLOTS | 32 | unow/src/unow_internal.h:24 |
| UNOW_DEDUP_WINDOW | 64 | unow/src/unow_internal.h:23 |
| NACK_MIN_INTERVAL_MS | 20 ms | ulama-gw/tools/ulama_gw.c:379 |
| NAL_ASSEMBLE_MAX | 64 KB | ulama-gw/tools/ulama_gw.c:88 |
| PARAM_NAL_DUP_COUNT | 3 | vcpd/tools/vcpd.c:125 |
| VIDEO_TS_GROUP_SIZE | 7 packets | vcpd/include/vcpd/video_source.h:9 |
| Default bitrate | 512 kbps | vcpd/tools/vcpd.c:268 |
| UNOW_TX_RATE | 1 Mbps legacy | unow/include/unow/unow_wire.h:16 |

## End-to-End Packet Flow

```
vcpd (LuckFox):
  H.265 encoder (ffmpeg/MPP, 512 kbps default)
       |
  NAL extraction (scan for start codes 00 00 01)
       |
  [VPS/SPS/PPS cache, 3x dup before IDR]
       |
  LTS encoder: NAL -> N chunks of 216 bytes
       |  each chunk gets 4-byte LTS header (stream_id, seq16, flags)
       |  last chunk flagged LTS_FLAG_LAST_OF_FRAME
       |
  retx buffer store (ring of 512 slots by seq)
       |
  ULAMA frame pack (14-byte header + 220-byte payload max)
       |
  send_ulama_video_ex() -> ulama_transport_tx_send_reliable()
       |                  -> radio_espnow_send_reliable()  [ASYNC: inject + track]
       |                       -> pcap_inject() + store in async_slots[]
       |                       -> ACKs matched in unow_diag_recv() loop
       |                       -> retransmits on timeout via unow_async_tick_locked()
       |
  ~~~~ WiFi 2.4 GHz (action frames at 1 Mbps legacy rate) ~~~~
       |
ulama-gw (Host):
  radio_espnow_recv() -> pcap_next_ex() on monitor-mode interface
       |
  ULAMA frame unpack
       |
  [if fragmented: frag_reassembly (up to 16 frags, 200ms timeout)]
       |
  LTS decode: extract (stream_id, seq16, flags, payload)
       |
  lts_decoder_insert() -> reorder buffer (128 slots, 200ms deadline)
       |
  lts_decoder_detect_gaps() -> try_send_nack() (every 20ms min)
       |                        -> NACK sent back to vcpd over ULAMA_CLASS_CTRL
       |
  lts_decoder_emit() -> ordered packet stream
       |
  emit_lts_to_cascade() -> NAL assembler
       |  concatenate payloads until LAST_OF_FRAME
       |  gap detected -> tolerate up to 2 (gap_tolerance)
       |  larger gap -> drop_nal()
       |
  flush_nal() -> cascade_frame -> UDP to cascade-core:5600
```
