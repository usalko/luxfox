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

#include "vcpd/uvcp.h"
#include "vcpd/video_source.h"
#include "vcpd/video_ring.h"
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
		"  --stream-id   ID         Stream ID                (default 0)\n"
		"  --autostart              Start streaming immediately (no UVCP READY needed)\n"
		"  --test        FILE       Capture raw encoder output to file and exit\n"
		"  --test-frames N          Number of frames to capture in test mode (default 50)\n"
		"  --test-pattern           Use color bars instead of camera\n"
		"  --ack-timeout US         UNOW ACK timeout in us                 (default 8000)\n"
		"  --ack-retry   N          UNOW ACK max retries                   (adaptive: 3-5)\n"
		"  --benchmark   KBPS       Benchmark mode: send synthetic data     (default 0=off)\n"
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

	/* In MPP ring mode vsrc_read returns one VENC pack per call (up to
	 * VIDEO_MPP_READ_MAX bytes).  A 1316-byte stack buffer would silently
	 * truncate every pack; use a static buffer sized to the maximum. */
#ifdef VCPD_WITH_MPP
	static uint8_t buf[VIDEO_MPP_READ_MAX];
#else
	uint8_t buf[VIDEO_TS_GROUP_BYTES];
#endif
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

typedef struct {
	video_source_t video;
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
	uint32_t ack_timeout_us;
	uint32_t ack_max_retry;
	int benchmark_kbps;
} vcpd_ctx_t;

static unsigned int tx_fail_count = 0;
#define TX_FAIL_THRESHOLD 10

/* Pack and send one already-built ULAMA frame, tracking consecutive
 * TX failures for the main loop's transport-recovery logic. */
static bool send_ulama_frame_raw(vcpd_ctx_t *ctx, const uint8_t *frame_buf, size_t frame_len)
{
	int rc = ulama_transport_tx_send(&ctx->ulama_tx, frame_buf, frame_len);
	if (rc >= 0) {
		tx_fail_count = 0;
		return true;
	}
	tx_fail_count++;
	return false;
}

/* Send a single unfragmented ULAMA_CLASS_VIDEO frame (used for UVCP PONG
 * replies and benchmark mode — both are well under ULAMA_FRAME_MAX_PAYLOAD). */
static bool send_ulama_video(vcpd_ctx_t *ctx, const uint8_t *data, size_t len)
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

	return send_ulama_frame_raw(ctx, frame_buf, frame_len);
}

/*
 * Peek at a raw HEVC bitstream to detect a keyframe (VPS NAL = type 32).
 * The ring slot for a GOP start is structured as:
 *   [00 00 00 01][VPS header byte][...][SPS NAL][PPS NAL][IDR NAL]
 * HEVC NAL type is (byte_after_start_code >> 1) & 0x3F.
 * VPS = 32, SPS = 33, PPS = 34, IDR = 19/20.
 */
static bool is_hevc_keyframe(const uint8_t *data, size_t len)
{
	if (len < 5)
		return false;
	/* 4-byte start code 00 00 00 01 */
	if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)
		return ((data[4] >> 1) & 0x3f) == 32;   /* VPS */
	/* 3-byte start code 00 00 01 */
	if (len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1)
		return ((data[3] >> 1) & 0x3f) == 32;
	return false;
}

/*
 * Fragment a complete H.265 frame (one ring slot) into ULAMA frames and send.
 * Each fragment: ULAMA header + up to ULAMA_FRAME_MAX_PAYLOAD bytes payload.
 * All fragments of one frame share the same ulama_seq so the receiver can
 * reassemble them via frag_reassembly_insert().
 */
static void send_video_frame(vcpd_ctx_t *ctx, const uint8_t *data, size_t len)
{
	if (len == 0)
		return;

	const size_t mtu = ULAMA_FRAME_MAX_PAYLOAD;
	size_t nfrags = (len + mtu - 1) / mtu;
	if (nfrags > 255) {
		/* Frame too large — should never happen with VIDEO_RING_SLOT_MAX = 64KB */
		fprintf(stderr, "vcpd: frame too large (%zu bytes), dropping\n", len);
		return;
	}

	bool keyframe = is_hevc_keyframe(data, len);
	uint16_t seq  = ctx->ulama_seq++;
	uint8_t  kf_flag = keyframe ? ULAMA_FLAG_VIDEO_KEYFRAME : 0;

	uint8_t frame_buf[ULAMA_FRAME_HEADER_SIZE + ULAMA_FRAME_MAX_PAYLOAD];

	for (size_t i = 0; i < nfrags; i++) {
		size_t off   = i * mtu;
		size_t chunk = (off + mtu <= len) ? mtu : (len - off);
		bool   last  = (i + 1 == nfrags);

		ulama_frame_view_t uf = {
			.src_node      = ctx->node_id,
			.dst_node      = ctx->dst_node,
			.flags         = ULAMA_FLAG_FRAGMENT | ULAMA_FLAG_DUP_ALLOWED | kf_flag
					 | (last ? ULAMA_FLAG_LAST_FRAGMENT : 0),
			.traffic_class = ULAMA_CLASS_VIDEO,
			.seq           = seq,
			.frag_idx      = (uint8_t)i,
			.frag_total    = (uint8_t)nfrags,
			.ttl           = ULAMA_FRAME_DEFAULT_TTL,
			.payload       = data + off,
			.payload_len   = chunk,
		};

		size_t frame_len = 0;
		if (!ulama_frame_pack(&uf, frame_buf, sizeof(frame_buf), &frame_len)) {
			fprintf(stderr, "vcpd: ulama_frame_pack failed for fragment %zu/%zu\n",
				i + 1, nfrags);
			continue;
		}

		if (ctx->verbose && i == 0)
			fprintf(stderr, "vcpd: video frame %zu bytes → %zu frags, keyframe=%d\n",
				len, nfrags, keyframe);

		(void)send_ulama_frame_raw(ctx, frame_buf, frame_len);
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
			vsrc_request_idr(&ctx->video);
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
	size_t mtu = ULAMA_FRAME_MAX_PAYLOAD;
	uint64_t target_bps = (uint64_t)ctx->benchmark_kbps * 1000;
	uint64_t target_Bps = target_bps / 8;
	double pps_target = (double)target_Bps / (double)mtu;
	uint64_t interval_us = pps_target > 0 ? (uint64_t)(1000000.0 / pps_target) : 10000;

	fprintf(stderr, "vcpd: BENCHMARK — target %d Kbit/s, mtu=%zu, interval=%llu us (%.0f pps)\n",
		ctx->benchmark_kbps, mtu,
		(unsigned long long)interval_us, pps_target);

	uint8_t dummy[ULAMA_FRAME_MAX_PAYLOAD];
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

		if (send_ulama_video(ctx, dummy, mtu)) {
			pkts_sent++;
			bytes_sent += mtu;
			rpt_pkts++;
			rpt_bytes += (uint32_t)mtu;
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
		else if (!strcmp(key, "ack_timeout_us"))     ctx->ack_timeout_us = (uint32_t)atoi(val);
		else if (!strcmp(key, "ack_retry"))          ctx->ack_max_retry = (uint32_t)atoi(val);
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
	ctx.ack_timeout_us = 2000;
	ctx.ack_max_retry = 0;
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
		{"ack-timeout",          required_argument, NULL, 'K'},
		{"ack-retry",            required_argument, NULL, 'Y'},
		{"benchmark",            required_argument, NULL, 'B'},
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
		case 'K': ctx.ack_timeout_us = (uint32_t)atoi(optarg); break;
		case 'Y': ctx.ack_max_retry = (uint32_t)atoi(optarg); break;
		case 'B': ctx.benchmark_kbps = atoi(optarg); break;
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
	fprintf(stderr, "vcpd: adaptive defaults for %d fps: gop=%d ack_retry=%u\n",
		fps, ctx.video.gop, ctx.ack_max_retry);

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

	uvcp_session_init(&ctx.uvcp_sess, UVCP_LEASE_DEFAULT_MS);

	fprintf(stderr,
		"vcpd: build #%d (%s@%s) %s\n"
		"vcpd: --- config ---\n"
		"vcpd:   source     = %s\n"
		"vcpd:   video      = %dx%d  fps=%d  gop=%d  bitrate=%d Kbit/s\n"
		"vcpd:   transport  = %s  node=%u → dst=%u  stream=%u\n"
		"vcpd:   ack        = %u us / %u retry\n"
		"vcpd:   autostart  = %s\n"
		"vcpd: ---------------\n",
		ULAMA_BUILD_NUMBER, ULAMA_GIT_BRANCH, ULAMA_GIT_HASH, ULAMA_BUILD_DATE,
		ctx.video.device,
		ctx.video.width, ctx.video.height, ctx.video.fps, ctx.video.gop, ctx.video.bitrate_kbps,
		ulama_transport_kind_name(tk), ctx.node_id, ctx.dst_node, ctx.stream_id,
		ctx.ack_timeout_us, ctx.ack_max_retry,
		ctx.autostart ? "true" : "false");

	if (ctx.benchmark_kbps > 0) {
		int rc = run_benchmark(&ctx);
		ulama_transport_tx_close(&ctx.ulama_tx);
		ulama_transport_rx_close(&ctx.ulama_rx);
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

	/* In MPP ring mode each read returns one whole H.265 frame (up to
	 * VIDEO_MPP_READ_MAX). In legacy / ffmpeg mode each read returns
	 * VIDEO_TS_GROUP_BYTES. */
#ifdef VCPD_WITH_MPP
	static uint8_t ts_buf[VIDEO_MPP_READ_MAX];
#else
	uint8_t ts_buf[VIDEO_TS_GROUP_BYTES];
#endif

	uint64_t diag_last_ms = now_ms();
	uint32_t diag_polls = 0;
	uint32_t diag_poll_video = 0;
	uint32_t diag_video_reads = 0;
	uint32_t diag_video_bytes = 0;
	uint32_t diag_tx_bytes = 0;
	uint32_t diag_tx_ok = 0;
	uint32_t diag_tx_fail = 0;

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
				" | frames=%u B"
				" | tx=%u/%u B fail=%u\n",
				_ts, diag_polls, nfds, diag_poll_video, diag_video_reads,
				ctx.video.running, ctx.video.pipe_fd,
				diag_video_bytes,
				diag_tx_ok, diag_tx_bytes, diag_tx_fail);
			diag_polls = 0;
			diag_poll_video = 0;
			diag_video_reads = 0;
			diag_video_bytes = 0;
			diag_tx_bytes = 0;
			diag_tx_ok = 0;
			diag_tx_fail = 0;
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
			if (n > 0) { diag_video_reads++; diag_video_bytes += (uint32_t)n; }
			if (n <= 0) {
				if (n == 0 || errno == ENODEV || errno == EIO) {
					fprintf(stderr, "vcpd: video source lost (%s), entering recovery\n",
						n == 0 ? "EOF" : strerror(errno));
					vsrc_stop(&ctx.video);

					/* Recovery: wait for USB re-enumeration.
					 * Poll ulama_rx during the wait so UVCP PING/PONG keep working. */
					for (int retry = 0; retry < 30 && g_running; retry++) {
						struct pollfd rpfd = { .fd = ctx.ulama_rx.fd, .events = POLLIN };
						poll(&rpfd, 1, 1000); /* 1s timeout = same cadence as before */
						if (rpfd.revents & POLLIN)
							handle_uvcp_rx(&ctx);
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

						/* Retry UNOW TX init until it works (poll during waits). */
						ulama_transport_tx_close(&ctx.ulama_tx);
						for (int t = 0; t < 10 && g_running; t++) {
							if (ulama_transport_tx_init_unow(&ctx.ulama_tx, ctx.node_id, ctx.iface, NULL) == 0) {
								fprintf(stderr, "vcpd: UNOW TX reconnected\n");
								break;
							}
							fprintf(stderr, "vcpd: UNOW TX init retry %d/10...\n", t + 1);
							(void)system(cmd);
							struct pollfd rpfd = { .fd = ctx.ulama_rx.fd, .events = POLLIN };
							poll(&rpfd, 1, 2000);
							if (rpfd.revents & POLLIN)
								handle_uvcp_rx(&ctx);
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

			/* One vsrc_read() call = one complete H.265 frame (full ring slot). */
			send_video_frame(&ctx, ts_buf, (size_t)n);
			diag_tx_ok++;
			diag_tx_bytes += (uint32_t)n;

			/* Detect TX failure (interface gone or IPC overload) */
			if (tx_fail_count >= TX_FAIL_THRESHOLD) {
				diag_tx_fail++;
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

	fprintf(stderr, "vcpd: shutting down\n");
	vsrc_stop(&ctx.video);
	ulama_transport_tx_close(&ctx.ulama_tx);
	ulama_transport_rx_close(&ctx.ulama_rx);

	return 0;
}
