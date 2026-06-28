#include "radiod/stats.h"

#include <stdio.h>
#include <string.h>

void radio_stats_init(radio_stats_t *st, int64_t now_us)
{
	if (st == NULL)
		return;
	memset(st, 0, sizeof(*st));
	st->period_start_us = now_us;
}

void radio_stats_add_tx_time(radio_stats_t *st, uint64_t us)
{
	if (st != NULL)
		st->tx_time_us += us;
}

void radio_stats_add_rx_time(radio_stats_t *st, uint64_t us)
{
	if (st != NULL)
		st->rx_time_us += us;
}

void radio_stats_add_cycle(radio_stats_t *st)
{
	if (st != NULL)
		st->cycles++;
}

void radio_stats_add_tx_packet(radio_stats_t *st, uint8_t priority)
{
	if (st == NULL)
		return;
	st->tx_packets++;
	if (priority < 4)
		st->tx_by_prio[priority]++;
}

int radio_stats_report(radio_stats_t *st, int64_t now_us,
		       const radio_tx_scheduler_t *sched,
		       const radio_rx_dispatcher_t *rxd,
		       const radio_route_table_t *rt)
{
	int64_t elapsed_us;
	double elapsed_s;
	double tx_pct, rx_pct;

	if (st == NULL)
		return 0;

	elapsed_us = now_us - st->period_start_us;
	if (elapsed_us < (int64_t)RADIO_STATS_INTERVAL_S * 1000000LL)
		return 0;

	elapsed_s = (double)elapsed_us / 1000000.0;
	tx_pct = elapsed_us > 0 ? (double)st->tx_time_us * 100.0 / (double)elapsed_us : 0.0;
	rx_pct = elapsed_us > 0 ? (double)st->rx_time_us * 100.0 / (double)elapsed_us : 0.0;

	fprintf(stderr,
		"[radiod stats] %.1fs: cycles=%u tx_pkt=%u rx_pkt=%u "
		"tx%%=%.1f rx%%=%.1f "
		"prio[C=%u T=%u V=%u B=%u] ",
		elapsed_s,
		st->cycles,
		st->tx_packets,
		rxd != NULL ? rxd->stats.rx_dispatched : 0,
		tx_pct, rx_pct,
		st->tx_by_prio[0], st->tx_by_prio[1],
		st->tx_by_prio[2], st->tx_by_prio[3]);

	if (sched != NULL) {
		fprintf(stderr, "q[C=%d T=%d V=%d B=%d] ",
			radio_tx_queue_depth(sched, 0),
			radio_tx_queue_depth(sched, 1),
			radio_tx_queue_depth(sched, 2),
			radio_tx_queue_depth(sched, 3));
	}

	if (rxd != NULL) {
		fprintf(stderr, "ack[ok=%u to=%u retx=%u] dedup=%u/%u",
			rxd->stats.tx_ack_ok,
			rxd->stats.tx_ack_timeout,
			rxd->stats.tx_retries,
			rxd->stats.rx_dedup_dropped,
			rxd->stats.rx_ulama_dedup_dropped);

		if (rxd->stats.relay_forwarded > 0 ||
		    rxd->stats.relay_dropped_ttl > 0) {
			fprintf(stderr, " relay[fwd=%u ttl_drop=%u "
				"C=%u T=%u V=%u B=%u]",
				rxd->stats.relay_forwarded,
				rxd->stats.relay_dropped_ttl,
				rxd->stats.relay_by_prio[0],
				rxd->stats.relay_by_prio[1],
				rxd->stats.relay_by_prio[2],
				rxd->stats.relay_by_prio[3]);
		}
	}

	if (rt != NULL) {
		fprintf(stderr, " routes=%d", radio_route_count(rt));
	}

	fprintf(stderr, "\n");

	/* Reset for next period */
	memset(st, 0, sizeof(*st));
	st->period_start_us = now_us;
	return 1;
}
