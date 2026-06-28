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
	uint64_t rx_time_us;
	uint64_t idle_time_us;

	/* Packet counts this period */
	uint32_t tx_packets;
	uint32_t rx_packets;
	uint32_t cycles;

	/* Per-priority TX counts */
	uint32_t tx_by_prio[4];
} radio_stats_t;

void radio_stats_init(radio_stats_t *st, int64_t now_us);

/* Call at the start and end of TX/RX slots to accumulate time. */
void radio_stats_add_tx_time(radio_stats_t *st, uint64_t us);
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
			const radio_route_table_t *rt);
