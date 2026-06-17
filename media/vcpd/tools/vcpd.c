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
#include <poll.h>

#include "vcpd/lts_encoder.h"
#include "vcpd/uvcp.h"
#include "vcpd/video_source.h"
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

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  --source      DEVICE     Video device             (default /dev/video0)\n"
		"  --codec       CODEC      Encoder codec            (default libx264)\n"
		"  --bitrate     KBPS       Target bitrate in kbps   (default 512)\n"
		"  --width       W          Video width              (default 640)\n"
		"  --height      H          Video height             (default 480)\n"
		"  --fps         FPS        Frame rate               (default 25)\n"
		"  --transport   udp|unow   ULAMA transport          (default udp)\n"
		"  --peer        ADDR:PORT  ULAMA TX peer (UDP)      (default 127.0.0.1:5000)\n"
		"  --listen      ADDR:PORT  ULAMA RX listen (UVCP)   (default 0.0.0.0:5002)\n"
		"  --iface       NAME       Monitor mode iface       (default mon0)\n"
		"  --node        ID         Drone node ID            (default 2)\n"
		"  --dst-node    ID         Gateway node ID          (default 1)\n"
		"  --stream-id   ID         LTS stream ID            (default 0)\n"
		"  --verbose                Verbose logging\n"
		"  --help                   Show this help\n",
		prog);
}

typedef struct {
	video_source_ffmpeg_t video;
	lts_encoder_t lts_enc;
	uvcp_session_t uvcp_sess;
	ulama_tx_transport_t ulama_tx;
	ulama_rx_transport_t ulama_rx;
	uint8_t node_id;
	uint8_t dst_node;
	uint8_t stream_id;
	uint16_t ulama_seq;
	bool verbose;
	char transport_str[16];
	char peer_addr[64];
	char listen_addr[64];
	char iface[32];
	char dst_mac_str[32];
} vcpd_ctx_t;

static void send_ulama_video(vcpd_ctx_t *ctx, const uint8_t *data, size_t len)
{
	ulama_frame_view_t uf = {
		.src_node = ctx->node_id,
		.dst_node = ctx->dst_node,
		.flags = ULAMA_FLAG_DUP_ALLOWED,
		.traffic_class = ULAMA_CLASS_VIDEO,
		.seq = ctx->ulama_seq++,
		.frag_idx = 0,
		.frag_total = 1,
		.ttl = ULAMA_FRAME_DEFAULT_TTL,
		.payload = data,
		.payload_len = len,
	};

	uint8_t frame_buf[ULAMA_FRAME_HEADER_SIZE + ULAMA_FRAME_MAX_PAYLOAD];
	size_t frame_len = 0;
	if (ulama_frame_pack(&uf, frame_buf, sizeof(frame_buf), &frame_len))
		ulama_transport_tx_send(&ctx->ulama_tx, frame_buf, frame_len);
}

static void handle_uvcp_rx(vcpd_ctx_t *ctx)
{
	uint8_t buf[512];
	uint8_t src_mac[6] = {0};
	int8_t rssi = 0;

	ssize_t n = ulama_transport_rx_recv(&ctx->ulama_rx, buf, sizeof(buf), 0, src_mac, &rssi);
	if (n <= 0)
		return;

	ulama_frame_view_t uf;
	if (!ulama_frame_unpack(buf, (size_t)n, &uf))
		return;

	if (!uvcp_is_control(uf.payload, uf.payload_len))
		return;

	uvcp_message_t msg;
	if (!uvcp_parse(uf.payload, uf.payload_len, &msg))
		return;

	if (ctx->verbose) {
		fprintf(stderr, "vcpd: UVCP verb=%d from node %u\n", msg.verb, uf.src_node);
	}

	uvcp_state_t prev = ctx->uvcp_sess.state;
	uvcp_session_handle(&ctx->uvcp_sess, &msg, now_ms());

	if (prev == UVCP_STATE_IDLE && ctx->uvcp_sess.state == UVCP_STATE_STREAMING) {
		fprintf(stderr, "vcpd: stream started (READY from operator)\n");
		if (!ctx->video.running)
			video_source_ffmpeg_start(&ctx->video);
	}

	if (msg.verb == UVCP_VERB_PING) {
		uint8_t pong_buf[64];
		size_t pong_len = uvcp_build_pong(pong_buf, sizeof(pong_buf));
		if (pong_len > 0) {
			send_ulama_video(ctx, pong_buf, pong_len);
			ctx->uvcp_sess.last_pong_ms = now_ms();
		}
	}
}

int main(int argc, char *argv[])
{
	vcpd_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	strncpy(ctx.video.device, "/dev/video0", sizeof(ctx.video.device));
	strncpy(ctx.video.codec, "libx264", sizeof(ctx.video.codec));
	ctx.video.bitrate_kbps = 512;
	ctx.video.width = 640;
	ctx.video.height = 480;
	ctx.video.fps = 25;
	ctx.node_id = 2;
	ctx.dst_node = 1;
	ctx.stream_id = 0;
	strncpy(ctx.transport_str, "udp", sizeof(ctx.transport_str));
	strncpy(ctx.peer_addr, "127.0.0.1:5000", sizeof(ctx.peer_addr));
	strncpy(ctx.listen_addr, "0.0.0.0:5002", sizeof(ctx.listen_addr));
	strncpy(ctx.iface, "mon0", sizeof(ctx.iface));

	static struct option long_opts[] = {
		{"source",    required_argument, NULL, 's'},
		{"codec",     required_argument, NULL, 'c'},
		{"bitrate",   required_argument, NULL, 'b'},
		{"width",     required_argument, NULL, 'W'},
		{"height",    required_argument, NULL, 'H'},
		{"fps",       required_argument, NULL, 'f'},
		{"transport", required_argument, NULL, 't'},
		{"peer",      required_argument, NULL, 'p'},
		{"listen",    required_argument, NULL, 'l'},
		{"iface",     required_argument, NULL, 'i'},
		{"node",      required_argument, NULL, 'n'},
		{"dst-node",  required_argument, NULL, 'd'},
		{"stream-id", required_argument, NULL, 'S'},
		{"dst-mac",   required_argument, NULL, 'm'},
		{"verbose",   no_argument,       NULL, 'v'},
		{"help",      no_argument,       NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "s:c:b:W:H:f:t:p:l:i:n:d:S:m:vh", long_opts, NULL)) != -1) {
		switch (opt) {
		case 's': strncpy(ctx.video.device, optarg, sizeof(ctx.video.device) - 1); break;
		case 'c': strncpy(ctx.video.codec, optarg, sizeof(ctx.video.codec) - 1); break;
		case 'b': ctx.video.bitrate_kbps = atoi(optarg); break;
		case 'W': ctx.video.width = atoi(optarg); break;
		case 'H': ctx.video.height = atoi(optarg); break;
		case 'f': ctx.video.fps = atoi(optarg); break;
		case 't': strncpy(ctx.transport_str, optarg, sizeof(ctx.transport_str) - 1); break;
		case 'p': strncpy(ctx.peer_addr, optarg, sizeof(ctx.peer_addr) - 1); break;
		case 'l': strncpy(ctx.listen_addr, optarg, sizeof(ctx.listen_addr) - 1); break;
		case 'i': strncpy(ctx.iface, optarg, sizeof(ctx.iface) - 1); break;
		case 'n': ctx.node_id = (uint8_t)atoi(optarg); break;
		case 'd': ctx.dst_node = (uint8_t)atoi(optarg); break;
		case 'S': ctx.stream_id = (uint8_t)atoi(optarg); break;
		case 'm': strncpy(ctx.dst_mac_str, optarg, sizeof(ctx.dst_mac_str) - 1); break;
		case 'v': ctx.verbose = true; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGPIPE, SIG_IGN);

	ulama_transport_kind_t tk = ulama_transport_parse_kind(ctx.transport_str);
	int rc;
	if (tk == ULAMA_TRANSPORT_KIND_UNOW) {
		uint8_t dst_mac[6];
		bool has_mac = (ctx.dst_mac_str[0] && ulama_transport_parse_mac(ctx.dst_mac_str, dst_mac));
		rc = ulama_transport_tx_init_unow(&ctx.ulama_tx, ctx.node_id, ctx.iface,
						  has_mac ? dst_mac : NULL);
	} else {
		rc = ulama_transport_tx_init_udp(&ctx.ulama_tx, ctx.peer_addr);
	}
	if (rc < 0) {
		fprintf(stderr, "vcpd: failed to init ULAMA TX: %s\n", strerror(errno));
		return 1;
	}

	if (tk == ULAMA_TRANSPORT_KIND_UNOW)
		rc = ulama_transport_rx_init_unow(&ctx.ulama_rx, ctx.node_id, ctx.iface);
	else
		rc = ulama_transport_rx_init_udp(&ctx.ulama_rx, ctx.listen_addr);
	if (rc < 0) {
		fprintf(stderr, "vcpd: failed to init ULAMA RX: %s\n", strerror(errno));
		return 1;
	}

	lts_encoder_init(&ctx.lts_enc, ctx.stream_id);
	uvcp_session_init(&ctx.uvcp_sess, UVCP_LEASE_DEFAULT_MS);

	fprintf(stderr, "vcpd: started (node=%u, dst=%u, stream=%u, transport=%s)\n",
		ctx.node_id, ctx.dst_node, ctx.stream_id, ulama_transport_kind_name(tk));

	uint8_t ts_buf[VIDEO_TS_GROUP_BYTES];

	while (g_running) {
		struct pollfd pfds[2];
		int nfds = 0;

		pfds[nfds].fd = ctx.ulama_rx.fd;
		pfds[nfds].events = POLLIN;
		nfds++;

		if (ctx.video.running && ctx.video.pipe_fd >= 0) {
			pfds[nfds].fd = ctx.video.pipe_fd;
			pfds[nfds].events = POLLIN;
			nfds++;
		}

		int ret = poll(pfds, (nfds_t)nfds, 100);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (pfds[0].revents & POLLIN)
			handle_uvcp_rx(&ctx);

		uint64_t ts = now_ms();

		if (!uvcp_session_is_active(&ctx.uvcp_sess, ts) && ctx.video.running) {
			fprintf(stderr, "vcpd: lease expired, stopping video\n");
			video_source_ffmpeg_stop(&ctx.video);
			continue;
		}

		if (nfds > 1 && (pfds[1].revents & POLLIN) && ctx.video.running) {
			ssize_t n = video_source_ffmpeg_read(&ctx.video, ts_buf, sizeof(ts_buf));
			if (n <= 0) {
				if (n == 0) {
					fprintf(stderr, "vcpd: ffmpeg EOF, restarting\n");
					video_source_ffmpeg_stop(&ctx.video);
					if (uvcp_session_is_active(&ctx.uvcp_sess, ts))
						video_source_ffmpeg_start(&ctx.video);
				}
				continue;
			}

			lts_encoded_packet_t lts_pkts[8];
			size_t npkts = lts_encoder_encode(&ctx.lts_enc, ts_buf, (size_t)n,
							  0, lts_pkts, 8);

			for (size_t i = 0; i < npkts; i++) {
				if (lts_pkts[i].len <= ULAMA_FRAME_MAX_PAYLOAD) {
					send_ulama_video(&ctx, lts_pkts[i].data, lts_pkts[i].len);
				}
			}

			if (ctx.verbose && npkts > 0) {
				fprintf(stderr, "vcpd: sent %zu LTS packets (%zd bytes TS)\n", npkts, n);
			}
		}
	}

	fprintf(stderr, "vcpd: shutting down\n");
	video_source_ffmpeg_stop(&ctx.video);
	ulama_transport_tx_close(&ctx.ulama_tx);
	ulama_transport_rx_close(&ctx.ulama_rx);

	return 0;
}
