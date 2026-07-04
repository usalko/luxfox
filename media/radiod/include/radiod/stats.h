#pragma once

#include <stdint.h>

#include "radiod/route_table.h"
#include "radiod/rx_dispatcher.h"
#include "radiod/tx_scheduler.h"

/* -------------------------------------------------------------------
 * Radio utilization metrics for monitoring.
 *
 * Tracked per reporting interval:
 *   - TX/RX utilization (time in TX / total time)
 *   - CTRL latency
 *   - Queue depths per priority
 *   - ACK success rate
 * ------------------------------------------------------------------- */

#define RADIO_STATS_INTERVAL_S  5

typedef struct {
	/* Timing */
	int64_t  period_start_us;
	uint64_t tx_time_us;
	uint64_t tx_airtime_us;
	uint64_t rx_time_us;
	uint64_t idle_time_us;

	/* Packet counts this period */
	uint32_t tx_packets;
	uint32_t rx_packets;
	uint32_t cycles;

	/* Per-priority TX counts */
	uint32_t tx_by_prio[4];
	uint32_t queue_drop_snap[4];
	uint32_t ipc_tx_ok_snap;
	uint32_t ipc_tx_fail_snap;

	/* SYNC stats (snapshot for reporting) */
	uint32_t sync_tx;
	uint32_t sync_rx;
	uint32_t sync_relay;
	uint32_t delay_req_tx;
	uint8_t  current_role;
	uint8_t  current_master;
	uint8_t  sync_state;
	uint8_t  num_slots;
	uint8_t  my_slot_index;
	uint8_t  num_known_slaves;
	uint8_t  sync_phase_valid;
	int64_t  sync_phase_last_us;
	int64_t  sync_phase_max_abs_us;
	uint8_t  sync_pll_valid;
	int64_t  sync_pll_filtered_us;
	int64_t  sync_pll_corr_us;
} radio_stats_t;

void radio_stats_init(radio_stats_t *st, int64_t now_us);

/* Call at the start and end of TX/RX slots to accumulate time. */
void radio_stats_add_tx_time(radio_stats_t *st, uint64_t us);
void radio_stats_add_tx_airtime(radio_stats_t *st, uint64_t us);
void radio_stats_add_rx_time(radio_stats_t *st, uint64_t us);
void radio_stats_add_cycle(radio_stats_t *st);
void radio_stats_add_tx_packet(radio_stats_t *st, uint8_t priority);

/*
 * Print stats if reporting interval has elapsed.
 * Includes queue depths from scheduler and RX stats from dispatcher.
 * Returns true if stats were printed (and reset for next period).
 */
int  radio_stats_report(radio_stats_t *st, int64_t now_us,
			const radio_tx_scheduler_t *sched,
			const radio_rx_dispatcher_t *rxd,
			const radio_route_table_t *rt,
			const radio_ipc_server_t *ipc);

struct radio_sync_t_fwd;
void radio_stats_update_sync(radio_stats_t *st,
			     const void *sync_ctx);
