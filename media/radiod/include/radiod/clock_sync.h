#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CLOCK_SYNC_HISTORY    8
#define CLOCK_SYNC_EMA_SHIFT  3       /* alpha = 1/8 = 0.125 */
#define CLOCK_SYNC_MAX_AGE_US 5000000 /* 5 sec */

typedef struct {
    int64_t t1;
    int64_t t2;
    int64_t t3;
    int64_t t4;
    bool    t3t4_valid;
} clock_sync_sample_t;

typedef struct {
    int64_t  offset_us;
    int64_t  rtt_us;
    int64_t  last_update_us;
    bool     synced;

    int64_t  ema_offset;
    bool     ema_initialized;

    int64_t  offset_history[CLOCK_SYNC_HISTORY];
    uint8_t  history_count;
    uint8_t  history_index;

    int64_t  pending_t3;
    uint32_t pending_seq;
    bool     delay_req_pending;

    uint8_t  sync_source_node;

    clock_sync_sample_t current;
} clock_sync_t;

void clock_sync_init(clock_sync_t *cs);

int64_t clock_sync_on_sync_rx(clock_sync_t *cs,
                               int64_t t1_master, int64_t t2_local,
                               uint8_t source_node);

void clock_sync_prepare_delay_req(clock_sync_t *cs,
                                   int64_t t3_local,
                                   uint32_t superframe_seq);

void clock_sync_on_delay_resp(clock_sync_t *cs,
                               int64_t t4_master);

int64_t clock_sync_to_master(const clock_sync_t *cs, int64_t local_us);

int64_t clock_sync_to_local(const clock_sync_t *cs, int64_t master_us);

bool clock_sync_is_valid(const clock_sync_t *cs, int64_t local_now_us);
