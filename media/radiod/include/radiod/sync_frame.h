#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SYNC_FRAME_MAGIC      0xBEU
#define SYNC_FRAME_VERSION    0x03U
#define DELAY_REQ_MAGIC       0xBDU
#define DELAY_REQ_VERSION     0x02U

#define SYNC_MAX_SLOTS        4

/* Monotonic-only protocol (v2).
 *
 * TDMA scheduling runs entirely on each node's CLOCK_MONOTONIC; the slave
 * anchors slot geometry to the local RX time of the beacon (last_sync_rx_us),
 * so there is NO cross-node clock offset to carry on the wire. The only
 * time value in the beacon is master_time_us — the master's CLOCK_REALTIME
 * (gettimeofday) — which the slave uses solely to make its system/log clock
 * human-readable. It never enters scheduling. */
#define SYNC_FRAME_SIZE       31
#define SYNC_FRAME_MAX_SIZE   SYNC_FRAME_SIZE
#define DELAY_REQ_FRAME_SIZE  8

typedef struct {
    uint8_t  master_node_id;
    uint8_t  sender_node_id;
    uint32_t superframe_seq;
    int64_t  master_time_us;   /* master CLOCK_REALTIME (wall), log-only */
    uint16_t dl_duration_us;
    uint16_t ul_slot_us;
    uint16_t guard_us;
    uint8_t  num_slots;
    uint8_t  relay_hops;
    uint8_t  slot_map[SYNC_MAX_SLOTS];
    uint16_t bootstrap_window_us;
    uint8_t  bootstrap_period;
} sync_frame_t;

typedef struct {
    uint8_t  requester_node_id;
    uint8_t  target_node_id;
    uint32_t superframe_seq;
} delay_req_frame_t;

bool sync_frame_pack(const sync_frame_t *in,
                     uint8_t *out, size_t capacity, size_t *out_len);
bool sync_frame_unpack(const uint8_t *in, size_t in_len,
                       sync_frame_t *out);

bool delay_req_pack(const delay_req_frame_t *in,
                    uint8_t *out, size_t capacity, size_t *out_len);
bool delay_req_unpack(const uint8_t *in, size_t in_len,
                      delay_req_frame_t *out);
