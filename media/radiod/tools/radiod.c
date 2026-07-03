/*
 * radiod — unified radio multiplexer daemon for LuckFox.
 *
 * One process owns the pcap handle (wlan0 monitor mode).
 * Applications (vcpd, ulamad) send/receive through IPC.
 * TDMA-like scheduling guarantees CTRL always gets through.
 * Optional mesh relay forwards frames to other nodes.
 */

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ulama/ulama_frame.h"
#include "ulama/ulama_version.h"

#include "radiod/ipc.h"
#include "radiod/route_table.h"
#include "radiod/rx_dispatcher.h"
#include "radiod/stats.h"
#include "radiod/sync.h"
#include "radiod/sync_frame.h"
#include "radiod/tx_scheduler.h"
#include "radiod/watchdog.h"

#ifndef ULAMA_WITH_UNOW
#define ULAMA_WITH_UNOW 0
#endif

#if ULAMA_WITH_UNOW
#include <pcap/pcap.h>
#include "unow_internal.h"
#endif

/* ---- TDMA Constants ---- */

#define TX_SLOT_SIZE          2
#define RX_SLOT_US            2000
#define ACK_TIMEOUT_US        12000
#define ACK_MAX_RETRY         6
/*
 * Inter-packet pacing inside a TX slot. The rtl8192eu USB adapter silently drops
 * the TAIL of a rapid inject burst (its TX ring/URB queue overflows), so a
 * keyframe's 4 back-to-back full-size fragments lost fragments 2-3 on EVERY send
 * — and because the drop is systematic, the time-spread keyframe copies all lost
 * the same fragments and never completed reassembly (kf_lost stayed ~half while
 * 1-2 fragment P-frames passed fine). Small TX_SLOT_SIZE (2) plus a generous gap
 * lets the ring drain between injects; a 4-fragment keyframe now spans multiple
 * cycles (with a ~2 ms RX slot between) instead of one ~1 ms burst. Airtime is
 * far from saturated (~6%), and cycle rate (~700/s) dwarfs the ~45 pkt/s load,
 * so throughput and latency are unaffected.
 */
#define TX_PACE_US            1500

/* ---- Globals ---- */

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

/* ---- Configuration ---- */

typedef struct {
	const char *iface;
	const char *sock_path;
	uint8_t     node_id;
	uint8_t     tx_rate_500kbps;
	bool        verbose;
	bool        relay;
	bool        has_dst_mac;
	uint8_t     dst_mac[6];
	uint32_t    tx_slot_size;
	uint32_t    rx_slot_us;
	uint32_t    ack_timeout_us;
	uint32_t    ack_max_retry;
	uint16_t    sync_dl_us;
	uint16_t    sync_ul_us;
	uint16_t    sync_guard_us;
	bool        sync_enabled;
} radiod_config_t;

static void config_defaults(radiod_config_t *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->iface = "wlan0";
	cfg->sock_path = RADIO_IPC_SOCK_PATH;
	cfg->node_id = 1;
	cfg->tx_rate_500kbps = 12;  /* 6 Mbps = 12 * 500kbps */
	cfg->tx_slot_size = TX_SLOT_SIZE;
	cfg->rx_slot_us = RX_SLOT_US;
	cfg->ack_timeout_us = ACK_TIMEOUT_US;
	cfg->ack_max_retry = ACK_MAX_RETRY;
	cfg->sync_dl_us = 1500;
	cfg->sync_ul_us = 5000;
	cfg->sync_guard_us = 300;
	cfg->sync_enabled = true;
}

static int64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000LL;
}

static int64_t wall_now_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

static uint8_t g_tx_rate_500kbps = UNOW_TX_RATE_1MBPS;

/* ---- IPC TX callback ---- */

typedef struct {
	radio_tx_scheduler_t *sched;
	radio_watchdog_t     *wd;
	bool                  verbose;
} ipc_tx_ctx_t;

static void on_ipc_tx_request(const radio_tx_request_t *req,
			      size_t total_len, void *user_ctx)
{
	ipc_tx_ctx_t *ctx = (ipc_tx_ctx_t *)user_ctx;

	(void)total_len;

	if (ctx->wd != NULL && radio_watchdog_video_blocked(ctx->wd)) {
		if (req->priority != RADIO_PRIO_CTRL)
			return;
	}

	radio_tx_enqueue(ctx->sched, req->priority, req->reliability,
			 req->payload, req->payload_len);
}

/* ---- TX helper: inject one slot into radio ---- */

#if ULAMA_WITH_UNOW
static uint64_t estimate_unow_airtime_us(size_t wire_len, uint8_t rate_half_mbps)
{
	if (wire_len == 0U)
		return 0U;
	if (rate_half_mbps == 0U)
		rate_half_mbps = g_tx_rate_500kbps > 0U ? g_tx_rate_500kbps : UNOW_TX_RATE_1MBPS;

	/* Approximation for 802.11 management/action frames in monitor injection:
	 * long preamble/PLCP (~192 us) + payload bits at the configured PHY rate.
	 * radiotap legacy rate units are 500 kbit/s, so 1 Mbps = 2. */
	uint64_t payload_us = (((uint64_t)wire_len * 8ULL * 2ULL) + rate_half_mbps - 1U)
		/ (uint64_t)rate_half_mbps;
	return 192ULL + payload_us;
}

/* Estimated on-air time for a queued ULAMA payload of `payload_len` bytes,
 * including the radiotap/802.11/vendor-action wire headers. Used to bound a
 * UL/DL slot by airtime instead of wall-clock: pcap_inject only enqueues a
 * frame (the airtime happens later on the wire), so a wall-clock loop can pile
 * several frames into the driver that transmit long past the slot boundary. */
#define TX_WIRE_HDR_OVERHEAD 48U
static uint64_t estimate_payload_airtime_us(size_t payload_len)
{
	return estimate_unow_airtime_us(payload_len + TX_WIRE_HDR_OVERHEAD,
					g_tx_rate_500kbps);
}

/* Peek the next packet the scheduler would dequeue (priority order P0..P3)
 * without removing it, so the caller can check it fits the remaining slot. */
static const radio_tx_slot_t *tx_peek_next(const radio_tx_scheduler_t *sched,
					   uint8_t *out_prio)
{
	for (uint8_t p = 0; p < RADIO_PRIO_COUNT; p++) {
		const radio_tx_slot_t *s = radio_tx_peek(sched, p);
		if (s != NULL) {
			if (out_prio != NULL)
				*out_prio = p;
			return s;
		}
	}
	return NULL;
}

static uint64_t tx_inject_slot(const radio_tx_slot_t *slot,
			       pcap_t *pcap,
			       const uint8_t own_mac[6],
			       const uint8_t default_dst[6],
			       radio_rx_dispatcher_t *rxd,
			       const radio_route_table_t *rt)
{
	/* Choose destination MAC:
	 * 1. Slot has explicit dst_mac (relay packet with known route) → use it
	 * 2. Route table has entry for dst_node → use it
	 * 3. Configured default → use it
	 * 4. Broadcast */
	const uint8_t *dst_mac = default_dst;

	if (slot->has_dst_mac) {
		dst_mac = slot->dst_mac;
	} else if (rt != NULL && slot->len >= ULAMA_FRAME_HEADER_SIZE) {
		static uint8_t route_mac[6];
		uint8_t dst_node = slot->data[3]; /* ULAMA header byte 3 */
		if (radio_route_lookup(rt, dst_node, route_mac))
			dst_mac = route_mac;
	}

	if (slot->reliability) {
		uint16_t seq = radio_async_next_seq(rxd);
		uint8_t seq_payload[2U + RADIO_TX_MAX_FRAME];
		seq_payload[0] = (uint8_t)(seq >> 8);
		seq_payload[1] = (uint8_t)(seq & 0xFFU);
		memcpy(seq_payload + 2U, slot->data, slot->len);

		uint8_t wire[sizeof(struct unow_radiotap_tx_header) +
			     sizeof(struct unow_dot11_mgmt_header) +
			     sizeof(struct unow_action_vendor_header) +
			     2U + RADIO_TX_MAX_FRAME];
		size_t wire_len = unow_build_action_frame_ex(
			wire, sizeof(wire),
			own_mac, dst_mac,
			seq_payload, slot->len + 2U,
			g_tx_rate_500kbps,
			UNOW_VENDOR_SUBTYPE_DATA_SEQ);

		if (wire_len > 0U) {
			pcap_inject(pcap, wire, wire_len);
			radio_async_store(rxd, wire, wire_len, seq);
			return estimate_unow_airtime_us(wire_len, g_tx_rate_500kbps);
		}
	} else {
		uint8_t wire[sizeof(struct unow_radiotap_tx_header) +
			     sizeof(struct unow_dot11_mgmt_header) +
			     sizeof(struct unow_action_vendor_header) +
			     RADIO_TX_MAX_FRAME];
		size_t wire_len = unow_build_action_frame(
			wire, sizeof(wire),
			own_mac, dst_mac,
			slot->data, slot->len,
			g_tx_rate_500kbps);

		if (wire_len > 0U) {
			pcap_inject(pcap, wire, wire_len);
			return estimate_unow_airtime_us(wire_len, g_tx_rate_500kbps);
		}
	}

	return 0U;
}
#endif

/* ---- Usage ---- */

static void usage(const char *prog)
{
	fprintf(stderr, "radiod: build #%d (%s@%s) %s\n",
		ULAMA_BUILD_NUMBER, ULAMA_GIT_BRANCH, ULAMA_GIT_HASH, ULAMA_BUILD_DATE);
	fprintf(stderr,
		"Usage: %s [OPTIONS]\n"
		"\n"
		"Unified radio multiplexer daemon.\n"
		"\n"
		"Options:\n"
		"  -i, --iface IFACE     Monitor interface (default: mon0)\n"
		"  -n, --node-id ID      Node ID (default: 1)\n"
		"  -d, --dst-mac MAC     Destination MAC (default: broadcast)\n"
		"      --tx-rate-mbps N  Legacy radiotap TX rate for injected frames (default: 6)\n"
		"  -s, --socket PATH     IPC socket path (default: %s)\n"
		"  -T, --tx-slot N       Max TX packets per slot (default: %u)\n"
		"  -R, --rx-slot US      RX slot duration in µs (default: %u)\n"
		"  -K, --ack-timeout US  Reliable ACK timeout in µs (default: %u)\n"
		"  -Y, --ack-retry N     Reliable max retransmits   (default: %u)\n"
		"      --relay           Enable mesh relay mode\n"
		"  -S, --sync            Enable SYNC protocol (TDMA + election)  (default: ON)\n"
		"      --no-sync         Disable SYNC; fall back to standalone quasi-TDMA\n"
		"  -D, --dl-us US        SYNC DL slot duration (default: 2000)\n"
		"  -U, --ul-us US        SYNC UL slot duration (default: 2000)\n"
		"  -G, --guard-us US     SYNC guard interval (default: 300)\n"
		"  -v, --verbose         Verbose output\n"
		"  -h, --help            Show this help\n",
		prog, RADIO_IPC_SOCK_PATH, TX_SLOT_SIZE, RX_SLOT_US,
		ACK_TIMEOUT_US, ACK_MAX_RETRY);
}

static bool parse_mac(const char *text, uint8_t mac[6])
{
	unsigned int o[6];

	if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x",
		   &o[0], &o[1], &o[2], &o[3], &o[4], &o[5]) != 6)
		return false;
	for (int i = 0; i < 6; i++)
		mac[i] = (uint8_t)o[i];
	return true;
}

static bool parse_tx_rate_mbps(const char *text, uint8_t *rate_500kbps)
{
	long mbps;

	if (text == NULL || rate_500kbps == NULL)
		return false;

	mbps = strtol(text, NULL, 10);
	if (mbps < 1 || mbps > 63)
		return false;

	*rate_500kbps = (uint8_t)(mbps * 2);
	return true;
}

/* ---- SYNC cycle helpers ---- */

#if ULAMA_WITH_UNOW
static void sync_inject_beacon(radio_sync_t *sync,
			       pcap_t *pcap,
			       const uint8_t own_mac[6],
			       int64_t ts)
{
	sync_frame_t beacon;
	radio_sync_build_beacon(sync, &beacon, ts);
	beacon.master_time_us = wall_now_us();

	uint8_t packed[SYNC_FRAME_MAX_SIZE];
	size_t packed_len;
	if (!sync_frame_pack(&beacon, packed, sizeof(packed), &packed_len))
		return;

	uint8_t wire[sizeof(struct unow_radiotap_tx_header) +
		     sizeof(struct unow_dot11_mgmt_header) +
		     sizeof(struct unow_action_vendor_header) +
		     SYNC_FRAME_MAX_SIZE];
	const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	size_t wire_len = unow_build_action_frame_ex(
		wire, sizeof(wire), own_mac, broadcast,
		packed, packed_len,
		g_tx_rate_500kbps, UNOW_VENDOR_SUBTYPE_SYNC);
	if (wire_len > 0U)
		pcap_inject(pcap, wire, wire_len);
}

static void sync_inject_delay_req(radio_sync_t *sync,
				  pcap_t *pcap,
				  const uint8_t own_mac[6],
				  int64_t ts)
{
	delay_req_frame_t dreq;
	if (!radio_sync_build_delay_req(sync, &dreq, ts))
		return;

	uint8_t packed[DELAY_REQ_FRAME_SIZE];
	size_t packed_len;
	if (!delay_req_pack(&dreq, packed, sizeof(packed), &packed_len))
		return;

	uint8_t wire[sizeof(struct unow_radiotap_tx_header) +
		     sizeof(struct unow_dot11_mgmt_header) +
		     sizeof(struct unow_action_vendor_header) +
		     DELAY_REQ_FRAME_SIZE];
	const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	size_t wire_len = unow_build_action_frame_ex(
		wire, sizeof(wire), own_mac, broadcast,
		packed, packed_len,
		g_tx_rate_500kbps, UNOW_VENDOR_SUBTYPE_DELAY_REQ);
	if (wire_len > 0U)
		pcap_inject(pcap, wire, wire_len);
}

static void sync_inject_null_frame(pcap_t *pcap,
				   const uint8_t own_mac[6])
{
	uint8_t null_byte = 0x00;
	uint8_t wire[sizeof(struct unow_radiotap_tx_header) +
		     sizeof(struct unow_dot11_mgmt_header) +
		     sizeof(struct unow_action_vendor_header) + 1];
	const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	size_t wire_len = unow_build_action_frame(
		wire, sizeof(wire), own_mac, broadcast,
		&null_byte, 1, g_tx_rate_500kbps);
	if (wire_len > 0U)
		pcap_inject(pcap, wire, wire_len);
}

static bool sync_bootstrap_window_active(const radio_sync_t *sync)
{
	return sync != NULL &&
		sync->bootstrap_window_us > 0 &&
		sync->bootstrap_period > 0 &&
		sync->superframe_seq != 0 &&
		(sync->superframe_seq % sync->bootstrap_period) == 0;
}

static int64_t sync_bootstrap_announce_us(const radio_sync_t *sync)
{
	uint32_t hash;
	int64_t window_start;
	int64_t headroom;
	int64_t tailroom;
	int64_t usable;

	if (!sync_bootstrap_window_active(sync) || sync->bootstrap_window_us == 0)
		return -1;

	window_start = sync->next_superframe_us - sync->bootstrap_window_us;
	headroom = sync->bootstrap_window_us > 1200 ? 200 : 0;
	tailroom = sync->bootstrap_window_us > 1200 ? 1000 : 1;
	usable = sync->bootstrap_window_us - headroom - tailroom;
	if (usable <= 0)
		usable = 1;
	hash = ((uint32_t)sync->own_node_id * 2654435761u) ^ sync->superframe_seq;
	return window_start + headroom + (int64_t)(hash % (uint32_t)usable);
}

static void sleep_until(int64_t target_us)
{
	int64_t remain = target_us - now_us();
	if (remain > 0)
		usleep((useconds_t)remain);
}

static void master_cycle(radio_sync_t *sync,
			 radio_tx_scheduler_t *sched,
			 radio_rx_dispatcher_t *rxd,
			 pcap_t *pcap,
			 const uint8_t own_mac[6],
			 const uint8_t *default_dst,
			 const radio_route_table_t *rt,
			 radio_stats_t *stats,
			 uint32_t ack_timeout_us,
			 uint32_t ack_max_retry)
{
	int64_t t_now = now_us();
	int64_t superframe_deadline;

	radio_sync_update_slot_map(sync, t_now);
	sync_inject_beacon(sync, pcap, own_mac, t_now);
	superframe_deadline = t_now + sync->superframe_period_us;

	/* DL slot: send our data */
	int64_t dl_deadline = now_us() + sync->dl_duration_us;

	/* Flush CTRL first */
	for (;;) {
		const radio_tx_slot_t *slot = radio_tx_peek(sched, RADIO_PRIO_CTRL);
		if (slot == NULL)
			break;
		uint8_t prio;
		slot = radio_tx_dequeue(sched, &prio);
		if (slot == NULL)
			break;
		radio_stats_add_tx_airtime(stats,
			tx_inject_slot(slot, pcap, own_mac, default_dst, rxd, rt));
		radio_stats_add_tx_packet(stats, prio);
	}

	/* Then P1/P2/P3 */
	while (now_us() < dl_deadline) {
		uint8_t prio;
		const radio_tx_slot_t *slot = radio_tx_dequeue(sched, &prio);
		if (slot == NULL)
			break;
		radio_stats_add_tx_airtime(stats,
			tx_inject_slot(slot, pcap, own_mac, default_dst, rxd, rt));
		radio_stats_add_tx_packet(stats, prio);
	}
	sleep_until(dl_deadline);

	/* Guard */
	usleep(sync->guard_us);

	/* UL slots: receive from each slave */
	for (uint8_t i = 0; i < sync->num_slots; i++) {
		int64_t ul_deadline = now_us() + sync->ul_slot_us;
		radio_rx_slot(rxd, pcap, own_mac, ul_deadline);
		usleep(sync->guard_us);
	}

	/* Bootstrap/contention window.
	 *
	 * On configured superframes the master extends the frame with an advertised
	 * RX-only window where unslotted slaves may send DELAY_REQ. Slotted slaves
	 * account for the same extension in next_superframe_us, so the beacon cadence
	 * stays self-consistent across the whole network. */
	if (sync_bootstrap_window_active(sync)) {
		int64_t join_deadline = now_us() + sync->bootstrap_window_us;
		radio_rx_slot(rxd, pcap, own_mac, join_deadline);
	}

	radio_async_tick(rxd, pcap, ack_timeout_us, ack_max_retry);

	if (superframe_deadline > now_us())
		sleep_until(superframe_deadline);
}

static void slave_cycle(radio_sync_t *sync,
			radio_tx_scheduler_t *sched,
			radio_rx_dispatcher_t *rxd,
			pcap_t *pcap,
			const uint8_t own_mac[6],
			const uint8_t *default_dst,
			const radio_route_table_t *rt,
			radio_stats_t *stats,
			uint32_t ack_timeout_us,
			uint32_t ack_max_retry)
{
	/* Wait for the next beacon near the predicted superframe boundary.
	 *
	 * On a miss, do NOT reset phase to now. radio_sync_on_beacon_timeout()
	 * advances predicted_anchor_us by one whole superframe, preserving slot
	 * geometry during bounded holdover. */
	int64_t now = now_us();
	int64_t anchor_before = sync->predicted_anchor_us;
	uint32_t seq_before = sync->superframe_seq;
	int64_t sync_deadline = (sync->next_superframe_us > now)
		? sync->next_superframe_us + SYNC_BEACON_SLACK_US
		: now + sync->superframe_period_us + SYNC_BEACON_SLACK_US;
	radio_rx_slot_until_sync(rxd, pcap, own_mac, sync_deadline);

	if (!radio_sync_is_synced(sync))
		return;
	if (sync->superframe_seq == seq_before &&
	    sync->predicted_anchor_us == anchor_before)
		radio_sync_on_beacon_timeout(sync, now_us());
	if (!radio_sync_is_synced(sync))
		return;

	radio_sync_compute_timing(sync, now_us());

	/* DL phase: receive master's data */
	if (sync->dl_end_us > now_us())
		radio_rx_slot(rxd, pcap, own_mac, sync->dl_end_us);

	/* My UL slot */
	if (sync->my_slot_index != 0xFF && radio_sync_should_transmit_ul(sync)) {
		sleep_until(sync->my_ul_start_us);

		/* DELAY_REQ first */
		sync_inject_delay_req(sync, pcap, own_mac, now_us());

		int64_t ul_deadline = sync->my_ul_end_us;
		bool sent_data = false;

		/* Airtime-bounded UL send.
		 *
		 * A 1440-byte video fragment is ~2.2 ms on air at 6 Mbps. pcap_inject only
		 * queues a frame into the driver — the airtime elapses later on the wire —
		 * so the old `while (now < ul_deadline)` loop kept dequeuing and injecting
		 * several fragments that then transmitted far past the UL slot, straight
		 * into the master's beacon/DL window where the master is transmitting and
		 * cannot receive (half duplex). That silently dropped most video and made
		 * every multi-fragment keyframe impossible to reassemble (kf_rx=0). Bound
		 * the slot by ESTIMATED AIRTIME instead, dequeuing (highest priority first)
		 * only while the next frame still fits, so nothing overruns the slot. */
		uint64_t air_budget = (sync->ul_slot_us > sync->guard_us)
			? (uint64_t)(sync->ul_slot_us - sync->guard_us)
			: (uint64_t)sync->ul_slot_us;
		/* The DELAY_REQ injected just above also consumes slot airtime. */
		uint64_t air_used = estimate_payload_airtime_us(DELAY_REQ_FRAME_SIZE);
		while (now_us() < ul_deadline) {
			uint8_t prio;
			const radio_tx_slot_t *next = tx_peek_next(sched, &prio);
			if (next == NULL)
				break;
			uint64_t air = estimate_payload_airtime_us(next->len);
			if (air_used + air > air_budget)
				break;  /* would overrun the slot; leave it for next superframe */
			const radio_tx_slot_t *slot = radio_tx_dequeue(sched, &prio);
			if (slot == NULL)
				break;
			radio_stats_add_tx_airtime(stats,
				tx_inject_slot(slot, pcap, own_mac, default_dst, rxd, rt));
			radio_stats_add_tx_packet(stats, prio);
			air_used += air;
			sent_data = true;
		}

		if (!sent_data)
			sync_inject_null_frame(pcap, own_mac);
	} else if (sync->my_slot_index == 0xFF && radio_sync_should_transmit_ul(sync)) {
		/* Bootstrap: synced but the master has not assigned us a UL slot yet.
		 * The master only learns about a slave from its DELAY_REQ, but the normal
		 * path above only sends DELAY_REQ once we already HAVE a slot — a
		 * chicken-and-egg that would otherwise keep a fresh slave invisible
		 * forever. Announce only inside the master's advertised bootstrap window;
		 * a seq-dependent hash spreads multiple joiners across that window so they
		 * do not keep colliding on every retry. */
		int64_t announce_us = sync_bootstrap_announce_us(sync);
		if (announce_us > now_us() &&
		    announce_us < sync->next_superframe_us) {
			sleep_until(announce_us);
			sync_inject_delay_req(sync, pcap, own_mac, now_us());
		}
	}

	/* Listen until end of superframe */
	if (sync->next_superframe_us > now_us())
		radio_rx_slot(rxd, pcap, own_mac, sync->next_superframe_us);

	radio_async_tick(rxd, pcap, ack_timeout_us, ack_max_retry);
}

static void candidate_cycle(radio_sync_t *sync,
			    radio_rx_dispatcher_t *rxd,
			    pcap_t *pcap,
			    const uint8_t own_mac[6])
{
	int64_t rx_deadline = now_us() + 5000;
	radio_rx_slot(rxd, pcap, own_mac, rx_deadline);
	(void)sync;
}
#endif /* ULAMA_WITH_UNOW */

/* ---- Main ---- */

/* ---- System clock sanity check ----
 * LuckFox has no battery-backed RTC — on cold boot the clock starts
 * from epoch (year 2000 or 1970). This makes log timestamps useless.
 * If system time is behind build time, set it to build time so that
 * logs are at least approximately correct. */
static void fix_system_clock(void)
{
	struct tm build_tm = {0};

	if (sscanf(ULAMA_BUILD_DATE, "%d-%d-%d %d:%d:%d",
		   &build_tm.tm_year, &build_tm.tm_mon, &build_tm.tm_mday,
		   &build_tm.tm_hour, &build_tm.tm_min, &build_tm.tm_sec) != 6)
		return;

	build_tm.tm_year -= 1900;
	build_tm.tm_mon -= 1;
	build_tm.tm_isdst = -1;

	time_t build_epoch = mktime(&build_tm);
	if (build_epoch <= 0)
		return;

	time_t now = time(NULL);
	if (now >= build_epoch)
		return;

	struct timeval tv = { .tv_sec = build_epoch, .tv_usec = 0 };
	if (settimeofday(&tv, NULL) == 0) {
		char buf[64];
		strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S",
			 localtime(&build_epoch));
		fprintf(stderr, "radiod: system clock was behind, set to "
			"build time %s\n", buf);
	}
}

int main(int argc, char **argv)
{
	radiod_config_t cfg;
	radio_ipc_server_t ipc;
	radio_tx_scheduler_t sched;
	radio_rx_dispatcher_t rxd;
	radio_watchdog_t wd;
	radio_stats_t stats;
	radio_route_table_t routes;

	config_defaults(&cfg);

	static const struct option long_opts[] = {
		{"iface",    required_argument, NULL, 'i'},
		{"node-id",  required_argument, NULL, 'n'},
		{"dst-mac",  required_argument, NULL, 'd'},
		{"tx-rate-mbps", required_argument, NULL, 1000},
		{"socket",   required_argument, NULL, 's'},
		{"tx-slot",  required_argument, NULL, 'T'},
		{"rx-slot",  required_argument, NULL, 'R'},
		{"relay",    no_argument,       NULL, 'r'},
		{"sync",     no_argument,       NULL, 'S'},
		{"dl-us",    required_argument, NULL, 'D'},
		{"ul-us",    required_argument, NULL, 'U'},
		{"guard-us", required_argument, NULL, 'G'},
		{"no-sync",  no_argument,       NULL, 1001},
		{"version",  no_argument,       NULL, 'V'},
		{"verbose",  no_argument,       NULL, 'v'},
		{"help",     no_argument,       NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "i:n:d:s:T:R:rSD:U:G:Vvh", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'i': cfg.iface = optarg; break;
		case 'n': cfg.node_id = (uint8_t)atoi(optarg); break;
		case 'd':
			if (!parse_mac(optarg, cfg.dst_mac)) {
				fprintf(stderr, "radiod: invalid MAC: %s\n", optarg);
				return 1;
			}
			cfg.has_dst_mac = true;
			break;
		case 1000:
			if (!parse_tx_rate_mbps(optarg, &cfg.tx_rate_500kbps)) {
				fprintf(stderr, "radiod: invalid --tx-rate-mbps value: %s\n", optarg);
				return 1;
			}
			break;
		case 's': cfg.sock_path = optarg; break;
		case 'T': cfg.tx_slot_size = (uint32_t)atoi(optarg); break;
		case 'R': cfg.rx_slot_us = (uint32_t)atoi(optarg); break;
		case 'r': cfg.relay = true; break;
		case 'S': cfg.sync_enabled = true; break;
		case 'D': cfg.sync_dl_us = (uint16_t)atoi(optarg); break;
		case 'U': cfg.sync_ul_us = (uint16_t)atoi(optarg); break;
		case 'G': cfg.sync_guard_us = (uint16_t)atoi(optarg); break;
		case 1001: cfg.sync_enabled = false; break;
		case 'V':
			fprintf(stderr, "radiod: build #%d (%s@%s) %s\n",
				ULAMA_BUILD_NUMBER, ULAMA_GIT_BRANCH, ULAMA_GIT_HASH, ULAMA_BUILD_DATE);
			return 0;
		case 'v': cfg.verbose = true; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	fix_system_clock();
	g_tx_rate_500kbps = cfg.tx_rate_500kbps;

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

#if !ULAMA_WITH_UNOW
	fprintf(stderr, "radiod: built without UNOW support, cannot run\n");
	return 1;
#else

	/* ---- Initialize IPC FIRST (before interface wait) ----
	 * Clients (ulamad, vcpd) start shortly after radiod and send
	 * registration messages. The socket must exist before they try
	 * to connect — messages accumulate in the kernel buffer while
	 * we wait for the radio interface. */

	if (radio_ipc_server_init(&ipc, cfg.sock_path) < 0) {
		fprintf(stderr, "radiod: IPC init failed (%s): %s\n",
			cfg.sock_path, strerror(errno));
		return 1;
	}
	fprintf(stderr, "radiod: build #%d (%s@%s) %s\n",
		ULAMA_BUILD_NUMBER, ULAMA_GIT_BRANCH, ULAMA_GIT_HASH, ULAMA_BUILD_DATE);
	fprintf(stderr, "radiod: IPC listening on %s\n", cfg.sock_path);

	/* ---- Wait for radio interface ---- */

	unow_iface_info_t iface_info;
	pcap_t *pcap_handle = NULL;
	int datalink = 0;
	char error_buf[256] = {0};

	fprintf(stderr, "radiod: waiting for interface %s...\n", cfg.iface);

	for (int attempt = 1; g_running; attempt++) {
		memset(error_buf, 0, sizeof(error_buf));

		/* Drain IPC while waiting — accept client registrations early */
		radio_ipc_drain(&ipc, NULL, NULL);

		if (unow_iface_query(cfg.iface, &iface_info,
				     error_buf, sizeof(error_buf)) != 0) {
			if (attempt == 1 || attempt % 10 == 0)
				fprintf(stderr, "radiod: [%d] %s not found, retrying...\n",
					attempt, cfg.iface);
			sleep(1);
			continue;
		}

		if (unow_iface_open_pcap(cfg.iface, &pcap_handle, &datalink,
					 error_buf, sizeof(error_buf)) != 0) {
			if (attempt == 1 || attempt % 10 == 0)
				fprintf(stderr, "radiod: [%d] pcap open failed: %s\n",
					attempt, error_buf);
			sleep(1);
			continue;
		}

		if (datalink != DLT_IEEE802_11_RADIO) {
			fprintf(stderr, "radiod: [%d] %s not in monitor mode (datalink=%d), retrying...\n",
				attempt, cfg.iface, datalink);
			unow_iface_close_pcap(&pcap_handle);
			sleep(2);
			continue;
		}

		break;
	}

	if (!g_running) {
		fprintf(stderr, "radiod: interrupted while waiting for interface\n");
		if (pcap_handle != NULL)
			unow_iface_close_pcap(&pcap_handle);
		radio_ipc_server_close(&ipc);
		return 0;
	}

	fprintf(stderr,
		"radiod: radio ready iface=%s mac=%02x:%02x:%02x:%02x:%02x:%02x "
		"node_id=%u tx_slot=%u rx_slot=%u µs tx_rate=%u.%u Mbps relay=%s\n",
		cfg.iface,
		iface_info.mac[0], iface_info.mac[1], iface_info.mac[2],
		iface_info.mac[3], iface_info.mac[4], iface_info.mac[5],
		cfg.node_id, cfg.tx_slot_size, cfg.rx_slot_us,
		cfg.tx_rate_500kbps / 2U,
		(cfg.tx_rate_500kbps & 1U) ? 5U : 0U,
		cfg.relay ? "on" : "off");

	/* ---- Initialize subsystems ---- */

	int64_t ts = now_us();

	radio_tx_scheduler_init(&sched);
	radio_rx_dispatcher_init(&rxd, &ipc);
	rxd.async_max_retry = (uint8_t)(cfg.ack_max_retry > 0U ? cfg.ack_max_retry : 1U);
	radio_route_table_init(&routes);
	radio_watchdog_init(&wd, ts);
	radio_stats_init(&stats, ts);

	if (cfg.relay) {
		radio_rx_dispatcher_enable_relay(&rxd, cfg.node_id,
						 &sched, &routes);
		fprintf(stderr, "radiod: mesh relay enabled for node %u\n",
			cfg.node_id);
	} else {
		/* Even without relay, set own_node_id for watchdog filtering */
		rxd.own_node_id = cfg.node_id;
	}

	radio_sync_t sync_engine;
	if (cfg.sync_enabled) {
		radio_sync_init(&sync_engine, cfg.node_id,
				cfg.sync_dl_us, cfg.sync_ul_us,
				cfg.sync_guard_us);
		radio_rx_dispatcher_set_sync(&rxd, &sync_engine);
		/* Current field topology is 2 nodes (host master + one drone slave).
		 * SYNC relaying is unnecessary there and can poison clock sync if a slave
		 * rebroadcasts a beacon with a mistranslated origin timestamp. */
		radio_rx_dispatcher_set_sync_relay_enabled(&rxd, false);
		fprintf(stderr,
			"radiod: SYNC protocol enabled, node_id=%u "
			"dl=%u ul=%u guard=%u\n",
			cfg.node_id, cfg.sync_dl_us,
			cfg.sync_ul_us, cfg.sync_guard_us);
	}

	ipc_tx_ctx_t ipc_ctx = {
		.sched = &sched,
		.wd = &wd,
		.verbose = cfg.verbose,
	};

	const uint8_t broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	const uint8_t *default_dst = cfg.has_dst_mac ? cfg.dst_mac : broadcast_mac;

	/* ================================================================
	 * TDMA Main Loop
	 *
	 * Outer loop handles pcap recovery after USB disconnect.
	 * Inner loop runs TDMA scheduling while pcap is healthy.
	 * IPC stays alive throughout — clients keep their connections.
	 * ================================================================ */

	uint32_t pcap_error_count = 0;

	while (g_running) {

		/* ---- PCAP recovery: reopen after USB disconnect ---- */

		if (pcap_handle == NULL) {
			fprintf(stderr, "radiod: radio lost, waiting for %s...\n",
				cfg.iface);

			while (g_running) {
				/* Keep draining IPC during recovery so clients
				 * don't get EAGAIN and TX queues don't stall */
				radio_ipc_drain(&ipc, on_ipc_tx_request, &ipc_ctx);

				memset(error_buf, 0, sizeof(error_buf));

				if (unow_iface_query(cfg.iface, &iface_info,
						     error_buf, sizeof(error_buf)) != 0) {
					sleep(1);
					continue;
				}
				if (unow_iface_open_pcap(cfg.iface, &pcap_handle,
							 &datalink, error_buf,
							 sizeof(error_buf)) != 0) {
					sleep(1);
					continue;
				}
				if (datalink != DLT_IEEE802_11_RADIO) {
					unow_iface_close_pcap(&pcap_handle);
					sleep(2);
					continue;
				}

				/* Radio restored — let driver settle before
				 * entering TDMA loop */
				pcap_error_count = 0;
				fprintf(stderr,
					"radiod: radio restored iface=%s mac=%02x:%02x:%02x:%02x:%02x:%02x, settling...\n",
					cfg.iface,
					iface_info.mac[0], iface_info.mac[1],
					iface_info.mac[2], iface_info.mac[3],
					iface_info.mac[4], iface_info.mac[5]);
				sleep(2);
				radio_watchdog_init(&wd, now_us());
				break;
			}
			if (!g_running)
				break;
		}

		/* ---- TDMA cycle ---- */

		int64_t cycle_start = now_us();
		bool wd_emergency = false;

		if (cfg.sync_enabled) {
			/* SYNC-based TDMA cycle */
			radio_role_t role = radio_sync_tick(
				&sync_engine, now_us());

			switch (role) {
			case RADIO_ROLE_MASTER:
				master_cycle(&sync_engine, &sched, &rxd,
					     pcap_handle, iface_info.mac,
					     default_dst, &routes, &stats,
					     cfg.ack_timeout_us, cfg.ack_max_retry);
				break;
			case RADIO_ROLE_SLAVE:
				slave_cycle(&sync_engine, &sched, &rxd,
					    pcap_handle, iface_info.mac,
					    default_dst, &routes, &stats,
					    cfg.ack_timeout_us, cfg.ack_max_retry);
				/* SYNC beacon = implicit heartbeat */
				if (radio_sync_is_synced(&sync_engine))
					radio_watchdog_feed(&wd, now_us());
				break;
			case RADIO_ROLE_CANDIDATE:
				candidate_cycle(&sync_engine, &rxd,
						pcap_handle, iface_info.mac);
				break;
			}
		} else {
			/* Standalone TDMA cycle (no SYNC) */
			int64_t tx_start, tx_end, rx_end;

			/* 1. Flush all CTRL packets (P0, no rate limit) */

			tx_start = now_us();
			for (;;) {
				const radio_tx_slot_t *slot = radio_tx_peek(&sched, RADIO_PRIO_CTRL);
				if (slot == NULL)
					break;

				uint8_t prio;
				slot = radio_tx_dequeue(&sched, &prio);
				if (slot == NULL)
					break;

				radio_stats_add_tx_airtime(&stats,
					tx_inject_slot(slot, pcap_handle, iface_info.mac,
					       default_dst, &rxd, &routes));
				radio_stats_add_tx_packet(&stats, prio);
			}

			/* 2. TX slot: up to N packets from P1/P2/P3 */

			for (uint32_t i = 0; i < cfg.tx_slot_size; i++) {
				uint8_t prio;
				const radio_tx_slot_t *slot = radio_tx_dequeue(&sched, &prio);
				if (slot == NULL)
					break;

				radio_stats_add_tx_airtime(&stats,
					tx_inject_slot(slot, pcap_handle, iface_info.mac,
					       default_dst, &rxd, &routes));
				radio_stats_add_tx_packet(&stats, prio);

				/*
				 * Standalone mode used to blast several video fragments back-to-back
				 * and only then open the RX slot. Reliable keyframe fragments need
				 * an ACK from the ground station, but the ACK often collided with the
				 * remaining burst. Give the peer a short gap after each injected data
				 * frame, and for reliable packets immediately open a brief RX window so
				 * the ACK can land before we send the next fragment.
				 */
				if (slot->reliability) {
					int64_t ack_rx_deadline = now_us() + (int64_t)(cfg.ack_timeout_us / 2U);
					radio_rx_slot(&rxd, pcap_handle, iface_info.mac, ack_rx_deadline);
				}
				if (i + 1U < cfg.tx_slot_size)
					usleep(TX_PACE_US);
			}

			tx_end = now_us();
			radio_stats_add_tx_time(&stats, (uint64_t)(tx_end - tx_start));

			/* 3. RX slot */

			int64_t rx_deadline = now_us() + (int64_t)cfg.rx_slot_us;
			radio_rx_slot(&rxd, pcap_handle, iface_info.mac, rx_deadline);

			rx_end = now_us();
			radio_stats_add_rx_time(&stats, (uint64_t)(rx_end - tx_end));

			/* 4. Async retry tick */

			radio_async_tick(&rxd, pcap_handle,
					 cfg.ack_timeout_us, cfg.ack_max_retry);

			/* Watchdog feed (standalone) */
			if (rxd.ctrl_for_us > 0)
				radio_watchdog_feed(&wd, now_us());
		}

		/* ---- Common: IPC, watchdog, stats, recovery ---- */

		radio_ipc_drain(&ipc, on_ipc_tx_request, &ipc_ctx);

		radio_link_state_t link = radio_watchdog_tick(&wd, now_us(), &wd_emergency);

		if (wd_emergency) {
			ulama_frame_view_t beacon = {
				.src_node = cfg.node_id,
				.dst_node = 0xFF,
				.flags = 0,
				.traffic_class = ULAMA_CLASS_CTRL,
				.seq = 0,
				.ttl = ULAMA_FRAME_ONE_HOP_TTL,
				.payload = NULL,
				.payload_len = 0,
			};
			uint8_t beacon_buf[ULAMA_FRAME_HEADER_SIZE];
			size_t beacon_len = 0;
			if (ulama_frame_pack(&beacon, beacon_buf, sizeof(beacon_buf), &beacon_len)) {
				uint8_t wire[sizeof(struct unow_radiotap_tx_header) +
					     sizeof(struct unow_dot11_mgmt_header) +
					     sizeof(struct unow_action_vendor_header) +
					     ULAMA_FRAME_HEADER_SIZE];
				size_t wire_len = unow_build_action_frame(
					wire, sizeof(wire),
					iface_info.mac, broadcast_mac,
					beacon_buf, beacon_len,
					g_tx_rate_500kbps);
				if (wire_len > 0U)
					pcap_inject(pcap_handle, wire, wire_len);
			}
		}

		(void)link;

		radio_route_expire(&routes, now_us());

		radio_stats_add_cycle(&stats);
		if (cfg.sync_enabled)
			radio_stats_update_sync(&stats, &sync_engine);
		radio_stats_report(&stats, now_us(), &sched, &rxd, &routes, &ipc);

		if (pcap_handle != NULL && rxd.stats.rx_pcap_error > 0) {
			pcap_error_count += rxd.stats.rx_pcap_error;
			rxd.stats.rx_pcap_error = 0;
		} else {
			pcap_error_count = 0;
		}

		if (pcap_handle != NULL && pcap_error_count > 3) {
			fprintf(stderr,
				"radiod: pcap error (interface lost), entering recovery\n");
			unow_iface_close_pcap(&pcap_handle);
			pcap_handle = NULL;
			pcap_error_count = 0;
			continue;
		}

		int64_t cycle_elapsed = now_us() - cycle_start;
		if (cycle_elapsed < 500)
			usleep(100);
	}

	fprintf(stderr, "radiod: shutting down\n");
	radio_ipc_server_close(&ipc);
	if (pcap_handle != NULL)
		unow_iface_close_pcap(&pcap_handle);
	return 0;

#endif /* ULAMA_WITH_UNOW */
}
