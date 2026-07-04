#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "radiod/clock_sync.h"
#include "radiod/sync_frame.h"

#define SYNC_ELECTION_TIMEOUT_US   500000
#define SYNC_BEACON_INTERVAL_US    12000
#define SYNC_BEACON_SLACK_US       2000
#define SYNC_MISS_THRESHOLD        3
#define SYNC_LOST_THRESHOLD        10
#define SYNC_HOLDOVER_TX_MAX       8
#define SYNC_SLAVE_TIMEOUT_US      300000
#define SYNC_PERIOD_CORR_SHIFT     3
#define SYNC_PERIOD_CORR_MAX_US    1500
#define SYNC_BOOTSTRAP_WINDOW_US   4000
#define SYNC_BOOTSTRAP_PERIOD      8
#define SYNC_DEDUP_WINDOW          32
#define SYNC_MAX_NODES             5

typedef enum {
    RADIO_ROLE_CANDIDATE = 0,
    RADIO_ROLE_MASTER,
    RADIO_ROLE_SLAVE,
} radio_role_t;

typedef enum {
    SYNC_STATE_SEARCHING = 0,
    SYNC_STATE_SYNCED,
    SYNC_STATE_HOLDOVER_TX,
    SYNC_STATE_HOLDOVER_RX_ONLY,
    SYNC_STATE_MASTER_TX,
    SYNC_STATE_MASTER_RX,
} sync_state_t;

typedef struct {
    uint8_t  node_id;
    int64_t  last_seen_us;
    bool     active;
} sync_slave_info_t;

typedef struct {
    uint8_t  master_node_id;
    uint32_t superframe_seq;
} sync_dedup_key_t;

typedef struct {
    uint8_t          own_node_id;

    radio_role_t     role;
    sync_state_t     state;

    int64_t          election_deadline_us;
    uint8_t          current_master_id;

    uint32_t         superframe_seq;
    sync_slave_info_t slaves[SYNC_MAX_NODES];
    uint8_t          num_known_slaves;

    clock_sync_t     clock;
    uint8_t          missed_beacons;
    int64_t          last_sync_rx_us;
    int64_t          predicted_anchor_us;
    int64_t          superframe_period_us;
    int64_t          period_correction_us;
    bool             period_correction_valid;
    int64_t          next_superframe_us;

    int64_t          dl_start_us;
    int64_t          dl_end_us;
    int64_t          my_ul_start_us;
    int64_t          my_ul_end_us;
    uint8_t          my_slot_index;

    uint16_t         dl_duration_us;
    uint16_t         ul_slot_us;
    uint16_t         guard_us;
    uint8_t          num_slots;
    uint8_t          slot_map[SYNC_MAX_SLOTS];
    uint16_t         bootstrap_window_us;
    uint8_t          bootstrap_period;

    sync_dedup_key_t dedup_ring[SYNC_DEDUP_WINDOW];
    uint16_t         dedup_head;
    uint16_t         dedup_count;

    uint32_t         sync_tx_count;
    uint32_t         sync_rx_count;
    uint32_t         sync_relay_count;
    uint32_t         delay_req_tx_count;
    uint32_t         elections_won;
    uint32_t         elections_lost;
    uint32_t         role_changes;
} radio_sync_t;

void radio_sync_init(radio_sync_t *s, uint8_t own_node_id,
                 uint16_t dl_us, uint16_t ul_us, uint16_t guard_us);

bool radio_sync_on_sync_rx(radio_sync_t *s,
               const sync_frame_t *frame,
               int64_t local_rx_us,
               uint8_t sender_node_id);

void radio_sync_on_delay_req_rx(radio_sync_t *s,
                const delay_req_frame_t *dreq,
                int64_t local_rx_us);

/* Master-side liveness refresh from any ULAMA packet received from a known
 * slave. This prevents slot eviction when a few DELAY_REQ packets are lost but
 * user data is still flowing in the assigned UL slot. */
void radio_sync_on_ul_packet_rx(radio_sync_t *s,
				uint8_t src_node,
				int64_t local_rx_us);

void radio_sync_build_beacon(radio_sync_t *s,
                  sync_frame_t *out_frame,
                  int64_t now_us);

void radio_sync_update_slot_map(radio_sync_t *s, int64_t now_us);

void radio_sync_compute_timing(radio_sync_t *s, int64_t local_now_us);

bool radio_sync_build_delay_req(radio_sync_t *s,
                 delay_req_frame_t *out,
                 int64_t now_us);

void radio_sync_on_beacon_timeout(radio_sync_t *s, int64_t now_us);

radio_role_t radio_sync_tick(radio_sync_t *s, int64_t now_us);

bool radio_sync_prepare_relay(radio_sync_t *s,
                   const sync_frame_t *rx_frame,
                   sync_frame_t *relay_frame,
                   int64_t local_tx_us);

radio_role_t radio_sync_get_role(const radio_sync_t *s);
bool radio_sync_is_synced(const radio_sync_t *s);
bool radio_sync_should_transmit_ul(const radio_sync_t *s);
int64_t radio_sync_get_offset(const radio_sync_t *s);
