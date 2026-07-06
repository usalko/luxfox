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
#include "ulama/ulama_frame.h"
#include "ulama/ulama_version.h"
#include "ulama/transport.h"
#include "unow/radio_unow.h"
#include "unow/unow_wire.h"

#define UVCP_PREFIX "UVCP/1 "
#define UVCP_PREFIX_LEN 7

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
	fprintf(stderr, "ulama-gw: build #%d (%s@%s) %s\n",
		ULAMA_BUILD_NUMBER, ULAMA_GIT_BRANCH, ULAMA_GIT_HASH, ULAMA_BUILD_DATE);
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  --cascade-in  ADDR:PORT  Listen for cascade-core outbound (default 127.0.0.1:5601)\n"
		"  --cascade-out ADDR:PORT  Send to cascade-core inbound     (default 127.0.0.1:5600)\n"
		"  --transport   udp|unow|radiod  Radio transport             (default udp)\n"
		"  --listen      ADDR:PORT  Listen for ULAMA frames (UDP)     (default 0.0.0.0:5000)\n"
		"  --peer        ADDR:PORT  Send ULAMA frames to (UDP)        (default 127.0.0.1:5001)\n"
		"  --iface       NAME       Monitor mode interface (UNOW)     (default mon0)\n"
		"  --channel     N          WiFi channel; auto-configures iface before start (UNOW)\n"
		"  --tx-rate-mbps N        Legacy radiotap TX rate for UNOW uplink (default 6)\n"
		"  --node        ID         Gateway node ID (1-253)           (default 254)\n"
		"  --dst-mac     MAC        Destination MAC for UNOW TX       (broadcast if omitted)\n"
		"  --verbose                Enable verbose logging\n"
		"  --help                   Show this help\n",
		prog);
}

#define GW_MAX_NODES 254
#define GW_VIDEO_REORDER_SLOTS 32
/*
 * Reorder hold for a sequence hole where NOTHING is in flight for that seq at
 * all (no fragment of it ever arrived) — genuinely nothing to wait for, so
 * keep it short: a real loss should only add a little latency before we skip
 * ahead.
 *
 * Was 250ms, then 400ms (field logs 2026-07-06 showed "reorder skip
 * ... waited=250-253ms" firing constantly at frag_loss=0% — the frame wasn't
 * lost, it just hadn't arrived when the gate gave up). But ANY fixed hold
 * shorter than frag_reassembly's own FRAG_TIMEOUT_MS (800ms) has the same
 * problem for a frame that IS still actively reassembling: the reorder gate
 * declares a gap before reassembly itself would give up. See
 * reassembly_has_pending_frame() below — while a frame is still in flight we
 * wait out its actual reassembly timeout instead of this fixed hold. Every
 * false gap sets wait_for_keyframe and blanks every frame until the next
 * keyframe (up to ~1s at gop=25/1000ms IDR), which was the real cause of the
 * choppiness, not the underlying RF loss.
 */
#define GW_VIDEO_REORDER_HOLD_MS 400
#define GW_VIDEO_DONE_WINDOW 64
/*
 * Control refresh interval for held-stick RC state.
 *
 * Real command responsiveness is handled by the change-driven path in
 * handle_cascade_rx(): any changed control payload is forwarded to radio
 * IMMEDIATELY. This keepalive only refreshes a HELD (unchanged) stick so the
 * drone's CTRL watchdog (2000 ms) stays fed.
 *
 * History of the rate:
 *  - 20 ms (50 Hz) forwarding every cascade duplicate → heavy uplink that
 *    collided with the drone's downlink video on the half-duplex channel.
 *  - 200 ms (5 Hz) and later 30 ms (33 Hz) both still tripped "CTRL link LOST".
 *    That was NOT a rate problem: the keepalive frame was addressed to dst=0,
 *    which the drone does not count as "CTRL for us", so it never fed the
 *    watchdog regardless of rate. Fixed by addressing the keepalive to the real
 *    control destination (see maybe_send_ctrl_keepalive).
 *
 * With correct addressing, a held stick only needs to feed a 2000 ms watchdog,
 * so 100 ms (10 Hz) is used: ~20x safety margin against burst loss, while
 * cutting steady-state uplink ~3x vs 33 Hz to reduce contention with downlink
 * video. Responsiveness is unaffected (real moves go change-driven). The durable
 * fix for uplink/downlink contention is still TDMA (Phase 8).
 */
#define GW_CTRL_KEEPALIVE_MS 100

typedef struct {
	uint32_t rx_pkts;
	uint32_t tx_pkts;
	uint32_t rx_bytes;
	uint32_t tx_bytes;
	int32_t  rssi_sum;
	uint32_t rssi_count;
} gw_node_stats_t;

typedef struct {
	uint32_t ulama_rx_video;
	uint32_t ulama_rx_telem;
	uint32_t ulama_rx_ctrl;
	uint32_t ctrl_tx;
	uint64_t video_bytes_out;
	uint64_t last_print_ms;
	int32_t rssi_sum;
	uint32_t rssi_count;
	/* Direct video delivery (replaces LTS/NAL-assembly stats) */
	uint32_t video_frames_rx;         /* fully assembled frames forwarded to cascade */
	uint32_t video_keyframes_rx;      /* fully assembled keyframes */
	uint32_t video_frags_rx;          /* video fragments received */
	uint32_t video_frags_expected;    /* frag_total tallied once per frame (completed or expired) */
	uint32_t video_frags_missing;     /* fragments never received via ANY of the redundant copies */
	uint32_t video_frames_dropped;    /* incomplete frames that failed to reassemble */
	uint32_t video_frames_incomplete; /* reassembly slots expired before completion */
	uint32_t video_keyframes_lost;    /* incomplete reassembly slots flagged as keyframes */
	uint32_t video_keyframe_flushes;  /* stale P-frame reassembly flushed on IDR */
	uint32_t video_reorder_skips;     /* skipped seq holes after short hold timeout */
	uint32_t video_wait_keyframe_drops; /* completed P-frames dropped while waiting for IDR */
	uint32_t video_reorder_full_drops;  /* completed frames dropped because reorder queue was full */
	uint32_t cascade_out_frames;
	gw_node_stats_t nodes[GW_MAX_NODES];
} gw_stats_t;

typedef struct {
	bool active;
	bool keyframe;
	uint16_t seq;
	uint16_t src_u16;
	uint64_t ready_ms;
	size_t len;
	uint8_t data[CASCADE_FRAME_MAX_PAYLOAD];
} video_reorder_slot_t;

typedef struct {
	bool primed;
	uint16_t src_u16;
	uint16_t next_seq;
	video_reorder_slot_t slots[GW_VIDEO_REORDER_SLOTS];
} video_reorder_ctx_t;

typedef struct {
	bool active;
	uint8_t src_node;
	uint16_t seq;
} video_done_key_t;

typedef struct {
	gw_config_t gw;
	char listen_addr[64];
	char peer_addr[64];
	char dst_mac_str[32];
	bool verbose;
	int channel;
	uint8_t tx_rate_500kbps;
	uint16_t seq_counter;
	int cascade_rx_fd;
	struct sockaddr_in cascade_tx_addr;
	int cascade_tx_fd;
	ulama_tx_transport_t ulama_tx;
	ulama_rx_transport_t ulama_rx;
	frag_reassembly_ctx_t reassembly;
	video_reorder_ctx_t video_reorder;
	video_done_key_t video_done[GW_VIDEO_DONE_WINDOW];
	uint16_t video_done_head;
	bool wait_for_keyframe;
	bool idr_request_in_flight;
	uint8_t last_ctrl_payload[CASCADE_FRAME_MAX_PAYLOAD];
	size_t last_ctrl_len;
	uint16_t last_ctrl_src;
	uint16_t last_ctrl_dst;
	uint64_t last_ctrl_tx_ms;
	bool has_last_ctrl;
	gw_stats_t stats;
} app_ctx_t;

static bool slot_is_video(const frag_reassembly_slot_t *slot);
static bool slot_has_keyframe_fragment(const frag_reassembly_slot_t *slot);

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

/*
 * Send a single cascade frame over ULAMA radio.
 * Handles fragmentation for large payloads.
 */
static void cascade_to_ulama_tx(app_ctx_t *ctx, const cascade_frame_view_t *cf)
{
	uint8_t ulama_class = gw_class_cascade_to_ulama(cf->traffic_class);
	uint8_t dst_node = gw_addr_u16_to_u8(&ctx->gw, cf->dst);

	if (ulama_class == ULAMA_CLASS_CTRL)
		ctx->stats.ctrl_tx++;

	/* Per-node TX stats */
	if (dst_node > 0 && dst_node < GW_MAX_NODES) {
		ctx->stats.nodes[dst_node].tx_pkts++;
		ctx->stats.nodes[dst_node].tx_bytes += (uint32_t)cf->payload_len;
	}

	if (cf->payload_len <= ULAMA_FRAME_MAX_PAYLOAD) {
		ulama_frame_view_t uf = {
			.src_node = ctx->gw.node_id,
			.dst_node = dst_node,
			.flags = ULAMA_FLAG_DUP_ALLOWED,
			.traffic_class = ulama_class,
			.seq = ctx->seq_counter++,
			.frag_idx = 0,
			.frag_total = 1,
			.ttl = ULAMA_FRAME_DEFAULT_TTL,
			.payload = cf->payload,
			.payload_len = cf->payload_len,
		};

		uint8_t frame_buf[ULAMA_FRAME_HEADER_SIZE + ULAMA_FRAME_MAX_PAYLOAD];
		size_t frame_len = 0;
		if (ulama_frame_pack(&uf, frame_buf, sizeof(frame_buf), &frame_len)) {
			/* CTRL → reliable, everything else → unreliable */
			if (ulama_class == ULAMA_CLASS_CTRL)
				ulama_transport_tx_send_reliable(&ctx->ulama_tx, frame_buf, frame_len);
			else
				ulama_transport_tx_send(&ctx->ulama_tx, frame_buf, frame_len);
		}
	} else {
		uint8_t frag_payloads[FRAG_MAX_FRAGMENTS][ULAMA_FRAME_MAX_PAYLOAD];
		size_t frag_sizes[FRAG_MAX_FRAGMENTS];
		size_t nfrags = frag_split(cf->payload, cf->payload_len, frag_payloads, frag_sizes, FRAG_MAX_FRAGMENTS);

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

/*
 * TDMA-prioritized cascade RX: process CTRL frames first (immediate),
 * then other classes (rate-limited by the caller's TX budget).
 */
static void handle_cascade_rx(app_ctx_t *ctx)
{
  for (int drain = 0; drain < 16; drain++) {
	uint8_t buf[CASCADE_FRAME_HEADER_SIZE + CASCADE_FRAME_MAX_PAYLOAD];
	ssize_t n = recv(ctx->cascade_rx_fd, buf, sizeof(buf), MSG_DONTWAIT);
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

	if (cf.traffic_class == CASCADE_CLASS_CONTROL &&
	    cf.payload_len <= sizeof(ctx->last_ctrl_payload)) {
		bool changed = !ctx->has_last_ctrl ||
			ctx->last_ctrl_len != cf.payload_len ||
			memcmp(ctx->last_ctrl_payload, cf.payload, cf.payload_len) != 0;

		memcpy(ctx->last_ctrl_payload, cf.payload, cf.payload_len);
		ctx->last_ctrl_len = cf.payload_len;
		/* Remember where real control is addressed so the keepalive can reuse it.
		 * The drone only feeds its CTRL watchdog for frames whose dst_node is its
		 * own id or broadcast; a keepalive sent to dst=0 is ignored by that check
		 * and lets the 2 s watchdog trip (CTRL LOST -> video suppressed). */
		ctx->last_ctrl_src = cf.src;
		ctx->last_ctrl_dst = cf.dst;
		ctx->has_last_ctrl = true;

		/* Forward control immediately only when it actually changed.
		 * Unchanged RC payloads are already covered by the gateway heartbeat
		 * replay path (maybe_send_ctrl_keepalive), so re-forwarding every
		 * duplicate from cascade-core just creates bidirectional contention. */
		if (changed) {
			cascade_to_ulama_tx(ctx, &cf);
			ctx->last_ctrl_tx_ms = now_ms();
		}
		continue;
	}

	cascade_to_ulama_tx(ctx, &cf);
  }
}

static void maybe_send_ctrl_keepalive(app_ctx_t *ctx, uint64_t now_ms_value)
{
	if (!ctx->has_last_ctrl || ctx->last_ctrl_len == 0)
		return;
	if ((now_ms_value - ctx->last_ctrl_tx_ms) < GW_CTRL_KEEPALIVE_MS)
		return;

	cascade_frame_view_t cf = {
		.version = CASCADE_FRAME_VERSION,
		.src = ctx->last_ctrl_src,
		.dst = ctx->last_ctrl_dst,
		.traffic_class = CASCADE_CLASS_CONTROL,
		.payload = ctx->last_ctrl_payload,
		.payload_len = ctx->last_ctrl_len,
	};

	cascade_to_ulama_tx(ctx, &cf);
	ctx->last_ctrl_tx_ms = now_ms_value;
}

static bool send_cascade_frame(app_ctx_t *ctx, const cascade_frame_view_t *cf)
{
	size_t buf_size = CASCADE_FRAME_HEADER_SIZE + cf->payload_len;
	uint8_t *buf = (uint8_t *)malloc(buf_size);
	if (!buf) return false;
	size_t len = 0;
	bool ok = false;
	if (cascade_frame_pack(cf, buf, buf_size, &len)) {
		ssize_t sent = sendto(ctx->cascade_tx_fd, buf, len, 0,
				      (struct sockaddr *)&ctx->cascade_tx_addr, sizeof(ctx->cascade_tx_addr));
		if (sent == (ssize_t)len) {
			ok = true;
		} else {
			fprintf(stderr, "gw: cascade-out sendto failed for %zu-byte frame (class=%u): %s\n",
				cf->payload_len, cf->traffic_class, strerror(errno));
		}
	} else {
		fprintf(stderr, "gw: cascade_frame_pack rejected %zu-byte payload (class=%u, max=%u)\n",
			cf->payload_len, cf->traffic_class, CASCADE_FRAME_MAX_PAYLOAD);
	}
	free(buf);
	return ok;
}

static void request_remote_idr(app_ctx_t *ctx)
{
	/*
	 * UVCP READY already causes vcpd to request an IDR when the stream is active.
	 * Reuse it as a lightweight recovery trigger after a detected video gap.
	 */
	static const uint8_t ready_msg[] = "UVCP/1 READY\n";
	ulama_frame_view_t uf = {
		.src_node = ctx->gw.node_id,
		.dst_node = 1,
		.flags = ULAMA_FLAG_DUP_ALLOWED,
		.traffic_class = ULAMA_CLASS_CTRL,
		.seq = ctx->seq_counter++,
		.frag_idx = 0,
		.frag_total = 1,
		.ttl = ULAMA_FRAME_DEFAULT_TTL,
		.payload = ready_msg,
		.payload_len = sizeof(ready_msg) - 1,
	};

	uint8_t frame_buf[ULAMA_FRAME_HEADER_SIZE + 32];
	size_t frame_len = 0;
	if (ulama_frame_pack(&uf, frame_buf, sizeof(frame_buf), &frame_len))
		(void)ulama_transport_tx_send_reliable(&ctx->ulama_tx, frame_buf, frame_len);
}

static int16_t seq_delta(uint16_t newer, uint16_t older)
{
	return (int16_t)(newer - older);
}

static bool hevc_nal_is_random_access(uint8_t nal_type)
{
	return nal_type == 32 ||
	       (nal_type >= 16 && nal_type <= 21);
}

/* Detect a HEVC random-access frame. SmartP / forced recovery may start
 * directly with IDR/CRA/BLA instead of VPS, so scan the first few Annex-B NALs. */
static bool video_is_keyframe(const uint8_t *data, size_t len)
{
	if (data == NULL || len < 5)
		return false;

	int scanned = 0;
	for (size_t i = 0; i + 4 < len && scanned < 8; i++) {
		size_t hdr = 0;
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
			hdr = i + 3;
		} else if (i + 4 < len && data[i] == 0 && data[i + 1] == 0 &&
			   data[i + 2] == 0 && data[i + 3] == 1) {
			hdr = i + 4;
		} else {
			continue;
		}
		if (hdr >= len)
			break;
		if (hevc_nal_is_random_access((data[hdr] >> 1) & 0x3f))
			return true;
		scanned++;
		i = hdr;
	}

	return false;
}

static bool payload_is_uvcp_control(const uint8_t *data, size_t len)
{
	return data != NULL && len >= UVCP_PREFIX_LEN &&
		memcmp(data, UVCP_PREFIX, UVCP_PREFIX_LEN) == 0;
}

static bool video_done_contains(const app_ctx_t *ctx, uint8_t src_node, uint16_t seq)
{
	for (size_t i = 0; i < GW_VIDEO_DONE_WINDOW; i++) {
		const video_done_key_t *key = &ctx->video_done[i];
		if (key->active && key->src_node == src_node && key->seq == seq)
			return true;
	}
	return false;
}

static void video_done_add(app_ctx_t *ctx, uint8_t src_node, uint16_t seq)
{
	video_done_key_t *key = &ctx->video_done[ctx->video_done_head];
	key->active = true;
	key->src_node = src_node;
	key->seq = seq;
	ctx->video_done_head = (uint16_t)((ctx->video_done_head + 1U) % GW_VIDEO_DONE_WINDOW);
}

static void video_reorder_reset(video_reorder_ctx_t *vr)
{
	if (vr == NULL)
		return;
	memset(vr, 0, sizeof(*vr));
}

static void video_reorder_drop_older_than(video_reorder_ctx_t *vr,
					  uint16_t min_seq)
{
	if (vr == NULL || !vr->primed)
		return;

	for (int i = 0; i < GW_VIDEO_REORDER_SLOTS; i++) {
		video_reorder_slot_t *slot = &vr->slots[i];

		if (!slot->active || slot->src_u16 != vr->src_u16)
			continue;
		if (seq_delta(slot->seq, min_seq) < 0)
			slot->active = false;
	}
}

static uint8_t frag_slot_received_count(const frag_reassembly_slot_t *slot)
{
	uint8_t count = 0;

	if (slot == NULL || !slot->active)
		return 0;

	for (uint8_t i = 0; i < slot->frag_total; i++) {
		if (slot->received_mask & (1u << i))
			count++;
	}

	return count;
}

static void frag_slot_missing_list(const frag_reassembly_slot_t *slot,
				   char *buf, size_t buf_size)
{
	size_t used = 0;

	if (buf == NULL || buf_size == 0)
		return;
	buf[0] = '\0';
	if (slot == NULL || !slot->active)
		return;

	for (uint8_t i = 0; i < slot->frag_total; i++) {
		if (slot->received_mask & (1u << i))
			continue;
		int written = snprintf(buf + used, buf_size - used,
			used == 0 ? "%u" : ",%u", i);
		if (written < 0 || (size_t)written >= buf_size - used)
			break;
		used += (size_t)written;
	}
}

static unsigned int video_reorder_active_count(const video_reorder_ctx_t *vr)
{
	unsigned int active = 0;

	if (vr == NULL)
		return 0;

	for (int i = 0; i < GW_VIDEO_REORDER_SLOTS; i++) {
		if (vr->slots[i].active)
			active++;
	}

	return active;
}

/* Compute simple checksum for debugging packet loss */
static uint32_t compute_frag_checksum(const uint8_t *data, size_t len)
{
	uint32_t checksum = 0;
	for (size_t i = 0; i < len; i++)
		checksum = (checksum * 31 + data[i]) & 0xFFFFFFFFU;
	return checksum;
}

static void log_video_frag_received(uint8_t src_node, uint16_t seq, uint8_t frag_idx,
				    uint8_t frag_total, const uint8_t *payload,
				    size_t payload_len, uint32_t received_mask)
{
	static uint32_t trace_count;
	uint32_t checksum = compute_frag_checksum(payload, payload_len);
	char recv_mask_str[64];
	int pos = 0;

	/* Only log every Nth fragment to avoid spam */
	if (!(trace_count < 8 || (trace_count % 256U) == 0U)) {
		trace_count++;
		return;
	}

	/* Build received fragment mask string */
	recv_mask_str[0] = '\0';
	for (int i = 0; i < frag_total && i < 32; i++) {
		if (received_mask & (1u << i)) {
			pos += sprintf(recv_mask_str + pos, "%d,", i);
		}
	}
	if (pos > 0)
		recv_mask_str[pos - 1] = '\0';

	fprintf(stderr,
		"gw: video frag rx src=%u seq=%u frag=%u/%u checksum=0x%08x payload_len=%zu received=[%s]\n",
		src_node, seq, frag_idx, frag_total, checksum, payload_len, recv_mask_str);
	trace_count++;
}

static void log_video_frag_missing(const frag_reassembly_slot_t *slot,
				   uint64_t now_ms)
{
	static uint32_t trace_count;
	char missing[128];
	char received[128];
	int recv_pos = 0;

	if (slot == NULL)
		return;

	if (!(trace_count < 16 || (trace_count % 256U) == 0U)) {
		trace_count++;
		return;
	}

	/* List missing fragments */
	frag_slot_missing_list(slot, missing, sizeof(missing));

	/* List received fragments with their checksums */
	received[0] = '\0';
	for (int i = 0; i < slot->frag_total && i < 32; i++) {
		if (slot->received_mask & (1u << i)) {
			uint32_t cs = compute_frag_checksum(
				slot->fragments[i].payload,
				slot->fragments[i].payload_len);
			recv_pos += sprintf(received + recv_pos, "%u:0x%04x,", i, cs & 0xFFFFU);
		}
	}
	if (recv_pos > 0)
		received[recv_pos - 1] = '\0';

	fprintf(stderr,
		"gw: video frag missing src=%u seq=%u age=%llums recv=%u/%u missing=[%s] checksums=[%s] keyframe=%u\n",
		slot->src_node, slot->seq,
		(unsigned long long)(now_ms - slot->first_ts_ms),
		(unsigned)frag_slot_received_count(slot),
		(unsigned)slot->frag_total,
		missing,
		received,
		slot_has_keyframe_fragment(slot) ? 1U : 0U);
	trace_count++;
}

static void log_video_reassembly_expire(const frag_reassembly_slot_t *slot,
					uint64_t now_ms)
{
	static uint32_t trace_count;
	char missing[128];

	if (slot == NULL)
		return;
	if (!(trace_count < 32 || (trace_count % 128U) == 0U)) {
		trace_count++;
		return;
	}

	frag_slot_missing_list(slot, missing, sizeof(missing));
	fprintf(stderr,
		"gw: video reassembly expire src=%u seq=%u age=%llums recv=%u/%u missing=[%s] keyframe=%u\n",
		slot->src_node,
		slot->seq,
		(unsigned long long)(now_ms - slot->first_ts_ms),
		(unsigned)frag_slot_received_count(slot),
		(unsigned)slot->frag_total,
		missing,
		slot_has_keyframe_fragment(slot) ? 1U : 0U);
	trace_count++;
}

static void log_video_reorder_skip(const app_ctx_t *ctx,
				   uint16_t expected_seq,
				   uint16_t next_ready_seq,
				   uint32_t hold_ms,
				   bool pending,
				   uint64_t wait_ms)
{
	static uint32_t trace_count;

	if (!(trace_count < 32 || (trace_count % 128U) == 0U)) {
		trace_count++;
		return;
	}

	fprintf(stderr,
		"gw: video reorder skip src=%u expect=%u have=%u hold=%u waited=%llums pending=%u active=%u\n",
		(unsigned)gw_addr_u16_to_u8(&ctx->gw, ctx->video_reorder.src_u16),
		expected_seq,
		next_ready_seq,
		hold_ms,
		(unsigned long long)wait_ms,
		pending ? 1U : 0U,
		video_reorder_active_count(&ctx->video_reorder));
	trace_count++;
}

static void log_video_reorder_full(const app_ctx_t *ctx,
				   uint16_t seq,
				   bool keyframe)
{
	static uint32_t trace_count;

	if (!(trace_count < 32 || (trace_count % 128U) == 0U)) {
		trace_count++;
		return;
	}

	fprintf(stderr,
		"gw: video reorder full src=%u seq=%u keyframe=%u next=%u active=%u wait_kf=%u\n",
		(unsigned)gw_addr_u16_to_u8(&ctx->gw, ctx->video_reorder.src_u16),
		seq,
		keyframe ? 1U : 0U,
		ctx->video_reorder.next_seq,
		video_reorder_active_count(&ctx->video_reorder),
		ctx->wait_for_keyframe ? 1U : 0U);
	trace_count++;
}

static video_reorder_slot_t *video_reorder_find(video_reorder_ctx_t *vr, uint16_t seq)
{
	if (vr == NULL)
		return NULL;

	for (int i = 0; i < GW_VIDEO_REORDER_SLOTS; i++) {
		video_reorder_slot_t *slot = &vr->slots[i];
		if (slot->active && slot->seq == seq)
			return slot;
	}

	return NULL;
}

static video_reorder_slot_t *video_reorder_find_next_ready(video_reorder_ctx_t *vr)
{
	video_reorder_slot_t *best = NULL;

	if (vr == NULL || !vr->primed)
		return NULL;

	for (int i = 0; i < GW_VIDEO_REORDER_SLOTS; i++) {
		video_reorder_slot_t *slot = &vr->slots[i];
		if (!slot->active || slot->src_u16 != vr->src_u16)
			continue;
		if (seq_delta(slot->seq, vr->next_seq) < 0)
			continue;
		if (best == NULL || seq_delta(best->seq, slot->seq) > 0)
			best = slot;
	}

	return best;
}

static video_reorder_slot_t *video_reorder_alloc(video_reorder_ctx_t *vr)
{
	if (vr == NULL)
		return NULL;

	for (int i = 0; i < GW_VIDEO_REORDER_SLOTS; i++) {
		if (!vr->slots[i].active)
			return &vr->slots[i];
	}

	return NULL;
}

static video_reorder_slot_t *video_reorder_reclaim_for_keyframe(video_reorder_ctx_t *vr)
{
	video_reorder_slot_t *best = NULL;

	if (vr == NULL)
		return NULL;

	for (int i = 0; i < GW_VIDEO_REORDER_SLOTS; i++) {
		video_reorder_slot_t *slot = &vr->slots[i];
		if (!slot->active || slot->keyframe)
			continue;
		if (best == NULL || slot->ready_ms < best->ready_ms)
			best = slot;
	}

	return best;
}

static bool slot_is_video(const frag_reassembly_slot_t *slot)
{
	if (slot == NULL || !slot->active)
		return false;

	for (uint8_t i = 0; i < slot->frag_total; i++) {
		if (!(slot->received_mask & (1u << i)))
			continue;
		if (slot->fragments[i].traffic_class == ULAMA_CLASS_VIDEO)
			return true;
	}

	return false;
}

static bool slot_has_keyframe_fragment(const frag_reassembly_slot_t *slot)
{
	if (slot == NULL || !slot->active)
		return false;

	for (uint8_t i = 0; i < slot->frag_total; i++) {
		if (!(slot->received_mask & (1u << i)))
			continue;
		if (slot->fragments[i].flags & ULAMA_FLAG_VIDEO_KEYFRAME)
			return true;
	}

	return false;
}

static void expire_reassembly_slots(app_ctx_t *ctx, uint64_t now_ms)
{
	for (int i = 0; i < FRAG_MAX_SLOTS; i++) {
		frag_reassembly_slot_t *slot = &ctx->reassembly.slots[i];

		if (!slot->active || (now_ms - slot->first_ts_ms) <= FRAG_TIMEOUT_MS)
			continue;

		if (slot_is_video(slot)) {
			ctx->stats.video_frames_incomplete++;
			if (slot_has_keyframe_fragment(slot))
				ctx->stats.video_keyframes_lost++;
			/* Log missing fragments with checksums of received ones */
			log_video_frag_missing(slot, now_ms);
			log_video_reassembly_expire(slot, now_ms);

			/* This frame never completed: tally how many of its
			 * fragments never arrived via ANY of the ~3 redundant
			 * transmission attempts, for a real (not assumed) per-
			 * fragment loss rate — see video_frags_missing in [stats]. */
			ctx->stats.video_frags_expected += slot->frag_total;
			ctx->stats.video_frags_missing +=
				(uint32_t)(slot->frag_total - frag_slot_received_count(slot));
		}

		slot->active = false;
	}
}

static void deliver_video_frame(app_ctx_t *ctx, const uint8_t *data,
				size_t len, uint16_t src_u16, bool keyframe)
{
	if (keyframe) {
		ctx->wait_for_keyframe = false;
		ctx->idr_request_in_flight = false;
	} else if (ctx->wait_for_keyframe) {
		/*
		 * A sequence gap means at least one reference frame is gone. Feeding the
		 * browser dependent P-frames after that produces the visible gray/corrupt
		 * blink until the next IDR. Hold P-frames back and resume only on a fresh
		 * keyframe.
		 */
		ctx->stats.video_wait_keyframe_drops++;
		return;
	}

	cascade_frame_view_t cf = {
		.version      = CASCADE_FRAME_VERSION,
		.src          = src_u16,
		.dst          = 0,
		.traffic_class = CASCADE_CLASS_VIDEO,
		.payload      = data,
		.payload_len  = len,
	};
	if (send_cascade_frame(ctx, &cf)) {
		ctx->stats.video_frames_rx++;
		if (keyframe)
			ctx->stats.video_keyframes_rx++;
		ctx->stats.cascade_out_frames++;
		ctx->stats.video_bytes_out += len;
	} else {
		ctx->stats.video_frames_dropped++;
	}
}

/*
 * True when ANY frame for (src_node, seq) is currently mid-reassembly: at
 * least one of its fragments has arrived but the frame is not complete yet.
 *
 * Originally this only checked keyframes (to give vcpd's delayed keyframe
 * copies time to land). But frag_reassembly_expire() itself doesn't give up
 * on a frame until FRAG_TIMEOUT_MS (800ms) — so an ordinary P-frame that's
 * still legitimately reassembling past GW_VIDEO_REORDER_HOLD_MS (400ms) was
 * being declared a "gap" by flush_video_reorder() 400ms BEFORE reassembly
 * itself would give up. That's not a real loss, just a frame that hasn't
 * finished arriving yet — but the false gap still sets wait_for_keyframe and
 * blanks every subsequent frame until the next keyframe (up to ~1s at
 * gop=25/1000ms IDR), which is what actually produced the choppiness despite
 * frag_loss=0% in the field logs. Checking for ANY pending frame (not just
 * keyframes) and holding it out to the reassembly timeout fixes this: we only
 * declare a gap once the frame is actually gone, never while it's still
 * legitimately in flight.
 */
static bool reassembly_has_pending_frame(const frag_reassembly_ctx_t *rc,
					 uint8_t src_node, uint16_t seq)
{
	for (int i = 0; i < FRAG_MAX_SLOTS; i++) {
		const frag_reassembly_slot_t *slot = &rc->slots[i];
		if (slot->active && slot->src_node == src_node && slot->seq == seq)
			return true;
	}
	return false;
}

static void flush_video_reorder(app_ctx_t *ctx, uint64_t now_ms)
{
	video_reorder_ctx_t *vr = &ctx->video_reorder;

	if (!vr->primed)
		return;

	for (;;) {
		video_reorder_slot_t *slot = video_reorder_find(vr, vr->next_seq);
		if (slot != NULL) {
			deliver_video_frame(ctx, slot->data, slot->len, slot->src_u16,
					  slot->keyframe);
			slot->active = false;
			vr->next_seq++;
			continue;
		}

		slot = video_reorder_find_next_ready(vr);
		if (slot == NULL)
			return;

		/*
		 * Wait longer for the missing frame while it's still actively
		 * reassembling (regardless of keyframe/P-frame), so we never declare a
		 * gap before frag_reassembly_expire() itself would give up
		 * (FRAG_TIMEOUT_MS). Only use the short hold when nothing is in flight
		 * for this seq at all — genuinely nothing to wait for.
		 */
		uint8_t src_node = gw_addr_u16_to_u8(&ctx->gw, vr->src_u16);
		bool pending = reassembly_has_pending_frame(&ctx->reassembly,
							    src_node, vr->next_seq);
		uint32_t hold = pending
				? (FRAG_TIMEOUT_MS + 50U) : GW_VIDEO_REORDER_HOLD_MS;
		if ((now_ms - slot->ready_ms) < hold)
			return;

		if (seq_delta(slot->seq, vr->next_seq) > 0) {
			log_video_reorder_skip(ctx, vr->next_seq, slot->seq,
					       hold, pending,
					       now_ms - slot->ready_ms);
			ctx->stats.video_reorder_skips += (uint16_t)(slot->seq - vr->next_seq);
			ctx->wait_for_keyframe = true;
			if (!ctx->idr_request_in_flight) {
				request_remote_idr(ctx);
				ctx->idr_request_in_flight = true;
			}
		}
		vr->next_seq = slot->seq;
		video_reorder_drop_older_than(vr, vr->next_seq);
	}
}

static void queue_video_frame(app_ctx_t *ctx, const uint8_t *data,
			      size_t len, uint16_t src_u16,
			      uint16_t seq, bool keyframe, uint64_t now_ms)
{
	video_reorder_ctx_t *vr = &ctx->video_reorder;

	if (len > CASCADE_FRAME_MAX_PAYLOAD) {
		ctx->stats.video_frames_dropped++;
		return;
	}
	if (ctx->wait_for_keyframe && !keyframe) {
		ctx->stats.video_wait_keyframe_drops++;
		return;
	}

	if (keyframe) {
		/*
		 * A keyframe is a clean random-access point. Resync the gate FORWARD to
		 * it (cutting through any holes), but never backward: a late keyframe
		 * copy that only finished after we already advanced past its seq must not
		 * rewind delivery — that produced out-of-order frames and a skip burst
		 * right after the keyframe. With the keyframe-aware hold above this branch
		 * normally sees seq == next_seq (delivered in order).
		 */
		if (!vr->primed || vr->src_u16 != src_u16) {
			video_reorder_reset(vr);
			vr->primed = true;
			vr->src_u16 = src_u16;
			vr->next_seq = seq;
		} else if (seq_delta(seq, vr->next_seq) >= 0) {
			vr->next_seq = seq;
		} else {
			return; /* stale late keyframe — drop, wait for the next fresh one */
		}
		video_reorder_drop_older_than(vr, vr->next_seq);
	} else if (!vr->primed || vr->src_u16 != src_u16) {
		/* Not yet primed: prime on the FIRST frame from this source, even if it
		 * is not a keyframe. Hard-gating on keyframes stalls the whole pipeline
		 * when large IDRs rarely reassemble — the browser decoder simply waits
		 * for its own keyframe once the stream starts flowing. */
		video_reorder_reset(vr);
		vr->primed = true;
		vr->src_u16 = src_u16;
		vr->next_seq = seq;
	}

	if (seq_delta(seq, vr->next_seq) < 0)
		return;
	if (video_reorder_find(vr, seq) != NULL)
		return;

	video_reorder_slot_t *slot = video_reorder_alloc(vr);
	if (slot == NULL && keyframe)
		slot = video_reorder_reclaim_for_keyframe(vr);
	if (slot == NULL) {
		ctx->stats.video_frames_dropped++;
		ctx->stats.video_reorder_full_drops++;
		log_video_reorder_full(ctx, seq, keyframe);
		return;
	}

	memset(slot, 0, sizeof(*slot));
	slot->active = true;
	slot->keyframe = keyframe;
	slot->seq = seq;
	slot->src_u16 = src_u16;
	slot->ready_ms = now_ms;
	slot->len = len;
	memcpy(slot->data, data, len);

	flush_video_reorder(ctx, now_ms);
}

static void handle_ulama_rx(app_ctx_t *ctx)
{
  for (int drain = 0; drain < 128; drain++) {
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

	/* Per-node RX stats */
	if (uf.src_node > 0 && uf.src_node < GW_MAX_NODES) {
		ctx->stats.nodes[uf.src_node].rx_pkts++;
		ctx->stats.nodes[uf.src_node].rx_bytes += (uint32_t)uf.payload_len;
		ctx->stats.nodes[uf.src_node].rssi_sum += rssi;
		ctx->stats.nodes[uf.src_node].rssi_count++;
	}
	ctx->stats.rssi_sum += rssi;
	ctx->stats.rssi_count++;

	uint16_t src_u16 = gw_addr_u8_to_u16(&ctx->gw, uf.src_node);
	uint8_t cascade_class = gw_class_ulama_to_cascade(uf.traffic_class);

	/* ANNOUNCE payload over CTRL class → remap to MANAGEMENT for cascade-core */
	if (uf.traffic_class == ULAMA_CLASS_CTRL && uf.payload_len > 9 &&
	    memcmp(uf.payload, "ANNOUNCE:", 9) == 0) {
		cascade_class = CASCADE_CLASS_MANAGEMENT;
	}

	if (uf.flags & ULAMA_FLAG_FRAGMENT) {
		if (uf.traffic_class == ULAMA_CLASS_VIDEO &&
		    video_done_contains(ctx, uf.src_node, uf.seq))
			continue;

		bool complete = frag_reassembly_insert(&ctx->reassembly, &uf, now_ms());
		if (uf.traffic_class == ULAMA_CLASS_VIDEO) {
			ctx->stats.video_frags_rx++;
			/* Log fragment reception with checksum for debugging */
			if (ctx->verbose) {
				frag_reassembly_slot_t *slot = 
					frag_reassembly_find_slot(&ctx->reassembly, uf.src_node, uf.seq);
				if (slot) {
					log_video_frag_received(uf.src_node, uf.seq, uf.frag_idx,
							       uf.frag_total, uf.payload, uf.payload_len,
							       slot->received_mask);
				}
			}
		}

		if (complete) {
			static uint8_t reassembled[FRAG_MAX_REASSEMBLED];
			size_t  reassembled_len = 0;

			if (!frag_reassembly_complete(&ctx->reassembly, uf.src_node, uf.seq,
						      reassembled, sizeof(reassembled),
						      &reassembled_len)) {
				if (uf.traffic_class == ULAMA_CLASS_VIDEO)
					ctx->stats.video_frames_dropped++;
				continue;
			}

			if (uf.traffic_class == ULAMA_CLASS_VIDEO) {
				bool kf = (uf.flags & ULAMA_FLAG_VIDEO_KEYFRAME) ||
					  video_is_keyframe(reassembled, reassembled_len);

				/* All frag_total fragments arrived (that's what "complete"
				 * means), so this frame contributes 0 to video_frags_missing. */
				ctx->stats.video_frags_expected += uf.frag_total;

				video_done_add(ctx, uf.src_node, uf.seq);

				if (kf) {
					/* Discard all stale incomplete frames from this sender.
					 * Equivalent to the old LTS keyframe_flush: prefer fresh IDR. */
					frag_reassembly_flush_stale_video(&ctx->reassembly,
									  uf.src_node, uf.seq);
					ctx->stats.video_keyframe_flushes++;
				}

				queue_video_frame(ctx, reassembled, reassembled_len,
						 src_u16, uf.seq, kf, now_ms());
			} else {
				/* Non-video fragmented frame: forward to cascade as before */
				cascade_frame_view_t cf = {
					.version       = CASCADE_FRAME_VERSION,
					.src           = src_u16,
					.dst           = 0,
					.traffic_class = cascade_class,
					.payload       = reassembled,
					.payload_len   = reassembled_len,
				};
				send_cascade_frame(ctx, &cf);
			}
		}
		continue;
	}

	/* Non-fragmented VIDEO frame (small P-frame < ULAMA_FRAME_MAX_PAYLOAD
	 * bytes sent without FRAGMENT flag). Deliver directly. send_video_frame()
	 * in vcpd always sets FRAGMENT even for single-fragment frames, but this
	 * keeps the gateway correct if that ever changes. */
	if (uf.traffic_class == ULAMA_CLASS_VIDEO) {
		if (payload_is_uvcp_control(uf.payload, uf.payload_len))
			continue;
		queue_video_frame(ctx, uf.payload, uf.payload_len,
				 src_u16, uf.seq,
				 video_is_keyframe(uf.payload, uf.payload_len), now_ms());
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

static app_ctx_t ctx;

int main(int argc, char *argv[])
{
	memset(&ctx, 0, sizeof(ctx));
	strncpy(ctx.gw.cascade_in, "127.0.0.1:5601", sizeof(ctx.gw.cascade_in));
	strncpy(ctx.gw.cascade_out, "127.0.0.1:5600", sizeof(ctx.gw.cascade_out));
	strncpy(ctx.gw.transport_str, "udp", sizeof(ctx.gw.transport_str));
	strncpy(ctx.gw.iface, "mon0", sizeof(ctx.gw.iface));
	strncpy(ctx.listen_addr, "0.0.0.0:5000", sizeof(ctx.listen_addr));
	strncpy(ctx.peer_addr, "127.0.0.1:5001", sizeof(ctx.peer_addr));
	ctx.gw.node_id = 254;
	ctx.verbose = false;
	ctx.channel = 0;
	ctx.tx_rate_500kbps = 12;  /* 6 Mbps = 12 * 500kbps */

	static struct option long_opts[] = {
		{"cascade-in",  required_argument, NULL, 'C'},
		{"cascade-out", required_argument, NULL, 'O'},
		{"transport",   required_argument, NULL, 't'},
		{"listen",      required_argument, NULL, 'l'},
		{"peer",        required_argument, NULL, 'p'},
		{"iface",       required_argument, NULL, 'i'},
		{"channel",     required_argument, NULL, 'c'},
		{"tx-rate-mbps", required_argument, NULL, 1000},
		{"node",        required_argument, NULL, 'n'},
		{"dst-mac",       required_argument, NULL, 'm'},
		{"version",       no_argument,       NULL, 'V'},
		{"verbose",       no_argument,       NULL, 'v'},
		{"help",        no_argument,       NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "C:O:t:l:p:i:c:n:m:Vvh", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'C': strncpy(ctx.gw.cascade_in, optarg, sizeof(ctx.gw.cascade_in) - 1); break;
		case 'O': strncpy(ctx.gw.cascade_out, optarg, sizeof(ctx.gw.cascade_out) - 1); break;
		case 't': strncpy(ctx.gw.transport_str, optarg, sizeof(ctx.gw.transport_str) - 1); break;
		case 'l': strncpy(ctx.listen_addr, optarg, sizeof(ctx.listen_addr) - 1); break;
		case 'p': strncpy(ctx.peer_addr, optarg, sizeof(ctx.peer_addr) - 1); break;
		case 'i': strncpy(ctx.gw.iface, optarg, sizeof(ctx.gw.iface) - 1); break;
		case 'c': ctx.channel = atoi(optarg); break;
		case 1000:
			if (!parse_tx_rate_mbps(optarg, &ctx.tx_rate_500kbps)) {
				fprintf(stderr, "gw: invalid --tx-rate-mbps value: %s\n", optarg);
				return 1;
			}
			break;
		case 'n': ctx.gw.node_id = (uint8_t)atoi(optarg); break;
		case 'm': strncpy(ctx.dst_mac_str, optarg, sizeof(ctx.dst_mac_str) - 1); break;
		case 'V':
			fprintf(stderr, "ulama-gw: build #%d (%s@%s) %s\n",
				ULAMA_BUILD_NUMBER, ULAMA_GIT_BRANCH, ULAMA_GIT_HASH, ULAMA_BUILD_DATE);
			return 0;
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
	if (tk == ULAMA_TRANSPORT_KIND_RADIOD) {
		/* radiod owns the monitor interface and the TDMA schedule; the gateway
		 * just speaks the radiod IPC socket (same as vcpd/ulamad on the drone). */
		rc = ulama_transport_tx_init_radiod(&ctx.ulama_tx, ctx.gw.node_id,
						    NULL, "ulama_gw_tx");
		if (rc < 0) {
			fprintf(stderr, "gw: failed to init radiod TX (is radiod running?): %s\n", strerror(errno));
			goto cleanup;
		}
		rc = ulama_transport_rx_init_radiod(&ctx.ulama_rx, ctx.gw.node_id,
						    NULL, "ulama_gw_rx");
	} else if (tk == ULAMA_TRANSPORT_KIND_UNOW) {
		#if ULAMA_WITH_UNOW
		unow_set_tx_rate_500kbps(ctx.tx_rate_500kbps);
		#endif
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

	fprintf(stderr, "ulama-gw: build #%d (%s@%s) %s started (node=%u, transport=%s)\n",
		ULAMA_BUILD_NUMBER, ULAMA_GIT_BRANCH, ULAMA_GIT_HASH, ULAMA_BUILD_DATE,
		ctx.gw.node_id, ulama_transport_kind_name(tk));
	fprintf(stderr, "  cascade-in:  %s\n", ctx.gw.cascade_in);
	fprintf(stderr, "  cascade-out: %s\n", ctx.gw.cascade_out);
	if (tk == ULAMA_TRANSPORT_KIND_UDP) {
		fprintf(stderr, "  ulama-listen: %s\n", ctx.listen_addr);
		fprintf(stderr, "  ulama-peer:   %s\n", ctx.peer_addr);
	} else if (tk == ULAMA_TRANSPORT_KIND_RADIOD) {
		fprintf(stderr, "  transport: radiod IPC (TDMA scheduler owns the radio)\n");
		fprintf(stderr, "  radio-rx-fd: %d (%s)\n", ctx.ulama_rx.fd,
			ctx.ulama_rx.fd >= 0 ? "poll active" : "poll DISABLED — is radiod running?");
	} else {
		fprintf(stderr, "  iface: %s\n", ctx.gw.iface);
		fprintf(stderr, "  tx-rate: %u.%u Mbps\n",
			ctx.tx_rate_500kbps / 2U,
			(ctx.tx_rate_500kbps & 1U) ? 5U : 0U);
		fprintf(stderr, "  radio-rx-fd: %d (%s)\n", ctx.ulama_rx.fd,
			ctx.ulama_rx.fd >= 0 ? "poll active" : "poll DISABLED — check UNOW init");
	}

	struct pollfd pfds[2];
	nfds_t nfds = 1;
	pfds[0].fd = ctx.cascade_rx_fd;
	pfds[0].events = POLLIN;
	if (ctx.ulama_rx.fd >= 0) {
		pfds[1].fd = ctx.ulama_rx.fd;
		pfds[1].events = POLLIN;
		nfds = 2;
	}

	while (g_running) {
		int ret = poll(pfds, nfds, 2);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		uint64_t ts = now_ms();

		/* TDMA schedule: CTRL first → cascade TX → ulama RX → cascade again.
		 * CTRL frames from cascade are sent with reliable delivery,
		 * other classes are sent unreliably (rate-limited by drain count). */
		handle_cascade_rx(&ctx);

		/* RX slot: receive ULAMA frames from device */
		handle_ulama_rx(&ctx);

		/* Second pass: catch any cascade frames that arrived during RX */
		handle_cascade_rx(&ctx);
		maybe_send_ctrl_keepalive(&ctx, ts);

		/* Refresh: handle_ulama_rx() just stamped slot->ready_ms/first_ts_ms
		 * via its own now_ms() calls while draining up to 128 packets, which
		 * can already read later than the loop-top ts (millisecond clock
		 * granularity makes even a single call land 1ms ahead). Comparing
		 * those against the stale ts underflows the unsigned "elapsed" math
		 * below to ~2^64, which showed up as insane age/waited values in the
		 * reorder-skip and reassembly-expire logs. */
		ts = now_ms();
		flush_video_reorder(&ctx, ts);

		/* Print stats every 5 seconds */
		if (ts - ctx.stats.last_print_ms >= 5000) {
			gw_stats_t *s = &ctx.stats;
			uint64_t dt = ts - s->last_print_ms;
			uint32_t vbps = (uint32_t)(s->video_bytes_out * 8000 / (dt > 0 ? dt : 1));
			int avg_rssi = s->rssi_count > 0 ? (int)(s->rssi_sum / (int32_t)s->rssi_count) : 0;
			/* Real end-to-end per-fragment loss: fragments never received via
			 * ANY of the ~3 redundant transmission attempts, over all fragments
			 * the sender declared (frag_total) across completed+expired frames.
			 * One decimal place of percent (permille/10) — enough to tell 0.1%
			 * from 1% from 20% apart without floating point. */
			uint32_t frag_loss_x10 = s->video_frags_expected > 0
				? (uint32_t)(((uint64_t)s->video_frags_missing * 1000)
					     / s->video_frags_expected)
				: 0;
			char _ts[9]; { time_t _t = time(NULL); strftime(_ts, sizeof(_ts), "%H:%M:%S", localtime(&_t)); }
			fprintf(stderr, "%s [stats] video_rx=%u telem_rx=%u ctrl_rx=%u ctrl_tx=%u | "
				"video frames_rx=%u kf_rx=%u frags_rx=%u dropped=%u incomplete=%u kf_lost=%u kf_flushes=%u reorder_skip=%u wait_kf_drop=%u rfull=%u "
				"frag_loss=%u.%u%%(%u/%u) | "
				"video_out=%u Kbit/s | rssi=%d\n",
				_ts, s->ulama_rx_video, s->ulama_rx_telem, s->ulama_rx_ctrl, s->ctrl_tx,
				s->video_frames_rx, s->video_keyframes_rx, s->video_frags_rx, s->video_frames_dropped,
				s->video_frames_incomplete, s->video_keyframes_lost,
				s->video_keyframe_flushes, s->video_reorder_skips,
				s->video_wait_keyframe_drops, s->video_reorder_full_drops,
				frag_loss_x10 / 10, frag_loss_x10 % 10,
				s->video_frags_missing, s->video_frags_expected,
				vbps / 1000, avg_rssi);
			/* Per-node summary */
			bool any_node = false;
			for (int ni = 0; ni < GW_MAX_NODES; ni++) {
				gw_node_stats_t *ns = &s->nodes[ni];
				if (ns->rx_pkts == 0 && ns->tx_pkts == 0)
					continue;
				if (!any_node) {
					fprintf(stderr, "%s [nodes]", _ts);
					any_node = true;
				}
				int node_rssi = ns->rssi_count > 0
					? (int)(ns->rssi_sum / (int32_t)ns->rssi_count) : 0;
				fprintf(stderr, " n%d[rx=%u/%uB tx=%u/%uB rssi=%d]",
					ni, ns->rx_pkts, ns->rx_bytes,
					ns->tx_pkts, ns->tx_bytes, node_rssi);
			}
			if (any_node)
				fprintf(stderr, "\n");

			uint32_t fps_x10 = (uint32_t)(s->cascade_out_frames * 10000 / (dt > 0 ? dt : 1));
			fprintf(stderr, "%s [pipeline] fps_out=%u.%u\n",
				_ts, fps_x10 / 10, fps_x10 % 10);

			memset(s, 0, sizeof(*s));
			s->last_print_ms = ts;
		}

		expire_reassembly_slots(&ctx, ts);
	}

	fprintf(stderr, "ulama-gw: shutting down\n");

cleanup:
	if (ctx.cascade_rx_fd >= 0) close(ctx.cascade_rx_fd);
	if (ctx.cascade_tx_fd >= 0) close(ctx.cascade_tx_fd);
	ulama_transport_tx_close(&ctx.ulama_tx);
	ulama_transport_rx_close(&ctx.ulama_rx);

	return 0;
}
