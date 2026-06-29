#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SYNC_FRAME_MAGIC      0xBEU
#define SYNC_FRAME_VERSION    0x01U
#define DELAY_REQ_MAGIC       0xBDU
#define DELAY_REQ_VERSION     0x01U

#define SYNC_MAX_SLOTS        4
#define SYNC_MAX_DELAY_RESP   4

#define SYNC_FRAME_MIN_SIZE   29
#define SYNC_FRAME_MAX_SIZE   (29 + SYNC_MAX_DELAY_RESP * 9)
#define DELAY_REQ_FRAME_SIZE  16

typedef struct {
    uint8_t  node_id;
    int64_t  t4_us;
} sync_delay_resp_t;

typedef struct {
    uint8_t  master_node_id;
    uint8_t  sender_node_id;
    uint32_t superframe_seq;
    int64_t  origin_time_us;
    uint16_t dl_duration_us;
    uint16_t ul_slot_us;
    uint16_t guard_us;
    uint8_t  num_slots;
    uint8_t  relay_hops;
    uint8_t  slot_map[SYNC_MAX_SLOTS];
    uint8_t  num_delay_resp;
    sync_delay_resp_t delay_resp[SYNC_MAX_DELAY_RESP];
} sync_frame_t;

typedef struct {
    uint8_t  requester_node_id;
    uint8_t  target_node_id;
    int64_t  t3_us;
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
