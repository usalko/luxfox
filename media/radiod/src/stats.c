#include "radiod/stats.h"
#include "radiod/sync.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

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

void radio_stats_add_tx_airtime(radio_stats_t *st, uint64_t us)
{
	if (st != NULL)
		st->tx_airtime_us += us;
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
		       const radio_route_table_t *rt,
		       const radio_ipc_server_t *ipc)
{
	int64_t elapsed_us;
	double elapsed_s;
	double tx_pct, air_pct, rx_pct;

	if (st == NULL)
		return 0;

	elapsed_us = now_us - st->period_start_us;
	if (elapsed_us < (int64_t)RADIO_STATS_INTERVAL_S * 1000000LL)
		return 0;

	elapsed_s = (double)elapsed_us / 1000000.0;
	tx_pct = elapsed_us > 0 ? (double)st->tx_time_us * 100.0 / (double)elapsed_us : 0.0;
	air_pct = elapsed_us > 0 ? (double)st->tx_airtime_us * 100.0 / (double)elapsed_us : 0.0;
	rx_pct = elapsed_us > 0 ? (double)st->rx_time_us * 100.0 / (double)elapsed_us : 0.0;

	char ts[9]; { time_t _t = time(NULL); strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&_t)); }
	fprintf(stderr,
		"%s [radiod stats] %.1fs: cycles=%u tx_pkt=%u rx_pkt=%u "
		"tx%%=%.1f air%%=%.1f rx%%=%.1f "
		"prio[C=%u T=%u V=%u B=%u] ",
		ts, elapsed_s,
		st->cycles,
		st->tx_packets,
		rxd != NULL ? rxd->stats.rx_dispatched : 0,
		tx_pct, air_pct, rx_pct,
		st->tx_by_prio[0], st->tx_by_prio[1],
		st->tx_by_prio[2], st->tx_by_prio[3]);

	if (sched != NULL) {
		uint32_t qdrop[4];
		for (int p = 0; p < 4; p++)
			qdrop[p] = sched->queues[p].dropped - st->queue_drop_snap[p];

		fprintf(stderr, "q[C=%d T=%d V=%d B=%d] ",
			radio_tx_queue_depth(sched, 0),
			radio_tx_queue_depth(sched, 1),
			radio_tx_queue_depth(sched, 2),
			radio_tx_queue_depth(sched, 3));
		fprintf(stderr, "qdrop[C=%u T=%u V=%u B=%u] ",
			qdrop[0], qdrop[1], qdrop[2], qdrop[3]);
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

	if (ipc != NULL) {
		int active = 0;
		uint32_t ipc_ok = ipc->tx_ok - st->ipc_tx_ok_snap;
		uint32_t ipc_fail = ipc->tx_fail - st->ipc_tx_fail_snap;
		for (int i = 0; i < ipc->client_count; i++)
			if (ipc->clients[i].active) active++;
		fprintf(stderr, " ipc[clients=%d ok=%u fail=%u tot_fail=%u]",
			active, ipc_ok, ipc_fail, ipc->tx_fail);
	}

	if (st->current_role > 0) {
		static const char *role_names[] = {"CAND", "MASTER", "SLAVE"};
		const char *rn = st->current_role <= 2
			? role_names[st->current_role] : "?";
		fprintf(stderr, " sync[role=%s master=%u tx=%u rx=%u relay=%u dreq=%u]",
			rn, st->current_master,
			st->sync_tx, st->sync_rx, st->sync_relay,
			st->delay_req_tx);
	}

	if (rxd != NULL && (rxd->stats.rx_sync > 0 ||
			    rxd->stats.rx_delay_req > 0)) {
		fprintf(stderr, " sync_rx[sync=%u dreq=%u relayed=%u]",
			rxd->stats.rx_sync,
			rxd->stats.rx_delay_req,
			rxd->stats.sync_relayed);
	}

	fprintf(stderr, "\n");

	/* Reset for next period but preserve queue-drop snapshots. */
	int64_t saved_start = now_us;
	uint32_t saved_qdrop[4] = {0, 0, 0, 0};
	uint32_t saved_ipc_tx_ok = 0;
	uint32_t saved_ipc_tx_fail = 0;
	if (sched != NULL) {
		for (int p = 0; p < 4; p++)
			saved_qdrop[p] = sched->queues[p].dropped;
	}
	if (ipc != NULL) {
		saved_ipc_tx_ok = ipc->tx_ok;
		saved_ipc_tx_fail = ipc->tx_fail;
	}
	memset(st, 0, sizeof(*st));
	st->period_start_us = saved_start;
	for (int p = 0; p < 4; p++)
		st->queue_drop_snap[p] = saved_qdrop[p];
	st->ipc_tx_ok_snap = saved_ipc_tx_ok;
	st->ipc_tx_fail_snap = saved_ipc_tx_fail;
	return 1;
}

void radio_stats_update_sync(radio_stats_t *st,
			     const void *sync_ctx)
{
	if (st == NULL || sync_ctx == NULL)
		return;
	const radio_sync_t *s = (const radio_sync_t *)sync_ctx;
	st->sync_tx = s->sync_tx_count;
	st->sync_rx = s->sync_rx_count;
	st->sync_relay = s->sync_relay_count;
	st->delay_req_tx = s->delay_req_tx_count;
	st->current_role = (uint8_t)s->role;
	st->current_master = s->current_master_id;
}
