#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
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
#include "vcpd/lts_fec_enc.h"
#include "vcpd/uvcp.h"
#include "vcpd/video_source.h"
#include "ulama/ulama_frame.h"
#include "ulama/ulama_version.h"
#include "ulama/transport.h"

#ifndef ULAMA_WITH_UNOW
#define ULAMA_WITH_UNOW 0
#endif
#if ULAMA_WITH_UNOW
#include "unow/radio_unow.h"
#endif

/* Unified video source API — delegates to MPP (HW) or ffmpeg (SW) */
#ifdef VCPD_WITH_MPP
#define vsrc_start(v)    video_source_mpp_start(v)
#define vsrc_stop(v)     video_source_mpp_stop(v)
#define vsrc_read(v,b,l) video_source_mpp_read(v,b,l)
#else
#define vsrc_start(v)    video_source_ffmpeg_start(v)
#define vsrc_stop(v)     video_source_ffmpeg_stop(v)
#define vsrc_read(v,b,l) video_source_ffmpeg_read(v,b,l)
#endif

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
	fprintf(stderr, "vcpd: build #%d (%s@%s) %s\n",
		ULAMA_BUILD_NUMBER, ULAMA_GIT_BRANCH, ULAMA_GIT_HASH, ULAMA_BUILD_DATE);
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  --source      DEVICE     Video device             (default /dev/video0)\n"
		"  --codec       CODEC      Encoder codec            (default libx264)\n"
		"  --bitrate     KBPS       Target bitrate in kbps   (default 512)\n"
		"  --width       W          Video width              (default 640)\n"
		"  --height      H          Video height             (default 480)\n"
		"  --fps         FPS        Frame rate               (default 25)\n"
		"  --gop         N          GOP size (1=all IDR)     (adaptive: =fps)\n"
		"  --transport   udp|unow   ULAMA transport          (default udp)\n"
		"  --peer        ADDR:PORT  ULAMA TX peer (UDP)      (default 127.0.0.1:5000)\n"
		"  --listen      ADDR:PORT  ULAMA RX listen (UVCP)   (default 0.0.0.0:5002)\n"
		"  --iface       NAME       Monitor mode iface       (default mon0)\n"
		"  --node        ID         Drone node ID            (default 2)\n"
		"  --dst-node    ID         Gateway node ID          (default 1)\n"
		"  --stream-id   ID         LTS stream ID            (default 0)\n"
		"  --autostart              Start streaming immediately (no UVCP READY needed)\n"
		"  --test        FILE       Capture raw encoder output to file and exit\n"
		"  --test-frames N          Number of frames to capture in test mode (default 50)\n"
		"  --test-pattern           Use color bars instead of camera\n"
		"  --pace-us     US         Inter-packet pacing delay in us    (adaptive: 300-3000)\n"
		"  --reliable    MODE       0=unreliable 1=last-pkt 2=all 3=adaptive (default 2)\n"
		"  --reliable-threshold N   Min NAL packets for adaptive mode 3    (default 3)\n"
		"  --ack-timeout US         UNOW ACK timeout in us                 (default 8000)\n"
		"  --ack-retry   N          UNOW ACK max retries                   (adaptive: 3-5)\n"
		"  --fec         K          FEC group size (0=disabled, 2-8)       (default 0)\n"
		"  --lts-mtu     N          LTS payload size in bytes              (default 1440)\n"
		"  --benchmark   KBPS       Benchmark mode: send synthetic data     (default 0=off)\n"
		"  --vps-repeat  N          VPS/SPS/PPS copies before each IDR      (default 1)\n"
		"  --verbose                Verbose logging\n"
		"  --help                   Show this help\n",
		prog);
}

static int run_test_capture(video_source_t *video, const char *outpath, int max_frames)
{
	fprintf(stderr, "vcpd: TEST MODE — capturing %d frames to %s\n", max_frames, outpath);

	if (vsrc_start(video) != 0) {
		fprintf(stderr, "vcpd: failed to start video source\n");
		return 1;
	}

	FILE *fp = fopen(outpath, "wb");
	if (!fp) {
		fprintf(stderr, "vcpd: cannot open %s: %s\n", outpath, strerror(errno));
		vsrc_stop(video);
		return 1;
	}

	uint8_t buf[VIDEO_TS_GROUP_BYTES];
	int frames = 0;
	size_t total_bytes = 0;
	uint64_t t0 = now_ms();

	while (frames < max_frames && g_running) {
		ssize_t n = vsrc_read(video, buf, sizeof(buf));
		if (n <= 0) {
			if (n == 0) break;
			if (errno == EINTR) continue;
			fprintf(stderr, "vcpd: read error: %s\n", strerror(errno));
			break;
		}
		fwrite(buf, 1, (size_t)n, fp);
		total_bytes += (size_t)n;
		frames++;

		if (frames % 10 == 0)
			fprintf(stderr, "vcpd: captured %d/%d frames (%zu bytes)\n",
				frames, max_frames, total_bytes);
	}

	fclose(fp);
	vsrc_stop(video);

	uint64_t elapsed = now_ms() - t0;
	fprintf(stderr, "vcpd: TEST DONE — %d frames, %zu bytes, %llu ms (%.1f fps)\n",
		frames, total_bytes, (unsigned long long)elapsed,
		elapsed > 0 ? frames * 1000.0 / elapsed : 0.0);
	fprintf(stderr, "vcpd: output: %s\n", outpath);
	fprintf(stderr, "vcpd: to verify, copy to host and run:\n");
	fprintf(stderr, "  ffplay -f hevc %s\n", outpath);
	fprintf(stderr, "  ffmpeg -f hevc -i %s -frames:v 5 frame_%%02d.jpg\n", outpath);
	return 0;
}

#define PARAM_NAL_MAX_SIZE 128

typedef struct {
	video_source_t video;
	lts_encoder_t lts_enc;
	uvcp_session_t uvcp_sess;
	ulama_tx_transport_t ulama_tx;
	ulama_rx_transport_t ulama_rx;
	uint8_t node_id;
	uint8_t dst_node;
	uint8_t stream_id;
	uint16_t ulama_seq;
	bool verbose;
	bool autostart;
	char test_output[256];
	int test_frames;
	char transport_str[16];
	char peer_addr[64];
	char listen_addr[64];
	char iface[32];
	char dst_mac_str[32];
	/* Cached HEVC parameter NALs: [0]=VPS(32), [1]=SPS(33), [2]=PPS(34) */
	uint8_t cached_params[3][PARAM_NAL_MAX_SIZE];
	size_t cached_params_len[3];
	lts_retx_buf_t *retx_buf;
	uint32_t nack_retx_count;
	uint32_t nack_retx_miss;
	unsigned int pace_us;
	int reliable_mode;
	int reliable_threshold;
	uint32_t ack_timeout_us;
	uint32_t ack_max_retry;
	int fec_group;
	lts_fec_encoder_t fec_enc;
	uint32_t fec_sent;
	int lts_mtu;
	int benchmark_kbps;
	int param_dup_count;
	lts_encoded_packet_t *lts_pkt_buf;
	size_t lts_pkt_cap;
} vcpd_ctx_t;

static unsigned int tx_fail_count = 0;
#define TX_FAIL_THRESHOLD 10

static bool send_ulama_video_ex(vcpd_ctx_t *ctx, const uint8_t *data, size_t len, bool reliable)
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
	if (!ulama_frame_pack(&uf, frame_buf, sizeof(frame_buf), &frame_len))
		return false;

	int rc;
	if (reliable)
		rc = ulama_transport_tx_send_reliable(&ctx->ulama_tx, frame_buf, frame_len);
	else
		rc = ulama_transport_tx_send(&ctx->ulama_tx, frame_buf, frame_len);

	if (rc >= 0) {
		tx_fail_count = 0;
		return true;
	}

	tx_fail_count++;
	return false;
}

static bool send_ulama_video(vcpd_ctx_t *ctx, const uint8_t *data, size_t len)
{
	return send_ulama_video_ex(ctx, data, len, false);
}

static bool ensure_lts_packet_capacity(vcpd_ctx_t *ctx, size_t required)
{
	if (required == 0)
		return true;
	if (required <= ctx->lts_pkt_cap)
		return true;

	lts_encoded_packet_t *new_buf = (lts_encoded_packet_t *)realloc(
		ctx->lts_pkt_buf, required * sizeof(*new_buf));
	if (!new_buf)
		return false;

	ctx->lts_pkt_buf = new_buf;
	ctx->lts_pkt_cap = required;
	return true;
}

static void handle_nack(vcpd_ctx_t *ctx, const lts_enc_nack_t *nack)
{
	for (int bit = 0; bit < 16; bit++) {
		if (!(nack->bitmask & (1 << bit)))
			continue;
		uint16_t seq = nack->start_seq + (uint16_t)bit;
		const lts_encoded_packet_t *pkt = lts_retx_buf_find(ctx->retx_buf, seq);
		if (pkt) {
			uint8_t retx_data[LTS_ENC_HEADER_SIZE + LTS_ENC_MAX_PAYLOAD];
			memcpy(retx_data, pkt->data, pkt->len);
			retx_data[3] |= LTS_ENC_FLAG_RETX;
			if (send_ulama_video(ctx, retx_data, pkt->len)) {
				ctx->nack_retx_count++;
				if (ctx->verbose)
					fprintf(stderr, "vcpd: RETX seq=%u (%zu bytes)\n", seq, pkt->len);
			} else if (ctx->verbose) {
				fprintf(stderr, "vcpd: RETX seq=%u SEND FAIL\n", seq);
			}
		} else {
			ctx->nack_retx_miss++;
			if (ctx->verbose)
				fprintf(stderr, "vcpd: RETX seq=%u MISS (cur=%u)\n", seq, ctx->lts_enc.next_seq);
		}
	}
}

static void handle_uvcp_rx(vcpd_ctx_t *ctx)
{
	for (int drain = 0; drain < 128; drain++) {
		uint8_t buf[512];
		uint8_t src_mac[6] = {0};
		int8_t rssi = 0;

		ssize_t n = ulama_transport_rx_recv(&ctx->ulama_rx, buf, sizeof(buf), 0, src_mac, &rssi);
		if (n <= 0)
			return;

		ulama_frame_view_t uf;
		if (!ulama_frame_unpack(buf, (size_t)n, &uf))
			continue;

		/* Skip own packets (monitor mode captures outgoing frames too) */
		if (uf.src_node == ctx->node_id)
			continue;

		if (lts_enc_is_nack(uf.payload, uf.payload_len)) {
			lts_enc_nack_t nack;
			if (lts_enc_decode_nack(uf.payload, uf.payload_len, &nack)) {
				if (ctx->verbose)
					fprintf(stderr, "vcpd: NACK rx start_seq=%u bitmask=0x%04x\n",
						nack.start_seq, nack.bitmask);
				handle_nack(ctx, &nack);
			}
			continue;
		}

		if (!uvcp_is_control(uf.payload, uf.payload_len))
			continue;

		uvcp_message_t msg;
		if (!uvcp_parse(uf.payload, uf.payload_len, &msg))
			continue;

		if (ctx->verbose)
			fprintf(stderr, "vcpd: UVCP verb=%d from node %u\n", msg.verb, uf.src_node);

		uvcp_state_t prev = ctx->uvcp_sess.state;
		uvcp_session_handle(&ctx->uvcp_sess, &msg, now_ms());

		if (prev == UVCP_STATE_IDLE && ctx->uvcp_sess.state == UVCP_STATE_STREAMING) {
			fprintf(stderr, "vcpd: stream started (READY from operator)\n");
			if (!ctx->video.running)
				vsrc_start(&ctx->video);
		}

		if (msg.verb == UVCP_VERB_PING) {
			uint8_t pong_buf[64];
			size_t pong_len = uvcp_build_pong(pong_buf, sizeof(pong_buf));
			if (pong_len > 0) {
					(void)send_ulama_video(ctx, pong_buf, pong_len);
				ctx->uvcp_sess.last_pong_ms = now_ms();
			}
		}
	}
}

static int run_benchmark(vcpd_ctx_t *ctx)
{
	size_t mtu = ctx->lts_enc.max_payload;
	uint64_t target_bps = (uint64_t)ctx->benchmark_kbps * 1000;
	uint64_t target_Bps = target_bps / 8;
	double pps_target = (double)target_Bps / (double)mtu;
	uint64_t interval_us = pps_target > 0 ? (uint64_t)(1000000.0 / pps_target) : 10000;
	bool use_reliable = (ctx->reliable_mode >= 2);

	fprintf(stderr, "vcpd: BENCHMARK — target %d Kbit/s, mtu=%zu, reliable=%d, interval=%llu us (%.0f pps)\n",
		ctx->benchmark_kbps, mtu, ctx->reliable_mode,
		(unsigned long long)interval_us, pps_target);

	uint8_t dummy[LTS_ENC_MAX_PAYLOAD];
	for (size_t i = 0; i < sizeof(dummy); i++)
		dummy[i] = (uint8_t)(i & 0xFF);

	uint64_t t0 = now_ms();
	uint64_t pkts_sent = 0, bytes_sent = 0;
	uint64_t last_report = t0;
	uint32_t rpt_pkts = 0, rpt_bytes = 0;

	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);

	while (g_running) {
		handle_uvcp_rx(ctx);

		lts_encoded_packet_t pkt;
		size_t np = lts_encoder_encode(&ctx->lts_enc, dummy, mtu, 0, &pkt, 1);
		if (np == 0) continue;

		if (send_ulama_video_ex(ctx, pkt.data, pkt.len, use_reliable)) {
			pkts_sent++;
			bytes_sent += pkt.len;
			rpt_pkts++;
			rpt_bytes += pkt.len;
		}

		uint64_t now = now_ms();
		if (now - last_report >= 5000) {
			uint64_t dt = now - last_report;
			fprintf(stderr, "[bench-tx] pps=%u bps=%u Kbit/s total=%llu pkts %llu bytes\n",
				(uint32_t)(rpt_pkts * 1000 / dt),
				(uint32_t)((uint64_t)rpt_bytes * 8000 / dt / 1000),
				(unsigned long long)pkts_sent,
				(unsigned long long)bytes_sent);
			rpt_pkts = 0;
			rpt_bytes = 0;
			last_report = now;
		}

		next.tv_nsec += (long)(interval_us * 1000);
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}

	uint64_t elapsed = now_ms() - t0;
	fprintf(stderr, "vcpd: BENCHMARK DONE — %llu pkts, %llu bytes, %llu ms, %u pps %u Kbit/s\n",
		(unsigned long long)pkts_sent, (unsigned long long)bytes_sent,
		(unsigned long long)elapsed,
		elapsed > 0 ? (uint32_t)(pkts_sent * 1000 / elapsed) : 0,
		elapsed > 0 ? (uint32_t)(bytes_sent * 8000 / elapsed / 1000) : 0);
	return 0;
}

static void vcpd_load_config(vcpd_ctx_t *ctx, const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "vcpd: cannot open config %s: %s\n", path, strerror(errno));
		return;
	}
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *hash = strchr(line, '#');
		if (hash)
			*hash = '\0';
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char *key = line;
		char *val = eq + 1;
		while (isspace((unsigned char)*key)) key++;
		while (isspace((unsigned char)*val)) val++;
		size_t klen = strlen(key);
		while (klen > 0 && isspace((unsigned char)key[klen - 1]))
			key[--klen] = '\0';
		size_t vlen = strlen(val);
		while (vlen > 0 && isspace((unsigned char)val[vlen - 1]))
			val[--vlen] = '\0';
		if (klen == 0 || vlen == 0)
			continue;

		if      (!strcmp(key, "source"))             strncpy(ctx->video.device, val, sizeof(ctx->video.device) - 1);
		else if (!strcmp(key, "bitrate"))            ctx->video.bitrate_kbps = atoi(val);
		else if (!strcmp(key, "width"))              ctx->video.width = atoi(val);
		else if (!strcmp(key, "height"))             ctx->video.height = atoi(val);
		else if (!strcmp(key, "fps"))                ctx->video.fps = atoi(val);
		else if (!strcmp(key, "gop"))                ctx->video.gop = atoi(val);
		else if (!strcmp(key, "transport"))          strncpy(ctx->transport_str, val, sizeof(ctx->transport_str) - 1);
		else if (!strcmp(key, "iface"))              strncpy(ctx->iface, val, sizeof(ctx->iface) - 1);
		else if (!strcmp(key, "node"))               ctx->node_id = (uint8_t)atoi(val);
		else if (!strcmp(key, "dst_node"))           ctx->dst_node = (uint8_t)atoi(val);
		else if (!strcmp(key, "dst_mac"))            strncpy(ctx->dst_mac_str, val, sizeof(ctx->dst_mac_str) - 1);
		else if (!strcmp(key, "stream_id"))          ctx->stream_id = (uint8_t)atoi(val);
		else if (!strcmp(key, "peer"))               strncpy(ctx->peer_addr, val, sizeof(ctx->peer_addr) - 1);
		else if (!strcmp(key, "listen"))             strncpy(ctx->listen_addr, val, sizeof(ctx->listen_addr) - 1);
		else if (!strcmp(key, "lts_mtu"))            ctx->lts_mtu = atoi(val);
		else if (!strcmp(key, "reliable"))           ctx->reliable_mode = atoi(val);
		else if (!strcmp(key, "reliable_threshold")) ctx->reliable_threshold = atoi(val);
		else if (!strcmp(key, "fec"))                ctx->fec_group = atoi(val);
		else if (!strcmp(key, "pace_us"))            ctx->pace_us = (unsigned int)atoi(val);
		else if (!strcmp(key, "ack_timeout_us"))     ctx->ack_timeout_us = (uint32_t)atoi(val);
		else if (!strcmp(key, "ack_retry"))          ctx->ack_max_retry = (uint32_t)atoi(val);
		else if (!strcmp(key, "vps_repeat"))         ctx->param_dup_count = atoi(val);
		else if (!strcmp(key, "autostart"))          ctx->autostart = !strcmp(val, "true");
		else if (!strcmp(key, "verbose"))            ctx->verbose = !strcmp(val, "true");
	}
	fclose(f);
}

int main(int argc, char *argv[])
{
	vcpd_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	strncpy(ctx.video.device, "/dev/video0", sizeof(ctx.video.device));
#ifndef VCPD_WITH_MPP
	strncpy(ctx.video.codec, "libx264", sizeof(ctx.video.codec));
#endif
	ctx.video.bitrate_kbps = 512;
	ctx.video.width = 640;
	ctx.video.height = 480;
	ctx.video.fps = 25;
	ctx.video.gop = 0;
	ctx.pace_us = 0;
	ctx.reliable_mode = 0;
	ctx.reliable_threshold = 3;
	ctx.ack_timeout_us = 2000;
	ctx.ack_max_retry = 0;
	ctx.lts_mtu = 1440;
	ctx.param_dup_count = 1;
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
		{"gop",       required_argument, NULL, 'g'},
		{"transport", required_argument, NULL, 't'},
		{"peer",      required_argument, NULL, 'p'},
		{"listen",    required_argument, NULL, 'l'},
		{"iface",     required_argument, NULL, 'i'},
		{"node",      required_argument, NULL, 'n'},
		{"dst-node",  required_argument, NULL, 'd'},
		{"stream-id", required_argument, NULL, 'S'},
		{"dst-mac",   required_argument, NULL, 'm'},
		{"autostart",    no_argument,       NULL, 'A'},
		{"test",         required_argument, NULL, 'T'},
		{"test-pattern", no_argument,       NULL, 'P'},
		{"test-frames",  required_argument, NULL, 'F'},
		{"pace-us",              required_argument, NULL, 'Z'},
		{"reliable",             required_argument, NULL, 'R'},
		{"reliable-threshold",   required_argument, NULL, 'Q'},
		{"ack-timeout",          required_argument, NULL, 'K'},
		{"ack-retry",            required_argument, NULL, 'Y'},
		{"fec",                  required_argument, NULL, 'E'},
		{"lts-mtu",              required_argument, NULL, 'M'},
		{"benchmark",            required_argument, NULL, 'B'},
		{"vps-repeat",           required_argument, NULL, 'D'},
		{"config",               required_argument, NULL, 'C'},
		{"version",              no_argument,       NULL, 'V'},
		{"verbose",              no_argument,       NULL, 'v'},
		{"help",         no_argument,       NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	int opt;
	ctx.test_frames = 50;

	/* Pre-scan for --config so that config file acts as defaults
	 * and all other CLI args can override it regardless of order. */
	for (int i = 1; i < argc - 1; i++) {
		if (!strcmp(argv[i], "--config") || !strcmp(argv[i], "-C")) {
			vcpd_load_config(&ctx, argv[i + 1]);
			break;
		}
		if (!strncmp(argv[i], "--config=", 9)) {
			vcpd_load_config(&ctx, argv[i] + 9);
			break;
		}
	}

	while ((opt = getopt_long(argc, argv, "s:c:b:W:H:f:t:p:l:i:n:d:S:m:T:F:Vvh", long_opts, NULL)) != -1) {
		switch (opt) {
		case 's': strncpy(ctx.video.device, optarg, sizeof(ctx.video.device) - 1); break;
#ifndef VCPD_WITH_MPP
		case 'c': strncpy(ctx.video.codec, optarg, sizeof(ctx.video.codec) - 1); break;
#else
		case 'c': break; /* codec ignored with MPP HW encoder */
#endif
		case 'b': ctx.video.bitrate_kbps = atoi(optarg); break;
		case 'W': ctx.video.width = atoi(optarg); break;
		case 'H': ctx.video.height = atoi(optarg); break;
		case 'f': ctx.video.fps = atoi(optarg); break;
		case 'g': ctx.video.gop = atoi(optarg); break;
		case 't': strncpy(ctx.transport_str, optarg, sizeof(ctx.transport_str) - 1); break;
		case 'p': strncpy(ctx.peer_addr, optarg, sizeof(ctx.peer_addr) - 1); break;
		case 'l': strncpy(ctx.listen_addr, optarg, sizeof(ctx.listen_addr) - 1); break;
		case 'i': strncpy(ctx.iface, optarg, sizeof(ctx.iface) - 1); break;
		case 'n': ctx.node_id = (uint8_t)atoi(optarg); break;
		case 'd': ctx.dst_node = (uint8_t)atoi(optarg); break;
		case 'S': ctx.stream_id = (uint8_t)atoi(optarg); break;
		case 'm': strncpy(ctx.dst_mac_str, optarg, sizeof(ctx.dst_mac_str) - 1); break;
		case 'A': ctx.autostart = true; break;
		case 'T': strncpy(ctx.test_output, optarg, sizeof(ctx.test_output) - 1); break;
		case 'P': ctx.video.test_pattern = true; break;
		case 'F': ctx.test_frames = atoi(optarg); break;
		case 'Z': ctx.pace_us = (unsigned int)atoi(optarg); break;
		case 'R': ctx.reliable_mode = atoi(optarg); break;
		case 'Q': ctx.reliable_threshold = atoi(optarg); break;
		case 'K': ctx.ack_timeout_us = (uint32_t)atoi(optarg); break;
		case 'Y': ctx.ack_max_retry = (uint32_t)atoi(optarg); break;
		case 'E': ctx.fec_group = atoi(optarg); break;
		case 'M': ctx.lts_mtu = atoi(optarg); break;
		case 'B': ctx.benchmark_kbps = atoi(optarg); break;
		case 'D': ctx.param_dup_count = atoi(optarg); break;
		case 'C': break; /* already handled in pre-scan above */
		case 'V':
			fprintf(stderr, "vcpd: build #%d (%s@%s) %s\n",
				ULAMA_BUILD_NUMBER, ULAMA_GIT_BRANCH, ULAMA_GIT_HASH, ULAMA_BUILD_DATE);
			return 0;
		case 'v': ctx.verbose = true; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGPIPE, SIG_IGN);

	/* Adaptive defaults based on fps if not explicitly set */
	int fps = ctx.video.fps > 0 ? ctx.video.fps : 25;
	if (ctx.video.gop == 0)
		ctx.video.gop = fps > 5 ? fps : 5;
	if (ctx.ack_max_retry == 0)
		ctx.ack_max_retry = fps <= 5 ? 5 : 3;
	if (ctx.pace_us == 0)
		ctx.pace_us = fps <= 5 ? 3000 : 300;
	fprintf(stderr, "vcpd: adaptive defaults for %d fps: gop=%d ack_retry=%u pace=%u us\n",
		fps, ctx.video.gop, ctx.ack_max_retry, ctx.pace_us);

	if (ctx.test_output[0]) {
		return run_test_capture(&ctx.video, ctx.test_output, ctx.test_frames);
	}

	ulama_transport_kind_t tk = ulama_transport_parse_kind(ctx.transport_str);
	int rc;
	if (tk == ULAMA_TRANSPORT_KIND_RADIOD) {
		rc = ulama_transport_tx_init_radiod(&ctx.ulama_tx, ctx.node_id,
						    NULL, "vcpd_tx");
	} else if (tk == ULAMA_TRANSPORT_KIND_UNOW) {
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

	if (tk == ULAMA_TRANSPORT_KIND_RADIOD)
		rc = ulama_transport_rx_init_radiod(&ctx.ulama_rx, ctx.node_id,
						    NULL, "vcpd_rx");
	else if (tk == ULAMA_TRANSPORT_KIND_UNOW)
		rc = ulama_transport_rx_init_unow(&ctx.ulama_rx, ctx.node_id, ctx.iface);
	else
		rc = ulama_transport_rx_init_udp(&ctx.ulama_rx, ctx.listen_addr);
	if (rc < 0) {
		fprintf(stderr, "vcpd: failed to init ULAMA RX: %s\n", strerror(errno));
		return 1;
	}

#if ULAMA_WITH_UNOW
	if (tk == ULAMA_TRANSPORT_KIND_UNOW)
		unow_set_ack_params(ctx.ack_timeout_us, ctx.ack_max_retry);
#endif

	lts_encoder_init(&ctx.lts_enc, ctx.stream_id);
	if (ctx.lts_mtu > 0 && ctx.lts_mtu <= LTS_ENC_MAX_PAYLOAD)
		ctx.lts_enc.max_payload = (size_t)ctx.lts_mtu;
	if (ctx.fec_group > 0)
		lts_fec_encoder_init(&ctx.fec_enc, ctx.fec_group);
	ctx.retx_buf = (lts_retx_buf_t *)calloc(1, sizeof(lts_retx_buf_t));
	if (!ctx.retx_buf) {
		fprintf(stderr, "vcpd: failed to allocate retransmit buffer\n");
		return 1;
	}
	lts_retx_buf_init(ctx.retx_buf);
	uvcp_session_init(&ctx.uvcp_sess, UVCP_LEASE_DEFAULT_MS);

	fprintf(stderr,
		"vcpd: build #%d (%s@%s) %s\n"
		"vcpd: --- config ---\n"
		"vcpd:   source     = %s\n"
		"vcpd:   video      = %dx%d  fps=%d  gop=%d  bitrate=%d Kbit/s\n"
		"vcpd:   transport  = %s  node=%u → dst=%u  stream=%u\n"
		"vcpd:   lts_mtu    = %zu B\n"
		"vcpd:   pace       = %u us\n"
		"vcpd:   reliable   = %d  threshold=%d  ack=%u us / %u retry\n"
		"vcpd:   fec        = %d\n"
		"vcpd:   vps_repeat = %d\n"
		"vcpd:   autostart  = %s\n"
		"vcpd: ---------------\n",
		ULAMA_BUILD_NUMBER, ULAMA_GIT_BRANCH, ULAMA_GIT_HASH, ULAMA_BUILD_DATE,
		ctx.video.device,
		ctx.video.width, ctx.video.height, ctx.video.fps, ctx.video.gop, ctx.video.bitrate_kbps,
		ulama_transport_kind_name(tk), ctx.node_id, ctx.dst_node, ctx.stream_id,
		ctx.lts_enc.max_payload,
		ctx.pace_us,
		ctx.reliable_mode, ctx.reliable_threshold, ctx.ack_timeout_us, ctx.ack_max_retry,
		ctx.fec_group,
		ctx.param_dup_count,
		ctx.autostart ? "true" : "false");

	if (ctx.benchmark_kbps > 0) {
		int rc = run_benchmark(&ctx);
		ulama_transport_tx_close(&ctx.ulama_tx);
		ulama_transport_rx_close(&ctx.ulama_rx);
		free(ctx.retx_buf);
		return rc;
	}

	if (ctx.autostart) {
		fprintf(stderr, "vcpd: autostart — starting video immediately\n");
		ctx.uvcp_sess.state = UVCP_STATE_STREAMING;
		ctx.uvcp_sess.last_ready_ms = now_ms();
		ctx.uvcp_sess.lease_ms = UINT64_MAX / 2;
		int vrc = vsrc_start(&ctx.video);
		fprintf(stderr, "vcpd: vsrc_start returned %d, pipe_fd=%d, running=%d\n",
			vrc, ctx.video.pipe_fd, ctx.video.running);
	}

	/* In MPP ring mode each read returns one VENC pack (up to VIDEO_MPP_READ_MAX).
	 * In legacy / ffmpeg mode each read returns VIDEO_TS_GROUP_BYTES. */
#ifdef VCPD_WITH_MPP
	static uint8_t ts_buf[VIDEO_MPP_READ_MAX];
	const size_t nal_buf_cap = 2 * VIDEO_MPP_READ_MAX;  /* fits one full ring slot + partial head */
#else
	uint8_t ts_buf[VIDEO_TS_GROUP_BYTES];
	const size_t nal_buf_cap = 64 * 1024;
#endif

	/* NAL accumulator: collect bytes from pipe/ring, split on start codes,
	 * feed complete NALs to LTS encoder so LAST_OF_FRAME aligns with NAL boundaries */
	uint8_t *nal_buf = malloc(nal_buf_cap);
	size_t nal_len = 0;

	uint64_t diag_last_ms = now_ms();
	uint32_t diag_polls = 0;
	uint32_t diag_poll_video = 0;
	uint32_t diag_video_reads = 0;
	uint32_t diag_nals = 0;
	uint32_t diag_nal_bytes = 0;
	uint32_t diag_nal_idr = 0;
	uint32_t diag_nal_p = 0;
	uint32_t diag_nal_param = 0;
	uint32_t diag_lts_pkts = 0;
	uint32_t diag_lts_idr = 0;
	uint32_t diag_lts_param = 0;
	uint32_t diag_tx_bytes = 0;
	uint32_t diag_tx_ok = 0;
	uint32_t diag_tx_fail = 0;
	uint32_t diag_fec_pkts = 0;
	uint32_t diag_pipe_bytes = 0;

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
		diag_polls++;

		uint64_t diag_now = now_ms();
		if (diag_now - diag_last_ms >= 5000) {
			char _ts[9]; { time_t _t = time(NULL); strftime(_ts, sizeof(_ts), "%H:%M:%S", localtime(&_t)); }
			fprintf(stderr, "%s vcpd: [diag] polls=%u nfds=%d video=%u/%u running=%d pipe_fd=%d"
				" | pipe=%u B"
				" | nals=%u(idr=%u p=%u prm=%u) %u B"
				" | lts=%u(idr=%u prm=%u) tx=%u/%u B fail=%u fec=%u\n",
				_ts, diag_polls, nfds, diag_poll_video, diag_video_reads,
				ctx.video.running, ctx.video.pipe_fd,
				diag_pipe_bytes,
				diag_nals, diag_nal_idr, diag_nal_p, diag_nal_param, diag_nal_bytes,
				diag_lts_pkts, diag_lts_idr, diag_lts_param,
				diag_tx_ok, diag_tx_bytes, diag_tx_fail, diag_fec_pkts);
			diag_polls = 0;
			diag_poll_video = 0;
			diag_video_reads = 0;
			diag_nals = 0;
			diag_nal_bytes = 0;
			diag_nal_idr = 0;
			diag_nal_p = 0;
			diag_nal_param = 0;
			diag_lts_pkts = 0;
			diag_lts_idr = 0;
			diag_lts_param = 0;
			diag_tx_bytes = 0;
			diag_tx_ok = 0;
			diag_tx_fail = 0;
			diag_fec_pkts = 0;
			diag_pipe_bytes = 0;
			diag_last_ms = diag_now;
		}
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (pfds[0].revents & POLLIN)
			handle_uvcp_rx(&ctx);
		else if (ctx.ulama_rx.fd < 0)
			handle_uvcp_rx(&ctx);

		uint64_t ts = now_ms();

		if (!uvcp_session_is_active(&ctx.uvcp_sess, ts) && ctx.video.running) {
			fprintf(stderr, "vcpd: lease expired, stopping video\n");
			vsrc_stop(&ctx.video);
			continue;
		}

		if (nfds > 1 && (pfds[1].revents & (POLLIN | POLLERR | POLLHUP)) && ctx.video.running) {
			diag_poll_video++;
			ssize_t n = vsrc_read(&ctx.video, ts_buf, sizeof(ts_buf));
			if (n > 0) { diag_video_reads++; diag_pipe_bytes += (uint32_t)n; }
			if (n <= 0) {
				if (n == 0 || errno == ENODEV || errno == EIO) {
					fprintf(stderr, "vcpd: video source lost (%s), entering recovery\n",
						n == 0 ? "EOF" : strerror(errno));
					vsrc_stop(&ctx.video);

					/* Recovery: wait for USB re-enumeration, restore monitor mode, restart */
					for (int retry = 0; retry < 30 && g_running; retry++) {
						usleep(1000000); /* 1s */
						/* Check if video device reappeared */
						int probe_fd = open(ctx.video.device, O_RDONLY);
						if (probe_fd >= 0) {
							close(probe_fd);
							fprintf(stderr, "vcpd: video device %s reappeared\n", ctx.video.device);
							break;
						}
					}

					/* Restore UNOW: wait for interface, set monitor mode, retry TX init */
					if (strcmp(ctx.transport_str, "unow") == 0) {
						char cmd[512];
						snprintf(cmd, sizeof(cmd),
							"for i in $(seq 1 15); do "
							"  ip link show %s >/dev/null 2>&1 && break; "
							"  sleep 1; "
							"done; "
							"sleep 2; "
							"ip link set %s down 2>/dev/null; "
							"iw dev %s set type monitor 2>/dev/null; "
							"ip link set %s up 2>/dev/null; "
							"iw dev %s set channel 6 2>/dev/null; "
							"sleep 2",
							ctx.iface, ctx.iface, ctx.iface, ctx.iface, ctx.iface);
						(void)system(cmd);

						/* Retry UNOW TX init until it works */
						ulama_transport_tx_close(&ctx.ulama_tx);
						for (int t = 0; t < 10 && g_running; t++) {
							if (ulama_transport_tx_init_unow(&ctx.ulama_tx, ctx.node_id, ctx.iface, NULL) == 0) {
								fprintf(stderr, "vcpd: UNOW TX reconnected\n");
								break;
							}
							fprintf(stderr, "vcpd: UNOW TX init retry %d/10...\n", t + 1);
							usleep(2000000);
							(void)system(cmd);
						}
					}

					/* Restart video */
					if (uvcp_session_is_active(&ctx.uvcp_sess, now_ms()) || ctx.autostart) {
						fprintf(stderr, "vcpd: restarting video source\n");
						vsrc_start(&ctx.video);
					}
				}
				continue;
			}

			/* Accumulate pipe data, extract complete NALs by start code */
			if (nal_len + (size_t)n > nal_buf_cap)
				nal_len = 0;
			memcpy(nal_buf + nal_len, ts_buf, (size_t)n);
			nal_len += (size_t)n;

			/* Scan for NALs: find pairs of start codes */
			size_t pos = 0;
			while (pos < nal_len) {
				/* Find first start code */
				size_t sc1 = nal_len; /* sentinel */
				for (size_t j = pos; j + 2 < nal_len; j++) {
					if (nal_buf[j] == 0 && nal_buf[j+1] == 0 &&
					    (nal_buf[j+2] == 1 ||
					     (j+3 < nal_len && nal_buf[j+2] == 0 && nal_buf[j+3] == 1))) {
						sc1 = j;
						break;
					}
				}
				if (sc1 >= nal_len) break;

				size_t sc1_len = (sc1+3 < nal_len && nal_buf[sc1+2] == 0) ? 4 : 3;

				/* Find second start code */
				size_t sc2 = nal_len;
				for (size_t j = sc1 + sc1_len; j + 2 < nal_len; j++) {
					if (nal_buf[j] == 0 && nal_buf[j+1] == 0 &&
					    (nal_buf[j+2] == 1 ||
					     (j+3 < nal_len && nal_buf[j+2] == 0 && nal_buf[j+3] == 1))) {
						sc2 = j;
						break;
					}
				}
				if (sc2 >= nal_len) break;

				/* Complete NAL: nal_buf[sc1..sc2) */
				uint8_t *nal_data = nal_buf + sc1;
				size_t nal_size = sc2 - sc1;
				diag_nals++;
				diag_nal_bytes += (uint32_t)nal_size;

				/* HEVC NAL type from first byte after start code */
				uint8_t hevc_nal_type = (nal_data[sc1_len] >> 1) & 0x3f;

				/* Track NAL type for diagnostics */
				if (hevc_nal_type == 19 || hevc_nal_type == 20)
					diag_nal_idr++;
				else if (hevc_nal_type >= 32 && hevc_nal_type <= 34)
					diag_nal_param++;
				else
					diag_nal_p++;

				/* Cache VPS(32)/SPS(33)/PPS(34) */
				if (hevc_nal_type >= 32 && hevc_nal_type <= 34 &&
				    nal_size <= PARAM_NAL_MAX_SIZE) {
					int idx = hevc_nal_type - 32;
					memcpy(ctx.cached_params[idx], nal_data, nal_size);
					ctx.cached_params_len[idx] = nal_size;
				}

				/* Before IDR(19,20): re-send VPS/SPS/PPS with duplication.
				 * Each param NAL sent ctx.param_dup_count times for
				 * redundancy on lossy radio. At PER=50%, probability of
				 * losing all 3 copies = 12.5% vs 50% for single send. */
				if ((hevc_nal_type == 19 || hevc_nal_type == 20) &&
				    ctx.cached_params_len[0] > 0) {
					for (int dup = 0; dup < ctx.param_dup_count; dup++) {
						for (int p = 0; p < 3; p++) {
							if (ctx.cached_params_len[p] == 0) continue;
							lts_encoded_packet_t pp[4];
							size_t np = lts_encoder_encode(&ctx.lts_enc,
								ctx.cached_params[p], ctx.cached_params_len[p],
								LTS_ENC_FLAG_KEYFRAME, pp, 4);
							diag_lts_param += (uint32_t)np;
							for (size_t i = 0; i < np; i++) {
								lts_retx_buf_store(ctx.retx_buf, &pp[i]);
								if (pp[i].len <= ULAMA_FRAME_MAX_PAYLOAD)
									(void)send_ulama_video(&ctx, pp[i].data, pp[i].len);
								if (ctx.pace_us > 0 && i + 1 < np)
									usleep(ctx.pace_us);
							}
						}
					}
					if (ctx.verbose)
						fprintf(stderr, "vcpd: sent VPS/SPS/PPS ×%d before IDR\n",
							ctx.param_dup_count);
				}

				size_t max_pkts = lts_encoder_packet_count(&ctx.lts_enc, nal_size);
				if (max_pkts == 0)
					max_pkts = 1;
				if (!ensure_lts_packet_capacity(&ctx, max_pkts)) {
					fprintf(stderr,
						"vcpd: failed to reserve %zu LTS packets for %zu-byte NAL (mtu=%zu)\n",
						max_pkts, nal_size, ctx.lts_enc.max_payload);
					diag_tx_fail++;
					pos = sc2;
					continue;
				}

				lts_encoded_packet_t *lts_pkts = ctx.lts_pkt_buf;
				bool is_idr = (hevc_nal_type == 19 || hevc_nal_type == 20);
				size_t npkts = lts_encoder_encode(&ctx.lts_enc, nal_data, nal_size,
								  is_idr ? LTS_ENC_FLAG_KEYFRAME : 0,
								  lts_pkts, max_pkts);
				if (npkts != max_pkts) {
					fprintf(stderr,
						"vcpd: short LTS encode for %zu-byte NAL: expected %zu packets, got %zu\n",
						nal_size, max_pkts, npkts);
					diag_tx_fail++;
					pos = sc2;
					continue;
				}
				if (is_idr)
					diag_lts_idr += (uint32_t)npkts;

				bool use_reliable = (ctx.reliable_mode == 2) ||
					(ctx.reliable_mode == 3 && (int)npkts >= ctx.reliable_threshold);

				diag_lts_pkts += (uint32_t)npkts;
				for (size_t i = 0; i < npkts; i++) {
					lts_retx_buf_store(ctx.retx_buf, &lts_pkts[i]);
					if (lts_pkts[i].len <= ULAMA_FRAME_MAX_PAYLOAD) {
						bool pkt_reliable = use_reliable ||
							(ctx.reliable_mode == 1 && i + 1 == npkts);
						if (send_ulama_video_ex(&ctx, lts_pkts[i].data, lts_pkts[i].len, pkt_reliable)) {
							diag_tx_ok++;
							diag_tx_bytes += (uint32_t)lts_pkts[i].len;
						} else {
							diag_tx_fail++;
						}
					} else {
						diag_tx_fail++;
					}
					if (ctx.fec_group > 0) {
						lts_encoded_packet_t fec_pkt;
						if (lts_fec_encoder_add(&ctx.fec_enc, &lts_pkts[i], &ctx.lts_enc, &fec_pkt)) {
							lts_retx_buf_store(ctx.retx_buf, &fec_pkt);
							if (fec_pkt.len <= ULAMA_FRAME_MAX_PAYLOAD) {
								if (send_ulama_video_ex(&ctx, fec_pkt.data, fec_pkt.len, use_reliable)) {
									ctx.fec_sent++;
									diag_fec_pkts++;
								} else {
									diag_tx_fail++;
								}
							} else {
								diag_tx_fail++;
							}
						}
					}
					if (i + 1 < npkts) {
						handle_uvcp_rx(&ctx);
						if (ctx.pace_us > 0)
							usleep(ctx.pace_us);
					}
				}

				if (ctx.verbose && npkts > 0)
					fprintf(stderr, "vcpd: NAL type=%u %zu bytes → %zu LTS pkts\n",
						hevc_nal_type, nal_size, npkts);

				pos = sc2;
			}

			/* Keep unprocessed tail */
			if (pos > 0 && pos < nal_len) {
				memmove(nal_buf, nal_buf + pos, nal_len - pos);
				nal_len -= pos;
			} else if (pos >= nal_len) {
				nal_len = 0;
			}

			/* Detect TX failure (interface gone or IPC overload) */
			if (tx_fail_count >= TX_FAIL_THRESHOLD) {
				tx_fail_count = 0;

				if (tk == ULAMA_TRANSPORT_KIND_RADIOD) {
					/* radiod IPC: transient EAGAIN during video
					 * burst is normal — just reset and continue,
					 * don't tear down the IPC connection */
				} else {
					fprintf(stderr, "vcpd: TX failed %u times, recovering UNOW\n", TX_FAIL_THRESHOLD);
					vsrc_stop(&ctx.video);

					char cmd[512];
					snprintf(cmd, sizeof(cmd),
						"for i in $(seq 1 15); do "
						"  ip link show %s >/dev/null 2>&1 && break; "
						"  sleep 1; "
						"done; "
						"sleep 2; "
						"ip link set %s down 2>/dev/null; "
						"iw dev %s set type monitor 2>/dev/null; "
						"ip link set %s up 2>/dev/null; "
						"iw dev %s set channel 6 2>/dev/null; "
						"sleep 2",
						ctx.iface, ctx.iface, ctx.iface, ctx.iface, ctx.iface);

					ulama_transport_tx_close(&ctx.ulama_tx);
					for (int t = 0; t < 10 && g_running; t++) {
						(void)system(cmd);
						if (ulama_transport_tx_init_unow(&ctx.ulama_tx, ctx.node_id, ctx.iface, NULL) == 0) {
							fprintf(stderr, "vcpd: UNOW TX reconnected\n");
							break;
						}
						fprintf(stderr, "vcpd: UNOW TX retry %d/10...\n", t + 1);
					}

					if (uvcp_session_is_active(&ctx.uvcp_sess, now_ms()) || ctx.autostart) {
						fprintf(stderr, "vcpd: restarting video after TX recovery\n");
						vsrc_start(&ctx.video);
					}
				}
			}
		}
	}

	fprintf(stderr, "vcpd: shutting down (nack_retx=%u retx_miss=%u fec_sent=%u)\n", ctx.nack_retx_count, ctx.nack_retx_miss, ctx.fec_sent);
	vsrc_stop(&ctx.video);
	ulama_transport_tx_close(&ctx.ulama_tx);
	ulama_transport_rx_close(&ctx.ulama_rx);
	free(ctx.lts_pkt_buf);
	free(ctx.retx_buf);

	return 0;
}
