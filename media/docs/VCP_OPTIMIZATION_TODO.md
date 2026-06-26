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

#### Task 3.1: Design FEC scheme

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

#### Task 3.2: Implement FEC encoder in vcpd

**New file**: `vcpd/src/lts_fec.c`

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

#### Task 3.3: Implement FEC decoder in ulama-gw

**New file**: `ulama-gw/src/lts_fec.c`

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

#### Task 3.4: Test FEC effectiveness

- Test with FEC K=4 and K=8
- Measure: NAL drop rate with/without FEC, bandwidth overhead, latency impact
- Compare with Phase 2 reliable send — which gives better drop/latency tradeoff?

---

### Phase 4: NAL assembler grace period (Path C)

**Goal**: Give the NACK-retransmit pipeline time to work by not immediately
dropping NALs when a gap is detected.

**Prerequisite**: Results from Phase 1-3 showing that residual gaps exist but
NACK retransmissions arrive — they just arrive too late because the NAL
assembler already dropped.

#### Task 4.1: Add grace period to NAL assembler

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

## Key Constants Reference

| Constant | Value | Location |
|----------|-------|----------|
| LTS_ENC_MAX_PAYLOAD | 216 bytes | vcpd/include/vcpd/lts_encoder.h:8 |
| LTS_HEADER_SIZE | 4 bytes | ulama-gw/include/ulama_gw/lts_decoder.h:7 |
| LTS_REORDER_WINDOW | 128 slots | ulama-gw/include/ulama_gw/lts_decoder.h:18 |
| LTS_EMIT_DEADLINE_MS | 200 ms | ulama-gw/include/ulama_gw/lts_decoder.h:19 |
| LTS_RETX_SLOTS | 512 | vcpd/include/vcpd/lts_encoder.h:18 |
| ULAMA_FRAME_HEADER_SIZE | 14 bytes | ulama/include/ulama/ulama_frame.h:9 |
| ULAMA_FRAME_MAX_PAYLOAD | 220 bytes | ulama/include/ulama/ulama_frame.h:10 |
| ULAMA_ESPNOW_MAX_PAYLOAD | 240 bytes | unow/include/unow/radio_unow.h:9 |
| UNOW_ACK_TIMEOUT_US | 3000 us | unow/src/unow_internal.h:21 |
| UNOW_ACK_MAX_RETRY | 3 | unow/src/unow_internal.h:22 |
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
  send_ulama_video() -> ulama_transport_tx_send()
       |                  -> radio_espnow_send()  [UNRELIABLE]
       |                       -> pcap_inject() on monitor-mode interface
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
       |  gap detected -> drop_nal() immediately  <-- PROBLEM
       |
  flush_nal() -> cascade_frame -> UDP to cascade-core:5600
```
