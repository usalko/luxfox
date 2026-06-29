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

#define TX_SLOT_SIZE          4
#define RX_SLOT_US            2000
#define ACK_TIMEOUT_US        8000
#define ACK_MAX_RETRY         2

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
	cfg->tx_slot_size = TX_SLOT_SIZE;
	cfg->rx_slot_us = RX_SLOT_US;
	cfg->ack_timeout_us = ACK_TIMEOUT_US;
	cfg->ack_max_retry = ACK_MAX_RETRY;
	cfg->sync_dl_us = 2000;
	cfg->sync_ul_us = 2000;
	cfg->sync_guard_us = 300;
	cfg->sync_enabled = false;
}

static int64_t now_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

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
static void tx_inject_slot(const radio_tx_slot_t *slot,
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
			UNOW_TX_RATE_1MBPS,
			UNOW_VENDOR_SUBTYPE_DATA_SEQ);

		if (wire_len > 0U) {
			pcap_inject(pcap, wire, wire_len);
			radio_async_store(rxd, wire, wire_len, seq);
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
			UNOW_TX_RATE_1MBPS);

		if (wire_len > 0U)
			pcap_inject(pcap, wire, wire_len);
	}
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
		"  -s, --socket PATH     IPC socket path (default: %s)\n"
		"  -T, --tx-slot N       Max TX packets per slot (default: %u)\n"
		"  -R, --rx-slot US      RX slot duration in µs (default: %u)\n"
		"      --relay           Enable mesh relay mode\n"
		"  -S, --sync            Enable SYNC protocol (TDMA + election)\n"
		"  -D, --dl-us US        SYNC DL slot duration (default: 2000)\n"
		"  -U, --ul-us US        SYNC UL slot duration (default: 2000)\n"
		"  -G, --guard-us US     SYNC guard interval (default: 300)\n"
		"  -v, --verbose         Verbose output\n"
		"  -h, --help            Show this help\n",
		prog, RADIO_IPC_SOCK_PATH, TX_SLOT_SIZE, RX_SLOT_US);
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

/* ---- SYNC cycle helpers ---- */

#if ULAMA_WITH_UNOW
static void sync_inject_beacon(radio_sync_t *sync,
			       pcap_t *pcap,
			       const uint8_t own_mac[6],
			       int64_t ts)
{
	sync_frame_t beacon;
	radio_sync_build_beacon(sync, &beacon, ts);

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
		UNOW_TX_RATE_1MBPS, UNOW_VENDOR_SUBTYPE_SYNC);
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
		UNOW_TX_RATE_1MBPS, UNOW_VENDOR_SUBTYPE_DELAY_REQ);
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
		&null_byte, 1, UNOW_TX_RATE_1MBPS);
	if (wire_len > 0U)
		pcap_inject(pcap, wire, wire_len);
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
			 radio_stats_t *stats)
{
	int64_t t_now = now_us();

	radio_sync_update_slot_map(sync, t_now);
	sync_inject_beacon(sync, pcap, own_mac, t_now);

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
		tx_inject_slot(slot, pcap, own_mac, default_dst, rxd, rt);
		radio_stats_add_tx_packet(stats, prio);
	}

	/* Then P1/P2/P3 */
	while (now_us() < dl_deadline) {
		uint8_t prio;
		const radio_tx_slot_t *slot = radio_tx_dequeue(sched, &prio);
		if (slot == NULL)
			break;
		tx_inject_slot(slot, pcap, own_mac, default_dst, rxd, rt);
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

	radio_async_tick(rxd, pcap, ACK_TIMEOUT_US, ACK_MAX_RETRY);
}

static void slave_cycle(radio_sync_t *sync,
			radio_tx_scheduler_t *sched,
			radio_rx_dispatcher_t *rxd,
			pcap_t *pcap,
			const uint8_t own_mac[6],
			const uint8_t *default_dst,
			const radio_route_table_t *rt,
			radio_stats_t *stats)
{
	/* Wait for SYNC beacon */
	int64_t sync_deadline = sync->next_superframe_us > 0
		? sync->next_superframe_us + 2000
		: now_us() + SYNC_BEACON_INTERVAL_US + 2000;
	radio_rx_slot(rxd, pcap, own_mac, sync_deadline);

	if (!radio_sync_is_synced(sync))
		return;

	radio_sync_compute_timing(sync, now_us());

	/* DL phase: receive master's data */
	if (sync->dl_end_us > now_us())
		radio_rx_slot(rxd, pcap, own_mac, sync->dl_end_us);

	/* My UL slot */
	if (sync->my_slot_index != 0xFF) {
		sleep_until(sync->my_ul_start_us);

		/* DELAY_REQ first */
		sync_inject_delay_req(sync, pcap, own_mac, now_us());

		/* Send data */
		int64_t ul_deadline = sync->my_ul_end_us;
		bool sent_data = false;

		/* Flush CTRL first */
		for (;;) {
			if (now_us() >= ul_deadline)
				break;
			const radio_tx_slot_t *slot = radio_tx_peek(sched, RADIO_PRIO_CTRL);
			if (slot == NULL)
				break;
			uint8_t prio;
			slot = radio_tx_dequeue(sched, &prio);
			if (slot == NULL)
				break;
			tx_inject_slot(slot, pcap, own_mac, default_dst, rxd, rt);
			radio_stats_add_tx_packet(stats, prio);
			sent_data = true;
		}

		while (now_us() < ul_deadline) {
			uint8_t prio;
			const radio_tx_slot_t *slot = radio_tx_dequeue(sched, &prio);
			if (slot == NULL)
				break;
			tx_inject_slot(slot, pcap, own_mac, default_dst, rxd, rt);
			radio_stats_add_tx_packet(stats, prio);
			sent_data = true;
		}

		if (!sent_data)
			sync_inject_null_frame(pcap, own_mac);
	}

	/* Listen until end of superframe */
	if (sync->next_superframe_us > now_us())
		radio_rx_slot(rxd, pcap, own_mac, sync->next_superframe_us);
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
		{"socket",   required_argument, NULL, 's'},
		{"tx-slot",  required_argument, NULL, 'T'},
		{"rx-slot",  required_argument, NULL, 'R'},
		{"relay",    no_argument,       NULL, 'r'},
		{"sync",     no_argument,       NULL, 'S'},
		{"dl-us",    required_argument, NULL, 'D'},
		{"ul-us",    required_argument, NULL, 'U'},
		{"guard-us", required_argument, NULL, 'G'},
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
		case 's': cfg.sock_path = optarg; break;
		case 'T': cfg.tx_slot_size = (uint32_t)atoi(optarg); break;
		case 'R': cfg.rx_slot_us = (uint32_t)atoi(optarg); break;
		case 'r': cfg.relay = true; break;
		case 'S': cfg.sync_enabled = true; break;
		case 'D': cfg.sync_dl_us = (uint16_t)atoi(optarg); break;
		case 'U': cfg.sync_ul_us = (uint16_t)atoi(optarg); break;
		case 'G': cfg.sync_guard_us = (uint16_t)atoi(optarg); break;
		case 'V':
			fprintf(stderr, "radiod: build #%d (%s@%s) %s\n",
				ULAMA_BUILD_NUMBER, ULAMA_GIT_BRANCH, ULAMA_GIT_HASH, ULAMA_BUILD_DATE);
			return 0;
		case 'v': cfg.verbose = true; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

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
		"node_id=%u tx_slot=%u rx_slot=%u µs relay=%s\n",
		cfg.iface,
		iface_info.mac[0], iface_info.mac[1], iface_info.mac[2],
		iface_info.mac[3], iface_info.mac[4], iface_info.mac[5],
		cfg.node_id, cfg.tx_slot_size, cfg.rx_slot_us,
		cfg.relay ? "on" : "off");

	/* ---- Initialize subsystems ---- */

	int64_t ts = now_us();

	radio_tx_scheduler_init(&sched);
	radio_rx_dispatcher_init(&rxd, &ipc);
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
					     default_dst, &routes, &stats);
				break;
			case RADIO_ROLE_SLAVE:
				slave_cycle(&sync_engine, &sched, &rxd,
					    pcap_handle, iface_info.mac,
					    default_dst, &routes, &stats);
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

				tx_inject_slot(slot, pcap_handle, iface_info.mac,
					       default_dst, &rxd, &routes);
				radio_stats_add_tx_packet(&stats, prio);
			}

			/* 2. TX slot: up to N packets from P1/P2/P3 */

			for (uint32_t i = 0; i < cfg.tx_slot_size; i++) {
				uint8_t prio;
				const radio_tx_slot_t *slot = radio_tx_dequeue(&sched, &prio);
				if (slot == NULL)
					break;

				tx_inject_slot(slot, pcap_handle, iface_info.mac,
					       default_dst, &rxd, &routes);
				radio_stats_add_tx_packet(&stats, prio);
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
					UNOW_TX_RATE_1MBPS);
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
