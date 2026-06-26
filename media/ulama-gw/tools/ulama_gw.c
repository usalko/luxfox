#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>

#include "ulama_gw/cascade_frame.h"
#include "ulama_gw/gateway.h"
#include "ulama_gw/fragmentation.h"
#include "ulama_gw/lts_decoder.h"
#include "ulama_gw/lts_fec_dec.h"
#include "ulama/ulama_frame.h"
#include "ulama/transport.h"

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int setup_iface(const char *iface, int channel, bool verbose)
{
	char cmd[256];
	int rc;

	if (verbose)
		fprintf(stderr, "gw: configuring %s (channel %d)\n", iface, channel);

	snprintf(cmd, sizeof(cmd), "ip link set %s down", iface);
	rc = system(cmd);
	if (rc != 0)
		fprintf(stderr, "gw: warning: '%s' returned %d\n", cmd, rc);

	snprintf(cmd, sizeof(cmd), "ip link set %s up", iface);
	rc = system(cmd);
	if (rc != 0) {
		fprintf(stderr, "gw: failed: '%s' returned %d\n", cmd, rc);
		return -1;
	}

	snprintf(cmd, sizeof(cmd), "iw dev %s set channel %d", iface, channel);
	rc = system(cmd);
	if (rc != 0) {
		fprintf(stderr, "gw: failed: '%s' returned %d\n", cmd, rc);
		return -1;
	}

	if (verbose)
		fprintf(stderr, "gw: %s ready (channel %d)\n", iface, channel);
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  --cascade-in  ADDR:PORT  Listen for cascade-core outbound (default 127.0.0.1:5601)\n"
		"  --cascade-out ADDR:PORT  Send to cascade-core inbound     (default 127.0.0.1:5600)\n"
		"  --transport   udp|unow   Radio transport                   (default udp)\n"
		"  --listen      ADDR:PORT  Listen for ULAMA frames (UDP)     (default 0.0.0.0:5000)\n"
		"  --peer        ADDR:PORT  Send ULAMA frames to (UDP)        (default 127.0.0.1:5001)\n"
		"  --iface       NAME       Monitor mode interface (UNOW)     (default mon0)\n"
		"  --channel     N          WiFi channel; auto-configures iface before start (UNOW)\n"
		"  --node        ID         Gateway node ID (1-253)           (default 1)\n"
		"  --dst-mac     MAC        Destination MAC for UNOW TX       (broadcast if omitted)\n"
		"  --gap-tolerance N        Max skipped packets per NAL before drop  (default 2)\n"
		"  --verbose                Enable verbose logging\n"
		"  --help                   Show this help\n",
		prog);
}

#define NAL_ASSEMBLE_MAX (64 * 1024)

typedef struct {
	uint8_t buf[NAL_ASSEMBLE_MAX];
	size_t len;
	uint16_t first_seq;
	uint16_t expect_seq;
	bool active;
	bool skip;
	uint16_t gaps_tolerated;
	uint16_t max_gap_tolerance;
} nal_assembler_t;

typedef struct {
	uint32_t nal_complete;
	uint32_t nal_dropped;
	uint32_t lts_rx;
	uint32_t lts_gaps;
	uint32_t lts_unique;
	uint32_t lts_dup;
	uint16_t lts_seq_min;
	uint16_t lts_seq_max;
	bool lts_seq_valid;
	uint32_t ulama_rx_video;
	uint32_t ulama_rx_telem;
	uint32_t ulama_rx_ctrl;
	uint32_t ctrl_tx;
	uint32_t nack_sent;
	uint64_t video_bytes_out;
	uint64_t last_print_ms;
	int32_t rssi_sum;
	uint32_t rssi_count;
	uint32_t nal_drop_1pkt;
	uint32_t nal_drop_2_5pkt;
	uint32_t nal_drop_6_15pkt;
	uint32_t nal_drop_16plus;
	uint32_t burst_gaps;
	uint32_t single_gaps;
	uint32_t retx_arrived;
	uint32_t fec_recovered;
	uint32_t fec_unrecoverable;
	uint32_t nal_gap_skipped;
} gw_stats_t;

typedef struct {
	gw_config_t gw;
	char listen_addr[64];
	char peer_addr[64];
	char dst_mac_str[32];
	bool verbose;
	int channel;
	uint16_t seq_counter;
	int cascade_rx_fd;
	struct sockaddr_in cascade_tx_addr;
	int cascade_tx_fd;
	ulama_tx_transport_t ulama_tx;
	ulama_rx_transport_t ulama_rx;
	frag_reassembly_ctx_t reassembly;
	lts_decoder_t lts_dec;
	uint16_t lts_video_src;
	uint8_t lts_video_src_node;
	uint64_t last_nack_ms;
	nal_assembler_t nal_asm;
	lts_fec_decoder_t fec_dec;
	gw_stats_t stats;
} app_ctx_t;

static int parse_addr(const char *str, struct sockaddr_in *out)
{
	char buf[64];
	strncpy(buf, str, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	char *colon = strrchr(buf, ':');
	if (!colon)
		return -1;
	*colon = '\0';

	memset(out, 0, sizeof(*out));
	out->sin_family = AF_INET;
	out->sin_port = htons((uint16_t)atoi(colon + 1));
	if (inet_pton(AF_INET, buf, &out->sin_addr) != 1)
		return -1;

	return 0;
}

static int open_udp_listener(const char *addr_str)
{
	struct sockaddr_in sa;
	if (parse_addr(addr_str, &sa) < 0)
		return -1;

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

static int open_udp_sender(const char *addr_str, struct sockaddr_in *dst)
{
	if (parse_addr(addr_str, dst) < 0)
		return -1;

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	return fd;
}

static void handle_cascade_rx(app_ctx_t *ctx)
{
	uint8_t buf[CASCADE_FRAME_HEADER_SIZE + CASCADE_FRAME_MAX_PAYLOAD];
	ssize_t n = recv(ctx->cascade_rx_fd, buf, sizeof(buf), 0);
	if (n <= 0)
		return;

	cascade_frame_view_t cf;
	if (!cascade_frame_unpack(buf, (size_t)n, &cf)) {
		if (ctx->verbose)
			fprintf(stderr, "gw: invalid cascade-frame (%zd bytes)\n", n);
		return;
	}

	if (ctx->verbose) {
		fprintf(stderr, "gw: cascade RX v=%u src=%u dst=%u class=%u payload=%zu\n",
			cf.version, cf.src, cf.dst, cf.traffic_class, cf.payload_len);
	}

	uint8_t ulama_class = gw_class_cascade_to_ulama(cf.traffic_class);
	uint8_t dst_node = gw_addr_u16_to_u8(&ctx->gw, cf.dst);

	if (ulama_class == ULAMA_CLASS_CTRL)
		ctx->stats.ctrl_tx++;

	if (cf.payload_len <= ULAMA_FRAME_MAX_PAYLOAD) {
		ulama_frame_view_t uf = {
			.src_node = ctx->gw.node_id,
			.dst_node = dst_node,
			.flags = ULAMA_FLAG_DUP_ALLOWED,
			.traffic_class = ulama_class,
			.seq = ctx->seq_counter++,
			.frag_idx = 0,
			.frag_total = 1,
			.ttl = ULAMA_FRAME_DEFAULT_TTL,
			.payload = cf.payload,
			.payload_len = cf.payload_len,
		};

		uint8_t frame_buf[ULAMA_FRAME_HEADER_SIZE + ULAMA_FRAME_MAX_PAYLOAD];
		size_t frame_len = 0;
		if (ulama_frame_pack(&uf, frame_buf, sizeof(frame_buf), &frame_len))
			ulama_transport_tx_send(&ctx->ulama_tx, frame_buf, frame_len);
	} else {
		uint8_t frag_payloads[FRAG_MAX_FRAGMENTS][ULAMA_FRAME_MAX_PAYLOAD];
		size_t frag_sizes[FRAG_MAX_FRAGMENTS];
		size_t nfrags = frag_split(cf.payload, cf.payload_len, frag_payloads, frag_sizes, FRAG_MAX_FRAGMENTS);

		uint16_t base_seq = ctx->seq_counter++;
		for (size_t i = 0; i < nfrags; i++) {
			ulama_frame_view_t uf = {
				.src_node = ctx->gw.node_id,
				.dst_node = dst_node,
				.flags = ULAMA_FLAG_FRAGMENT | ((i == nfrags - 1) ? ULAMA_FLAG_LAST_FRAGMENT : 0) | ULAMA_FLAG_DUP_ALLOWED,
				.traffic_class = ulama_class,
				.seq = base_seq,
				.frag_idx = (uint8_t)i,
				.frag_total = (uint8_t)nfrags,
				.ttl = ULAMA_FRAME_DEFAULT_TTL,
				.payload = frag_payloads[i],
				.payload_len = frag_sizes[i],
			};

			uint8_t frame_buf[ULAMA_FRAME_HEADER_SIZE + ULAMA_FRAME_MAX_PAYLOAD];
			size_t frame_len = 0;
			if (ulama_frame_pack(&uf, frame_buf, sizeof(frame_buf), &frame_len))
				ulama_transport_tx_send(&ctx->ulama_tx, frame_buf, frame_len);
		}
	}
}

static void send_cascade_frame(app_ctx_t *ctx, const cascade_frame_view_t *cf)
{
	size_t buf_size = CASCADE_FRAME_HEADER_SIZE + cf->payload_len;
	uint8_t *buf = (uint8_t *)malloc(buf_size);
	if (!buf) return;
	size_t len = 0;
	if (cascade_frame_pack(cf, buf, buf_size, &len)) {
		sendto(ctx->cascade_tx_fd, buf, len, 0,
		       (struct sockaddr *)&ctx->cascade_tx_addr, sizeof(ctx->cascade_tx_addr));
	}
	free(buf);
}

static void flush_nal(app_ctx_t *ctx, uint16_t src_u16)
{
	nal_assembler_t *a = &ctx->nal_asm;
	if (!a->active || a->len == 0)
		return;

	cascade_frame_view_t cf = {
		.version = CASCADE_FRAME_VERSION,
		.src = src_u16,
		.dst = 0,
		.traffic_class = CASCADE_CLASS_VIDEO,
		.payload = a->buf,
		.payload_len = a->len,
	};
	send_cascade_frame(ctx, &cf);
	ctx->stats.nal_complete++;
	ctx->stats.video_bytes_out += a->len;

	if (ctx->verbose)
		fprintf(stderr, "gw: NAL complete seq=%u..%u len=%zu\n",
			a->first_seq, (uint16_t)(a->expect_seq - 1), a->len);

	a->len = 0;
	a->active = false;
}

static void drop_nal(app_ctx_t *ctx, uint16_t expected, uint16_t got)
{
	nal_assembler_t *a = &ctx->nal_asm;
	ctx->stats.nal_dropped++;
	ctx->stats.lts_gaps++;

	uint16_t pkt_count = (uint16_t)(got - a->first_seq);
	if (pkt_count <= 1)
		ctx->stats.nal_drop_1pkt++;
	else if (pkt_count <= 5)
		ctx->stats.nal_drop_2_5pkt++;
	else if (pkt_count <= 15)
		ctx->stats.nal_drop_6_15pkt++;
	else
		ctx->stats.nal_drop_16plus++;

	uint16_t gap_size = (uint16_t)(got - expected);
	if (gap_size > 1)
		ctx->stats.burst_gaps++;
	else
		ctx->stats.single_gaps++;

	if (ctx->verbose)
		fprintf(stderr, "gw: NAL DROPPED gap at seq expected=%u got=%u gap=%u nal_pkts=%u (had %zu bytes)\n",
			expected, got, gap_size, pkt_count, a->len);
	a->len = 0;
	a->active = false;
}

static void emit_lts_to_cascade(app_ctx_t *ctx, uint16_t src_u16)
{
	lts_packet_t emitted[64];
	size_t n = lts_decoder_emit(&ctx->lts_dec, emitted, 64, now_ms());

	nal_assembler_t *a = &ctx->nal_asm;

	for (size_t i = 0; i < n; i++) {
		lts_packet_t *pkt = &emitted[i];

		/* Gap detected — tolerate up to max_gap_tolerance skipped
		 * packets per NAL instead of immediately dropping.
		 * A partial NAL (with gaps) is better than a fully dropped one
		 * because H.265 can decode partial slices. */
		if (a->active && pkt->pkt_seq != a->expect_seq) {
			uint16_t gap = (uint16_t)(pkt->pkt_seq - a->expect_seq);
			if (a->gaps_tolerated + gap <= a->max_gap_tolerance) {
				a->gaps_tolerated += gap;
				ctx->stats.nal_gap_skipped += gap;
				if (ctx->verbose)
					fprintf(stderr, "gw: NAL gap tolerated seq expected=%u got=%u gap=%u (total=%u/%u)\n",
						a->expect_seq, pkt->pkt_seq, gap,
						a->gaps_tolerated, a->max_gap_tolerance);
			} else {
				drop_nal(ctx, a->expect_seq, pkt->pkt_seq);
			}
		}

		/* LAST_OF_FRAME = end of a NAL from vcpd. */
		if (pkt->flags & LTS_FLAG_LAST_OF_FRAME) {
			if (a->active && a->len > 0) {
				if (a->len + pkt->payload_len <= NAL_ASSEMBLE_MAX) {
					memcpy(a->buf + a->len, pkt->payload, pkt->payload_len);
					a->len += pkt->payload_len;
				}
				flush_nal(ctx, src_u16);
			} else if (!a->active && pkt->payload_len > 0) {
				memcpy(a->buf, pkt->payload, pkt->payload_len);
				a->len = pkt->payload_len;
				a->active = true;
				a->first_seq = pkt->pkt_seq;
				a->expect_seq = pkt->pkt_seq + 1;
				flush_nal(ctx, src_u16);
			} else {
				a->active = false;
				a->len = 0;
			}
			a->gaps_tolerated = 0;
			continue;
		}

		/* Start new NAL or continue current */
		if (!a->active) {
			a->active = true;
			a->len = 0;
			a->first_seq = pkt->pkt_seq;
			a->gaps_tolerated = 0;
		}

		if (a->len + pkt->payload_len <= NAL_ASSEMBLE_MAX) {
			memcpy(a->buf + a->len, pkt->payload, pkt->payload_len);
			a->len += pkt->payload_len;
		} else {
			drop_nal(ctx, a->expect_seq, pkt->pkt_seq);
			continue;
		}

		a->expect_seq = pkt->pkt_seq + 1;
	}
}

#define NACK_MIN_INTERVAL_MS 20

static void try_send_nack(app_ctx_t *ctx, uint64_t now)
{
	if (now - ctx->last_nack_ms < NACK_MIN_INTERVAL_MS)
		return;

	uint16_t gaps[16];
	size_t ngaps = lts_decoder_detect_gaps(&ctx->lts_dec, gaps, 16);
	if (ngaps == 0)
		return;

	lts_nack_t nack;
	nack.stream_id = 0;
	nack.start_seq = gaps[0];
	nack.bitmask = 0;
	for (size_t i = 0; i < ngaps; i++) {
		uint16_t offset = gaps[i] - gaps[0];
		if (offset < 16)
			nack.bitmask |= (uint16_t)(1 << offset);
	}

	uint8_t nack_buf[LTS_NACK_SIZE];
	size_t nack_len = lts_encode_nack(&nack, nack_buf, sizeof(nack_buf));
	if (nack_len == 0)
		return;

	ulama_frame_view_t uf = {
		.src_node = ctx->gw.node_id,
		.dst_node = ctx->lts_video_src_node,
		.flags = 0,
		.traffic_class = ULAMA_CLASS_CTRL,
		.seq = ctx->seq_counter++,
		.frag_idx = 0,
		.frag_total = 1,
		.ttl = ULAMA_FRAME_DEFAULT_TTL,
		.payload = nack_buf,
		.payload_len = nack_len,
	};

	uint8_t frame_buf[ULAMA_FRAME_HEADER_SIZE + ULAMA_FRAME_MAX_PAYLOAD];
	size_t frame_len = 0;
	if (ulama_frame_pack(&uf, frame_buf, sizeof(frame_buf), &frame_len))
		ulama_transport_tx_send(&ctx->ulama_tx, frame_buf, frame_len);

	ctx->last_nack_ms = now;
	ctx->stats.nack_sent++;

	if (ctx->verbose)
		fprintf(stderr, "gw: NACK sent start_seq=%u bitmask=0x%04x (%zu gaps)\n",
			nack.start_seq, nack.bitmask, ngaps);
}

static void handle_ulama_rx(app_ctx_t *ctx)
{
  for (int drain = 0; drain < 256; drain++) {
	uint8_t buf[ULAMA_FRAME_HEADER_SIZE + ULAMA_FRAME_MAX_PAYLOAD + 64];
	uint8_t src_mac[6] = {0};
	int8_t rssi = 0;

	ssize_t n = ulama_transport_rx_recv(&ctx->ulama_rx, buf, sizeof(buf), 0, src_mac, &rssi);
	if (n <= 0)
		return;

	ulama_frame_view_t uf;
	if (!ulama_frame_unpack(buf, (size_t)n, &uf)) {
		if (ctx->verbose)
			fprintf(stderr, "gw: invalid ULAMA frame (%zd bytes)\n", n);
		continue;
	}

	if (ctx->verbose) {
		fprintf(stderr, "gw: ULAMA RX src=%u dst=%u class=%u flags=0x%02X payload=%zu\n",
			uf.src_node, uf.dst_node, uf.traffic_class, uf.flags, uf.payload_len);
	}

	/* Per-class packet counters */
	switch (uf.traffic_class) {
	case ULAMA_CLASS_VIDEO:     ctx->stats.ulama_rx_video++; break;
	case ULAMA_CLASS_TELEMETRY: ctx->stats.ulama_rx_telem++; break;
	case ULAMA_CLASS_CTRL:      ctx->stats.ulama_rx_ctrl++; break;
	}

	uint16_t src_u16 = gw_addr_u8_to_u16(&ctx->gw, uf.src_node);
	uint8_t cascade_class = gw_class_ulama_to_cascade(uf.traffic_class);

	/* ANNOUNCE payload over CTRL class → remap to MANAGEMENT for cascade-core */
	if (uf.traffic_class == ULAMA_CLASS_CTRL && uf.payload_len > 9 &&
	    memcmp(uf.payload, "ANNOUNCE:", 9) == 0) {
		cascade_class = CASCADE_CLASS_MANAGEMENT;
	}

	if (uf.flags & ULAMA_FLAG_FRAGMENT) {
		bool complete = frag_reassembly_insert(&ctx->reassembly, &uf, now_ms());
		if (complete) {
			uint8_t reassembled[FRAG_MAX_REASSEMBLED];
			size_t reassembled_len = 0;
			if (frag_reassembly_complete(&ctx->reassembly, uf.src_node, uf.seq,
						     reassembled, sizeof(reassembled), &reassembled_len)) {
				if (uf.traffic_class == ULAMA_CLASS_VIDEO) {
					lts_packet_t pkt;
					if (lts_decode_packet(reassembled, reassembled_len, &pkt)) {
						uint64_t ts = now_ms();
						ctx->lts_video_src_node = uf.src_node;
						lts_decoder_insert(&ctx->lts_dec, &pkt, ts);
						emit_lts_to_cascade(ctx, src_u16);
						try_send_nack(ctx, ts);
					}
				} else {
					cascade_frame_view_t cf = {
						.version = CASCADE_FRAME_VERSION,
						.src = src_u16,
						.dst = 0,
						.traffic_class = cascade_class,
						.payload = reassembled,
						.payload_len = reassembled_len,
					};
					send_cascade_frame(ctx, &cf);
				}
			}
		}
		continue;
	}

	if (uf.traffic_class == ULAMA_CLASS_VIDEO) {
		lts_packet_t pkt;
		if (lts_decode_packet(uf.payload, uf.payload_len, &pkt)) {
			uint64_t ts = now_ms();
			ctx->lts_video_src_node = uf.src_node;
			ctx->stats.rssi_sum += rssi;
			ctx->stats.rssi_count++;

			if (pkt.flags & LTS_FLAG_FEC) {
				lts_packet_t recovered;
				uint32_t unrec_before = ctx->fec_dec.unrecoverable;
				if (lts_fec_decoder_add_parity(&ctx->fec_dec, pkt.stream_id,
							       pkt.payload, pkt.payload_len, &recovered)) {
					lts_decoder_insert(&ctx->lts_dec, &recovered, ts);
					ctx->stats.fec_recovered++;
				}
				if (ctx->fec_dec.unrecoverable > unrec_before)
					ctx->stats.fec_unrecoverable += ctx->fec_dec.unrecoverable - unrec_before;
			} else {
				bool is_dup = lts_decoder_insert(&ctx->lts_dec, &pkt, ts);
				if (is_dup)
					ctx->stats.lts_dup++;
				else {
					ctx->stats.lts_unique++;
					if (pkt.flags & LTS_FLAG_RETX)
						ctx->stats.retx_arrived++;
				}
				lts_fec_decoder_add_data(&ctx->fec_dec, pkt.pkt_seq, pkt.flags,
							 pkt.payload, pkt.payload_len);
			}
			if (!ctx->stats.lts_seq_valid) {
				ctx->stats.lts_seq_min = pkt.pkt_seq;
				ctx->stats.lts_seq_max = pkt.pkt_seq;
				ctx->stats.lts_seq_valid = true;
			} else {
				if (lts_seq_lt(pkt.pkt_seq, ctx->stats.lts_seq_min))
					ctx->stats.lts_seq_min = pkt.pkt_seq;
				if (lts_seq_lt(ctx->stats.lts_seq_max, pkt.pkt_seq))
					ctx->stats.lts_seq_max = pkt.pkt_seq;
			}
			emit_lts_to_cascade(ctx, src_u16);
			try_send_nack(ctx, ts);
		}
		continue;
	}

	cascade_frame_view_t cf = {
		.version = CASCADE_FRAME_VERSION,
		.src = src_u16,
		.dst = 0,
		.traffic_class = cascade_class,
		.payload = uf.payload,
		.payload_len = uf.payload_len,
	};
	send_cascade_frame(ctx, &cf);
  }
}

int main(int argc, char *argv[])
{
	app_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	strncpy(ctx.gw.cascade_in, "127.0.0.1:5601", sizeof(ctx.gw.cascade_in));
	strncpy(ctx.gw.cascade_out, "127.0.0.1:5600", sizeof(ctx.gw.cascade_out));
	strncpy(ctx.gw.transport_str, "udp", sizeof(ctx.gw.transport_str));
	strncpy(ctx.gw.iface, "mon0", sizeof(ctx.gw.iface));
	strncpy(ctx.listen_addr, "0.0.0.0:5000", sizeof(ctx.listen_addr));
	strncpy(ctx.peer_addr, "127.0.0.1:5001", sizeof(ctx.peer_addr));
	ctx.gw.node_id = 1;
	ctx.verbose = false;
	ctx.channel = 0;
	ctx.nal_asm.max_gap_tolerance = 2;

	static struct option long_opts[] = {
		{"cascade-in",  required_argument, NULL, 'C'},
		{"cascade-out", required_argument, NULL, 'O'},
		{"transport",   required_argument, NULL, 't'},
		{"listen",      required_argument, NULL, 'l'},
		{"peer",        required_argument, NULL, 'p'},
		{"iface",       required_argument, NULL, 'i'},
		{"channel",     required_argument, NULL, 'c'},
		{"node",        required_argument, NULL, 'n'},
		{"dst-mac",       required_argument, NULL, 'm'},
		{"gap-tolerance", required_argument, NULL, 'G'},
		{"verbose",       no_argument,       NULL, 'v'},
		{"help",        no_argument,       NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "C:O:t:l:p:i:c:n:m:vh", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'C': strncpy(ctx.gw.cascade_in, optarg, sizeof(ctx.gw.cascade_in) - 1); break;
		case 'O': strncpy(ctx.gw.cascade_out, optarg, sizeof(ctx.gw.cascade_out) - 1); break;
		case 't': strncpy(ctx.gw.transport_str, optarg, sizeof(ctx.gw.transport_str) - 1); break;
		case 'l': strncpy(ctx.listen_addr, optarg, sizeof(ctx.listen_addr) - 1); break;
		case 'p': strncpy(ctx.peer_addr, optarg, sizeof(ctx.peer_addr) - 1); break;
		case 'i': strncpy(ctx.gw.iface, optarg, sizeof(ctx.gw.iface) - 1); break;
		case 'c': ctx.channel = atoi(optarg); break;
		case 'n': ctx.gw.node_id = (uint8_t)atoi(optarg); break;
		case 'm': strncpy(ctx.dst_mac_str, optarg, sizeof(ctx.dst_mac_str) - 1); break;
		case 'G': ctx.nal_asm.max_gap_tolerance = (uint16_t)atoi(optarg); break;
		case 'v': ctx.verbose = true; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	ctx.cascade_rx_fd = open_udp_listener(ctx.gw.cascade_in);
	if (ctx.cascade_rx_fd < 0) {
		fprintf(stderr, "gw: failed to bind cascade-in %s: %s\n", ctx.gw.cascade_in, strerror(errno));
		return 1;
	}

	ctx.cascade_tx_fd = open_udp_sender(ctx.gw.cascade_out, &ctx.cascade_tx_addr);
	if (ctx.cascade_tx_fd < 0) {
		fprintf(stderr, "gw: failed to open cascade-out %s: %s\n", ctx.gw.cascade_out, strerror(errno));
		close(ctx.cascade_rx_fd);
		return 1;
	}

	ulama_transport_kind_t tk = ulama_transport_parse_kind(ctx.gw.transport_str);

	if (tk == ULAMA_TRANSPORT_KIND_UNOW && ctx.channel > 0) {
		if (setup_iface(ctx.gw.iface, ctx.channel, ctx.verbose) < 0) {
			fprintf(stderr, "gw: failed to configure %s\n", ctx.gw.iface);
			close(ctx.cascade_rx_fd);
			close(ctx.cascade_tx_fd);
			return 1;
		}
	}

	int rc;
	if (tk == ULAMA_TRANSPORT_KIND_UNOW) {
		uint8_t dst_mac[6];
		bool has_mac = (ctx.dst_mac_str[0] && ulama_transport_parse_mac(ctx.dst_mac_str, dst_mac));
		rc = ulama_transport_tx_init_unow(&ctx.ulama_tx, ctx.gw.node_id, ctx.gw.iface,
						  has_mac ? dst_mac : NULL);
		if (rc < 0) {
			fprintf(stderr, "gw: failed to init UNOW TX on %s: %s\n", ctx.gw.iface, strerror(errno));
			goto cleanup;
		}
		rc = ulama_transport_rx_init_unow(&ctx.ulama_rx, ctx.gw.node_id, ctx.gw.iface);
	} else {
		rc = ulama_transport_tx_init_udp(&ctx.ulama_tx, ctx.peer_addr);
		if (rc < 0) {
			fprintf(stderr, "gw: failed to init UDP TX to %s: %s\n", ctx.peer_addr, strerror(errno));
			goto cleanup;
		}
		rc = ulama_transport_rx_init_udp(&ctx.ulama_rx, ctx.listen_addr);
	}
	if (rc < 0) {
		fprintf(stderr, "gw: failed to init ULAMA RX: %s\n", strerror(errno));
		goto cleanup;
	}

	frag_reassembly_init(&ctx.reassembly);
	lts_decoder_init(&ctx.lts_dec, LTS_REORDER_WINDOW, LTS_EMIT_DEADLINE_MS);
	lts_fec_decoder_init(&ctx.fec_dec);

	fprintf(stderr, "ulama-gw: build=%s lts_max=%d started (node=%u, transport=%s)\n",
		ULAMA_GW_BUILD_ID, LTS_MAX_PAYLOAD,
		ctx.gw.node_id, ulama_transport_kind_name(tk));
	fprintf(stderr, "  cascade-in:  %s\n", ctx.gw.cascade_in);
	fprintf(stderr, "  cascade-out: %s\n", ctx.gw.cascade_out);
	if (tk == ULAMA_TRANSPORT_KIND_UDP) {
		fprintf(stderr, "  ulama-listen: %s\n", ctx.listen_addr);
		fprintf(stderr, "  ulama-peer:   %s\n", ctx.peer_addr);
	} else {
		fprintf(stderr, "  iface: %s\n", ctx.gw.iface);
	}

	struct pollfd pfds[1];
	pfds[0].fd = ctx.cascade_rx_fd;
	pfds[0].events = POLLIN;
	bool ulama_rx_is_pollable = (ctx.ulama_rx.fd >= 0);

	while (g_running) {
		int ret = poll(pfds, 1, ulama_rx_is_pollable ? 50 : 5);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		uint64_t ts = now_ms();

		if (pfds[0].revents & POLLIN)
			handle_cascade_rx(&ctx);

		/* For UNOW: fd=-1, poll can't watch it; call recv with timeout=0 */
		handle_ulama_rx(&ctx);

		/* Print stats every 5 seconds */
		if (ts - ctx.stats.last_print_ms >= 5000) {
			gw_stats_t *s = &ctx.stats;
			uint64_t dt = ts - s->last_print_ms;
			uint32_t vbps = (uint32_t)(s->video_bytes_out * 8000 / (dt > 0 ? dt : 1));
			uint32_t seq_range = s->lts_seq_valid
				? (uint16_t)(s->lts_seq_max - s->lts_seq_min + 1) : 0;
			uint32_t lost = (seq_range > s->lts_unique) ? seq_range - s->lts_unique : 0;
			int avg_rssi = s->rssi_count > 0 ? (int)(s->rssi_sum / (int32_t)s->rssi_count) : 0;
			fprintf(stderr, "[stats] video_rx=%u telem_rx=%u ctrl_rx=%u ctrl_tx=%u | "
				"LTS unique=%u dup=%u range=%u lost=%u | "
				"NAL ok=%u drop=%u | nack=%u | video_out=%u Kbit/s | rssi=%d | "
				"drop_sz 1/%u 2-5/%u 6-15/%u 16+/%u | gaps burst=%u single=%u | retx_ok=%u | fec +%u -%u | gap_skip=%u\n",
				s->ulama_rx_video, s->ulama_rx_telem, s->ulama_rx_ctrl, s->ctrl_tx,
				s->lts_unique, s->lts_dup, seq_range, lost,
				s->nal_complete, s->nal_dropped, s->nack_sent, vbps / 1000, avg_rssi,
				s->nal_drop_1pkt, s->nal_drop_2_5pkt, s->nal_drop_6_15pkt, s->nal_drop_16plus,
				s->burst_gaps, s->single_gaps, s->retx_arrived,
				s->fec_recovered, s->fec_unrecoverable, s->nal_gap_skipped);
			memset(s, 0, sizeof(*s));
			s->last_print_ms = ts;
		}

		frag_reassembly_expire(&ctx.reassembly, ts);

		lts_packet_t lts_out[16];
		size_t lts_n = lts_decoder_emit(&ctx.lts_dec, lts_out, 16, ts);
		for (size_t i = 0; i < lts_n; i++) {
			cascade_frame_view_t cf = {
				.version = CASCADE_FRAME_VERSION,
				.src = ctx.lts_video_src,
				.dst = 0,
				.traffic_class = CASCADE_CLASS_VIDEO,
				.payload = lts_out[i].payload,
				.payload_len = lts_out[i].payload_len,
			};
			send_cascade_frame(&ctx, &cf);
		}
	}

	fprintf(stderr, "ulama-gw: shutting down\n");

cleanup:
	if (ctx.cascade_rx_fd >= 0) close(ctx.cascade_rx_fd);
	if (ctx.cascade_tx_fd >= 0) close(ctx.cascade_tx_fd);
	ulama_transport_tx_close(&ctx.ulama_tx);
	ulama_transport_rx_close(&ctx.ulama_rx);

	return 0;
}
