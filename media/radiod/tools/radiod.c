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

#include "radiod/ipc.h"
#include "radiod/route_table.h"
#include "radiod/rx_dispatcher.h"
#include "radiod/stats.h"
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
} radiod_config_t;

static void config_defaults(radiod_config_t *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->iface = "mon0";
	cfg->sock_path = RADIO_IPC_SOCK_PATH;
	cfg->node_id = 1;
	cfg->tx_slot_size = TX_SLOT_SIZE;
	cfg->rx_slot_us = RX_SLOT_US;
	cfg->ack_timeout_us = ACK_TIMEOUT_US;
	cfg->ack_max_retry = ACK_MAX_RETRY;
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
		{"verbose",  no_argument,       NULL, 'v'},
		{"help",     no_argument,       NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "i:n:d:s:T:R:rvh", long_opts, NULL)) != -1) {
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

	/* ---- Initialize pcap / radio ---- */

	unow_iface_info_t iface_info;
	pcap_t *pcap_handle = NULL;
	int datalink = 0;
	char error_buf[256] = {0};

	if (unow_iface_query(cfg.iface, &iface_info, error_buf, sizeof(error_buf)) != 0) {
		fprintf(stderr, "radiod: interface probe failed for %s: %s\n",
			cfg.iface, error_buf);
		return 1;
	}

	if (unow_iface_open_pcap(cfg.iface, &pcap_handle, &datalink,
				 error_buf, sizeof(error_buf)) != 0) {
		fprintf(stderr, "radiod: pcap open failed for %s: %s\n",
			cfg.iface, error_buf);
		return 1;
	}

	if (datalink != DLT_IEEE802_11_RADIO) {
		fprintf(stderr, "radiod: %s is not in monitor mode (datalink=%d)\n",
			cfg.iface, datalink);
		unow_iface_close_pcap(&pcap_handle);
		return 1;
	}

	fprintf(stderr,
		"radiod: started iface=%s mac=%02x:%02x:%02x:%02x:%02x:%02x "
		"node_id=%u tx_slot=%u rx_slot=%u µs relay=%s\n",
		cfg.iface,
		iface_info.mac[0], iface_info.mac[1], iface_info.mac[2],
		iface_info.mac[3], iface_info.mac[4], iface_info.mac[5],
		cfg.node_id, cfg.tx_slot_size, cfg.rx_slot_us,
		cfg.relay ? "on" : "off");

	/* ---- Initialize IPC ---- */

	if (radio_ipc_server_init(&ipc, cfg.sock_path) < 0) {
		fprintf(stderr, "radiod: IPC init failed (%s): %s\n",
			cfg.sock_path, strerror(errno));
		unow_iface_close_pcap(&pcap_handle);
		return 1;
	}
	fprintf(stderr, "radiod: IPC listening on %s\n", cfg.sock_path);

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

	ipc_tx_ctx_t ipc_ctx = {
		.sched = &sched,
		.wd = &wd,
		.verbose = cfg.verbose,
	};

	const uint8_t broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	const uint8_t *default_dst = cfg.has_dst_mac ? cfg.dst_mac : broadcast_mac;

	/* ================================================================
	 * TDMA Main Loop
	 * ================================================================ */

	while (g_running) {
		int64_t cycle_start = now_us();
		int64_t tx_start, tx_end, rx_end;
		bool wd_emergency = false;

		/* ---- 1. Flush all CTRL packets (P0, no rate limit) ---- */

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

		/* ---- 2. TX slot: up to N packets from P1/P2/P3 ---- */

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

		/* ---- 3. RX slot ---- */

		int64_t rx_deadline = now_us() + (int64_t)cfg.rx_slot_us;
		radio_rx_slot(&rxd, pcap_handle, iface_info.mac, rx_deadline);

		rx_end = now_us();
		radio_stats_add_rx_time(&stats, (uint64_t)(rx_end - tx_end));

		/* ---- 4. Async retry tick ---- */

		radio_async_tick(&rxd, pcap_handle,
				 cfg.ack_timeout_us, cfg.ack_max_retry);

		/* ---- 5. Drain IPC requests ---- */

		radio_ipc_drain(&ipc, on_ipc_tx_request, &ipc_ctx);

		/* ---- 6. Watchdog: feed only on CTRL addressed to us ---- */

		if (rxd.ctrl_for_us > 0)
			radio_watchdog_feed(&wd, now_us());

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

		/* ---- 7. Route table maintenance ---- */

		radio_route_expire(&routes, now_us());

		/* ---- 8. Stats reporting ---- */

		radio_stats_add_cycle(&stats);
		radio_stats_report(&stats, now_us(), &sched, &rxd, &routes);

		/* Micro-sleep if cycle was too fast */
		int64_t cycle_elapsed = now_us() - cycle_start;
		if (cycle_elapsed < 500)
			usleep(100);
	}

	fprintf(stderr, "radiod: shutting down\n");
	radio_ipc_server_close(&ipc);
	unow_iface_close_pcap(&pcap_handle);
	return 0;

#endif /* ULAMA_WITH_UNOW */
}
