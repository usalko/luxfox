/*
 * MPP hardware video backend for vcpd.
 *
 * Go-like I/O model in C:
 *   ring + pipe  = channel   (encoder→sender)
 *   poll()       = select    (main loop, encode loop)
 *   pthread      = goroutine
 *
 * Camera pipeline (one encode thread):
 *   poll(V4L2, 5ms)  ← "select case <-v4l2_ch:"
 *     DQBUF → VDEC_SendStream(0)  ← non-blocking submit
 *   drain VDEC_GetFrame(0)        ← "for { select case <-vdec_ch: ... default: break }"
 *     VENC_SendFrame(0)           ← non-blocking submit
 *   drain VENC_GetStream(0)       ← "for { select case <-venc_ch: ... default: break }"
 *     ring_push + pipe-signal     ← "ch <- frame"
 *
 * VDEC and VENC run concurrently: while VENC encodes frame N, VDEC
 * decodes frame N+1 — each drain loop collects whichever finished first.
 *
 * Test-pattern pipeline (same structure, no V4L2):
 *   VENC_SendFrame(0) → drain VENC_GetStream(0) → pipe
 *   usleep(interval) provides FPS pacing.
 */

#ifdef VCPD_WITH_MPP

#include "vcpd/video_source.h"
#include "vcpd/video_ring.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "rk_debug.h"
#include "rk_comm_vdec.h"
#include "rockchip/mpp_err.h"
#include "rk_mpi_cal.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vdec.h"
#include "rk_mpi_venc.h"

#define V4L2_BUFFER_COUNT  4
#define VDEC_CHN_ID        0
#define VENC_CHN_ID        0
#define MJPEG_INFLIGHT_SLOTS 8
#define MJPEG_SUBMIT_BURST  6
#define VDEC_SUBMIT_TIMEOUT_MS   5
#define VDEC_GETFRAME_TIMEOUT_MS 2
#define VENC_SEND_TIMEOUT_MS     5
#define VENC_GETSTREAM_TIMEOUT_MS 2

/* Event-loop poll interval (ms) — the "yield" between non-blocking drain passes.
 * At 15fps V4L2 delivers a frame every ~66ms; 5ms gives ~13 drain attempts per frame. */
#define ENCODE_POLL_MS    5

/* Backing file for the capture→decode MJPEG ring (see mpp_capture_thread). */
#define MJPEG_RING_FILE   "/dev/shm/vcpd_mjpeg_ring"

typedef struct {
	void  *start;
	size_t length;
} mpp_v4l2_buffer_t;

typedef struct {
	_Atomic bool in_use;
	uint32_t len;
	uint8_t data[VIDEO_RING_SLOT_MAX];
} mjpeg_submit_slot_t;

#define MAX_VENC_PACKS 8

typedef struct {
	uint64_t window_start_ms;
	uint32_t v4l2_dq_ok;
	uint32_t vdec_submit_ok;
	uint32_t vdec_submit_busy;
	uint32_t vdec_submit_full;
	uint32_t vdec_submit_drop;
	uint32_t vdec_submit_calls;
	uint32_t vdec_submit_us_max;
	uint64_t vdec_submit_us_total;
	uint32_t vdec_frame_ok;
	uint32_t vdec_getframe_calls;
	uint32_t vdec_getframe_us_max;
	uint64_t vdec_getframe_us_total;
	uint32_t venc_send_ok;
	uint32_t venc_send_calls;
	uint32_t venc_send_us_max;
	uint64_t venc_send_us_total;
	uint32_t venc_stream_ok;
	uint32_t venc_getstream_calls;
	uint32_t venc_getstream_us_max;
	uint64_t venc_getstream_us_total;
	uint32_t nal_writes;
	uint64_t nal_bytes;
	uint32_t ring_drop;      /* encoded-output ring overrun (encode→sender)  */
	uint32_t cap_ring_drop;  /* MJPEG capture ring overrun (encode < capture) */
} mpp_diag_t;

typedef struct {
	video_source_mpp_t *src;

	int                v4l2_fd;
	mpp_v4l2_buffer_t  v4l2_bufs[V4L2_BUFFER_COUNT];
	uint32_t           actual_width;
	uint32_t           actual_height;
	uint32_t           actual_fps;
	uint32_t           actual_fps_num;
	uint32_t           actual_fps_den;

	VENC_STREAM_S      venc_stream;
	VENC_PACK_S        venc_packs[MAX_VENC_PACKS];

	int                pipe_wr;
	pthread_t          encode_thread;
	bool               encode_thread_started;
	volatile bool      stop;

	/* Capture→decode decoupling: a dedicated capture thread services V4L2 at the
	 * full camera rate and publishes raw MJPEG frames into mjpeg_ring; the encode
	 * thread consumes them (VDEC→VENC). mjpeg_pipe is the wakeup channel, mirroring
	 * the ring+pipe "channel" used between the encoder and the sender. */
	video_ring_t      *mjpeg_ring;
	int                mjpeg_pipe_rd;
	int                mjpeg_pipe_wr;
	pthread_t          capture_thread;
	bool               capture_thread_started;
	mjpeg_submit_slot_t mjpeg_submit[MJPEG_INFLIGHT_SLOTS];

	bool               diag_lock_init;
	pthread_mutex_t    diag_lock;
	mpp_diag_t         diag;

	/* Ring buffer: holds one whole encoded H.265 frame per slot, used in
	 * both camera and test-pattern mode so vsrc_read() always returns a
	 * complete frame. */
	video_ring_t      *ring;

	/* true if this session decodes a real camera feed (V4L2 + VDEC);
	 * false in test-pattern mode (VENC only, no VDEC). */
	bool               is_camera;

} mpp_ctx_t;

/* ------------------------------------------------------------------ timing */

static uint64_t mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint64_t mono_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

/* ------------------------------------------------------------------ diag */

static void mpp_diag_reset_locked(mpp_ctx_t *ctx, uint64_t now_ms)
{
	memset(&ctx->diag, 0, sizeof(ctx->diag));
	ctx->diag.window_start_ms = now_ms;
}

static void mpp_diag_reset(mpp_ctx_t *ctx, uint64_t now_ms)
{
	if (!ctx->diag_lock_init) {
		mpp_diag_reset_locked(ctx, now_ms);
		return;
	}
	pthread_mutex_lock(&ctx->diag_lock);
	mpp_diag_reset_locked(ctx, now_ms);
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_inc_capture(mpp_ctx_t *ctx)
{
	pthread_mutex_lock(&ctx->diag_lock);
	ctx->diag.v4l2_dq_ok++;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_inc_vdec_frame(mpp_ctx_t *ctx)
{
	pthread_mutex_lock(&ctx->diag_lock);
	ctx->diag.vdec_frame_ok++;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_note_vdec_getframe_latency(mpp_ctx_t *ctx, uint64_t us)
{
	pthread_mutex_lock(&ctx->diag_lock);
	ctx->diag.vdec_getframe_calls++;
	ctx->diag.vdec_getframe_us_total += us;
	if (us > ctx->diag.vdec_getframe_us_max)
		ctx->diag.vdec_getframe_us_max = (uint32_t)us;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_note_vdec_submit(mpp_ctx_t *ctx, RK_S32 ret)
{
	pthread_mutex_lock(&ctx->diag_lock);
	if (ret == RK_SUCCESS)
		ctx->diag.vdec_submit_ok++;
	else if (ret == MPP_ERR_TIMEOUT || ret == RK_ERR_VDEC_BUSY)
		ctx->diag.vdec_submit_busy++;
	else if (ret == RK_ERR_VDEC_BUF_FULL || ret == RK_ERR_VDEC_NOBUF ||
		 ret == MPP_ERR_BUFFER_FULL)
		ctx->diag.vdec_submit_full++;
	else
		ctx->diag.vdec_submit_drop++;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_note_vdec_submit_latency(mpp_ctx_t *ctx, uint64_t us)
{
	pthread_mutex_lock(&ctx->diag_lock);
	ctx->diag.vdec_submit_calls++;
	ctx->diag.vdec_submit_us_total += us;
	if (us > ctx->diag.vdec_submit_us_max)
		ctx->diag.vdec_submit_us_max = (uint32_t)us;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_inc_venc_submit(mpp_ctx_t *ctx)
{
	pthread_mutex_lock(&ctx->diag_lock);
	ctx->diag.venc_send_ok++;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_note_venc_submit_latency(mpp_ctx_t *ctx, uint64_t us)
{
	pthread_mutex_lock(&ctx->diag_lock);
	ctx->diag.venc_send_calls++;
	ctx->diag.venc_send_us_total += us;
	if (us > ctx->diag.venc_send_us_max)
		ctx->diag.venc_send_us_max = (uint32_t)us;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_note_venc_stream(mpp_ctx_t *ctx, uint32_t nals, uint64_t bytes)
{
	pthread_mutex_lock(&ctx->diag_lock);
	ctx->diag.venc_stream_ok++;
	ctx->diag.nal_writes += nals;
	ctx->diag.nal_bytes  += bytes;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_note_venc_getstream_latency(mpp_ctx_t *ctx, uint64_t us)
{
	pthread_mutex_lock(&ctx->diag_lock);
	ctx->diag.venc_getstream_calls++;
	ctx->diag.venc_getstream_us_total += us;
	if (us > ctx->diag.venc_getstream_us_max)
		ctx->diag.venc_getstream_us_max = (uint32_t)us;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_note_ring_drop(mpp_ctx_t *ctx)
{
	pthread_mutex_lock(&ctx->diag_lock);
	ctx->diag.ring_drop++;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_note_cap_ring_drop(mpp_ctx_t *ctx)
{
	pthread_mutex_lock(&ctx->diag_lock);
	ctx->diag.cap_ring_drop++;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_maybe_report(mpp_ctx_t *ctx)
{
	mpp_diag_t s;
	uint64_t now_ms = mono_ms();
	pthread_mutex_lock(&ctx->diag_lock);
	if (ctx->diag.window_start_ms == 0) {
		mpp_diag_reset_locked(ctx, now_ms);
		pthread_mutex_unlock(&ctx->diag_lock);
		return;
	}
	uint64_t dt = now_ms - ctx->diag.window_start_ms;
	if (dt < 5000) {
		pthread_mutex_unlock(&ctx->diag_lock);
		return;
	}
	s = ctx->diag;
	mpp_diag_reset_locked(ctx, now_ms);
	pthread_mutex_unlock(&ctx->diag_lock);

	uint64_t d = dt > 0 ? dt : 1;
	uint32_t fps_cap_x10 = (uint32_t)(s.v4l2_dq_ok    * 10000 / d);
	uint32_t fps_dec_x10 = (uint32_t)(s.vdec_frame_ok  * 10000 / d);
	uint32_t fps_enc_x10 = (uint32_t)(s.venc_stream_ok * 10000 / d);
	uint32_t avg_vdec_in  = s.vdec_submit_calls   > 0 ? (uint32_t)(s.vdec_submit_us_total   / s.vdec_submit_calls)   : 0;
	uint32_t avg_vdec_out = s.vdec_getframe_calls > 0 ? (uint32_t)(s.vdec_getframe_us_total / s.vdec_getframe_calls) : 0;
	uint32_t avg_venc_in  = s.venc_send_calls     > 0 ? (uint32_t)(s.venc_send_us_total     / s.venc_send_calls)     : 0;
	uint32_t avg_venc_out = s.venc_getstream_calls> 0 ? (uint32_t)(s.venc_getstream_us_total/ s.venc_getstream_calls): 0;
	uint32_t neg_num = ctx->actual_fps_num > 0 ? ctx->actual_fps_num : (uint32_t)(ctx->src->fps > 0 ? ctx->src->fps : 15);
	uint32_t neg_den = ctx->actual_fps_den > 0 ? ctx->actual_fps_den : 1;
	char ts[9]; { time_t t = time(NULL); strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t)); }
	fprintf(stderr,
		"%s vcpd: [mpp] cap=%u dec=%u enc=%u sub=%u/%u/%u/%u nals=%u bytes=%llu rdrop=%u cdrop=%u | "
		"fps cap=%u.%u dec=%u.%u enc=%u.%u | "
		"lat_us vdec_in=%u/%u vdec_out=%u/%u venc_in=%u/%u venc_out=%u/%u | "
		"negotiated=%u/%u fps\n",
		ts,
		s.v4l2_dq_ok, s.vdec_frame_ok, s.venc_stream_ok,
		s.vdec_submit_ok, s.vdec_submit_busy, s.vdec_submit_full, s.vdec_submit_drop,
		s.nal_writes, (unsigned long long)s.nal_bytes, s.ring_drop, s.cap_ring_drop,
		fps_cap_x10 / 10, fps_cap_x10 % 10,
		fps_dec_x10 / 10,  fps_dec_x10 % 10,
		fps_enc_x10 / 10,  fps_enc_x10 % 10,
		avg_vdec_in,  s.vdec_submit_us_max,
		avg_vdec_out, s.vdec_getframe_us_max,
		avg_venc_in,  s.venc_send_us_max,
		avg_venc_out, s.venc_getstream_us_max,
		neg_num, neg_den);
}

/* ------------------------------------------------------------------ V4L2 */

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;
	do { ret = ioctl(fd, request, arg); } while (ret == -1 && errno == EINTR);
	return ret;
}

static void mpp_update_actual_fps(mpp_ctx_t *ctx, const struct v4l2_streamparm *parm)
{
	uint32_t tpf_num = parm->parm.capture.timeperframe.numerator;
	uint32_t tpf_den = parm->parm.capture.timeperframe.denominator;
	if (tpf_num == 0 || tpf_den == 0) {
		ctx->actual_fps_num = (uint32_t)(ctx->src->fps > 0 ? ctx->src->fps : 15);
		ctx->actual_fps_den = 1;
		ctx->actual_fps     = ctx->actual_fps_num;
		return;
	}
	ctx->actual_fps_num = tpf_den;
	ctx->actual_fps_den = tpf_num;
	ctx->actual_fps     = (tpf_den + tpf_num / 2) / tpf_num;
}

/* Query VIDIOC_ENUM_FRAMEINTERVALS and return the supported fps value
 * closest to req_fps.  Falls back to req_fps if enumeration is unavailable. */
static int v4l2_snap_fps(int fd, uint32_t pixfmt, uint32_t w, uint32_t h, int req_fps)
{
	int best_fps  = 0;
	int best_diff = INT_MAX;

	struct v4l2_frmivalenum fi;
	memset(&fi, 0, sizeof(fi));
	fi.pixel_format = pixfmt;
	fi.width        = w;
	fi.height       = h;

	for (fi.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fi) == 0; fi.index++) {
		int fps = 0;
		if (fi.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
			if (fi.discrete.numerator > 0)
				fps = (int)(fi.discrete.denominator / fi.discrete.numerator);
			int diff = abs(fps - req_fps);
			if (fps > 0 && diff < best_diff) { best_diff = diff; best_fps = fps; }
		} else {
			/* stepwise / continuous: check min and max endpoints */
			int fps_min = 0, fps_max = 0;
			if (fi.stepwise.max.numerator > 0)
				fps_min = (int)(fi.stepwise.max.denominator / fi.stepwise.max.numerator);
			if (fi.stepwise.min.numerator > 0)
				fps_max = (int)(fi.stepwise.min.denominator / fi.stepwise.min.numerator);
			int d1 = fps_min > 0 ? abs(fps_min - req_fps) : INT_MAX;
			int d2 = fps_max > 0 ? abs(fps_max - req_fps) : INT_MAX;
			if (fps_min > 0 && d1 < best_diff) { best_diff = d1; best_fps = fps_min; }
			if (fps_max > 0 && d2 < best_diff) { best_diff = d2; best_fps = fps_max; }
		}
	}

	if (best_fps > 0 && best_fps != req_fps)
		fprintf(stderr, "vcpd: [mpp] fps snap: requested=%d → %d (nearest supported)\n",
			req_fps, best_fps);
	return best_fps > 0 ? best_fps : req_fps;
}

static int v4l2_open(mpp_ctx_t *ctx)
{
	struct v4l2_capability      cap;
	struct v4l2_format          fmt;
	struct v4l2_streamparm      parm;
	struct v4l2_requestbuffers  req;

	ctx->v4l2_fd = open(ctx->src->device, O_RDWR, 0);
	if (ctx->v4l2_fd < 0) return -1;

	if (xioctl(ctx->v4l2_fd, VIDIOC_QUERYCAP, &cap) < 0) return -1;
	if (!(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) ||
	    !(cap.device_caps & V4L2_CAP_STREAMING))
		return -1;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width       = (unsigned)ctx->src->width;
	fmt.fmt.pix.height      = (unsigned)ctx->src->height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
	fmt.fmt.pix.field       = V4L2_FIELD_NONE;
	if (xioctl(ctx->v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) return -1;
	ctx->actual_width  = fmt.fmt.pix.width;
	ctx->actual_height = fmt.fmt.pix.height;

	int snapped_fps = v4l2_snap_fps(ctx->v4l2_fd, V4L2_PIX_FMT_MJPEG,
					ctx->actual_width, ctx->actual_height,
					ctx->src->fps > 0 ? ctx->src->fps : 25);
	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.timeperframe.numerator   = 1;
	parm.parm.capture.timeperframe.denominator = (unsigned)snapped_fps;
	if (xioctl(ctx->v4l2_fd, VIDIOC_S_PARM, &parm) == 0) {
		struct v4l2_streamparm actual = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
		if (xioctl(ctx->v4l2_fd, VIDIOC_G_PARM, &actual) == 0)
			parm = actual;
	}
	mpp_update_actual_fps(ctx, &parm);

	memset(&req, 0, sizeof(req));
	req.count  = V4L2_BUFFER_COUNT;
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req) < 0) return -1;

	for (int i = 0; i < V4L2_BUFFER_COUNT; i++) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = (unsigned)i;
		if (xioctl(ctx->v4l2_fd, VIDIOC_QUERYBUF, &buf) < 0) return -1;
		ctx->v4l2_bufs[i].length = buf.length;
		ctx->v4l2_bufs[i].start  = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
						 MAP_SHARED, ctx->v4l2_fd, buf.m.offset);
		if (ctx->v4l2_bufs[i].start == MAP_FAILED) {
			ctx->v4l2_bufs[i].start = NULL;
			return -1;
		}
		if (xioctl(ctx->v4l2_fd, VIDIOC_QBUF, &buf) < 0) return -1;
	}
	return 0;
}

static int v4l2_stream_on(mpp_ctx_t *ctx)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	return xioctl(ctx->v4l2_fd, VIDIOC_STREAMON, &type);
}

static void v4l2_cleanup(mpp_ctx_t *ctx)
{
	if (ctx->v4l2_fd >= 0) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		xioctl(ctx->v4l2_fd, VIDIOC_STREAMOFF, &type);
	}
	for (int i = 0; i < V4L2_BUFFER_COUNT; i++) {
		if (ctx->v4l2_bufs[i].start) {
			munmap(ctx->v4l2_bufs[i].start, ctx->v4l2_bufs[i].length);
			ctx->v4l2_bufs[i].start = NULL;
		}
	}
	if (ctx->v4l2_fd >= 0) {
		close(ctx->v4l2_fd);
		ctx->v4l2_fd = -1;
	}
}

int video_source_mpp_capture_mjpeg(video_source_mpp_t *src, const char *outpath, int max_frames)
{
	if (!src || !outpath || max_frames <= 0)
		return -1;

	mpp_ctx_t *ctx = calloc(1, sizeof(mpp_ctx_t));
	if (!ctx)
		return -1;
	ctx->src = src;
	ctx->v4l2_fd = -1;

	if (v4l2_open(ctx) < 0) {
		fprintf(stderr, "vcpd: [mjpeg-test] failed to open %s: %s\n",
			src->device, strerror(errno));
		free(ctx);
		return -1;
	}

	if (v4l2_stream_on(ctx) < 0) {
		fprintf(stderr, "vcpd: [mjpeg-test] stream on failed: %s\n",
			strerror(errno));
		v4l2_cleanup(ctx);
		free(ctx);
		return -1;
	}

	FILE *fp = fopen(outpath, "wb");
	if (!fp) {
		fprintf(stderr, "vcpd: [mjpeg-test] cannot open %s: %s\n",
			outpath, strerror(errno));
		v4l2_cleanup(ctx);
		free(ctx);
		return -1;
	}

	fprintf(stderr,
		"vcpd: MJPEG TEST — capturing %d frames from %s to %s\n",
		max_frames, src->device, outpath);
	fprintf(stderr,
		"vcpd: [mjpeg-test] capture req=%dx%d@%d fps, got=%ux%u @ %u/%u fps (~%u)\n",
		src->width, src->height, src->fps,
		ctx->actual_width, ctx->actual_height,
		ctx->actual_fps_num > 0 ? ctx->actual_fps_num : (uint32_t)src->fps,
		ctx->actual_fps_den > 0 ? ctx->actual_fps_den : 1,
		ctx->actual_fps);

	struct pollfd pfd = { .fd = ctx->v4l2_fd, .events = POLLIN };
	uint64_t t0 = mono_ms();
	uint64_t total_bytes = 0;
	int frames = 0;

	while (frames < max_frames) {
		int pret = poll(&pfd, 1, 1000);
		if (pret < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "vcpd: [mjpeg-test] poll failed: %s\n", strerror(errno));
			break;
		}
		if (pret == 0 || !(pfd.revents & POLLIN))
			continue;

		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		if (xioctl(ctx->v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			fprintf(stderr, "vcpd: [mjpeg-test] DQBUF failed: %s\n", strerror(errno));
			break;
		}

		if (buf.bytesused > 0) {
			size_t wrote = fwrite(ctx->v4l2_bufs[buf.index].start, 1,
				(size_t)buf.bytesused, fp);
			if (wrote != (size_t)buf.bytesused) {
				fprintf(stderr, "vcpd: [mjpeg-test] fwrite failed for %s\n", outpath);
				(void)xioctl(ctx->v4l2_fd, VIDIOC_QBUF, &buf);
				break;
			}
			total_bytes += buf.bytesused;
			frames++;
			if ((frames % 25) == 0) {
				uint64_t elapsed = mono_ms() - t0;
				fprintf(stderr,
					"vcpd: [mjpeg-test] captured %d/%d frames (%llu bytes, %.1f fps)\n",
					frames, max_frames,
					(unsigned long long)total_bytes,
					elapsed > 0 ? frames * 1000.0 / (double)elapsed : 0.0);
			}
		}

		if (xioctl(ctx->v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
			fprintf(stderr, "vcpd: [mjpeg-test] QBUF failed: %s\n", strerror(errno));
			break;
		}
	}

	fflush(fp);
	fclose(fp);
	v4l2_cleanup(ctx);
	free(ctx);

	uint64_t elapsed = mono_ms() - t0;
	fprintf(stderr,
		"vcpd: MJPEG TEST DONE — %d frames, %llu bytes, %llu ms (%.1f fps)\n",
		frames, (unsigned long long)total_bytes, (unsigned long long)elapsed,
		elapsed > 0 ? frames * 1000.0 / (double)elapsed : 0.0);
	fprintf(stderr, "vcpd: output: %s\n", outpath);
	fprintf(stderr, "vcpd: to verify, run:\n");
	fprintf(stderr, "  ffplay -f mjpeg %s\n", outpath);
	fprintf(stderr, "  ffmpeg -f mjpeg -i %s -frames:v 5 frame_%%02d.jpg\n", outpath);
	return frames > 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ MPP helpers */

static RK_S32 mjpeg_submit_slot_release(void *opaque)
{
	mjpeg_submit_slot_t *slot = (mjpeg_submit_slot_t *)opaque;
	if (slot != NULL)
		atomic_store_explicit(&slot->in_use, false, memory_order_release);
	return RK_SUCCESS;
}

static mjpeg_submit_slot_t *mjpeg_submit_slot_acquire(mpp_ctx_t *ctx)
{
	for (int i = 0; i < MJPEG_INFLIGHT_SLOTS; i++) {
		bool expected = false;
		if (atomic_compare_exchange_strong_explicit(&ctx->mjpeg_submit[i].in_use,
				&expected, true,
				memory_order_acq_rel, memory_order_acquire))
			return &ctx->mjpeg_submit[i];
	}
	return NULL;
}

static RK_S32 init_vdec(uint32_t width, uint32_t height)
{
	VDEC_PIC_BUF_ATTR_S pic_buf_attr;
	MB_PIC_CAL_S        pic_buf_result;
	VDEC_CHN_ATTR_S     chn_attr;
	VDEC_CHN_PARAM_S    chn_param;

	memset(&pic_buf_attr, 0, sizeof(pic_buf_attr));
	pic_buf_attr.enCodecType                   = RK_VIDEO_ID_MJPEG;
	pic_buf_attr.stPicBufAttr.u32Width         = width;
	pic_buf_attr.stPicBufAttr.u32Height        = height;
	pic_buf_attr.stPicBufAttr.enPixelFormat    = RK_FMT_YUV420SP;
	pic_buf_attr.stPicBufAttr.enCompMode       = COMPRESS_MODE_NONE;

	RK_S32 ret = RK_MPI_CAL_VDEC_GetPicBufferSize(&pic_buf_attr, &pic_buf_result);
	if (ret != RK_SUCCESS) return ret;

	memset(&chn_attr, 0, sizeof(chn_attr));
	chn_attr.enMode          = VIDEO_MODE_FRAME;
	chn_attr.enType          = RK_VIDEO_ID_MJPEG;
	chn_attr.u32PicWidth     = width;
	chn_attr.u32PicHeight    = height;
	chn_attr.u32FrameBufCnt  = 6;
	chn_attr.u32StreamBufCnt = 4;
	chn_attr.u32FrameBufSize = pic_buf_result.u32MBSize;

	ret = RK_MPI_VDEC_CreateChn(VDEC_CHN_ID, &chn_attr);
	if (ret != RK_SUCCESS) return ret;

	memset(&chn_param, 0, sizeof(chn_param));
	chn_param.enType                               = RK_VIDEO_ID_MJPEG;
	chn_param.stVdecPictureParam.enPixelFormat     = RK_FMT_YUV420SP;
	ret = RK_MPI_VDEC_SetChnParam(VDEC_CHN_ID, &chn_param);
	if (ret != RK_SUCCESS) { RK_MPI_VDEC_DestroyChn(VDEC_CHN_ID); return ret; }

	ret = RK_MPI_VDEC_StartRecvStream(VDEC_CHN_ID);
	if (ret != RK_SUCCESS) { RK_MPI_VDEC_DestroyChn(VDEC_CHN_ID); return ret; }

	return RK_SUCCESS;
}

static RK_S32 init_venc(mpp_ctx_t *ctx)
{
	VENC_CHN_ATTR_S chn_attr;
	memset(&chn_attr, 0, sizeof(chn_attr));

	chn_attr.stVencAttr.enType          = RK_VIDEO_ID_HEVC;
	chn_attr.stVencAttr.enPixelFormat   = RK_FMT_YUV420SP;
	chn_attr.stVencAttr.u32Profile      = 0;
	chn_attr.stVencAttr.u32PicWidth     = ctx->actual_width;
	chn_attr.stVencAttr.u32PicHeight    = ctx->actual_height;
	chn_attr.stVencAttr.u32VirWidth     = ctx->actual_width;
	chn_attr.stVencAttr.u32VirHeight    = ctx->actual_height;
	chn_attr.stVencAttr.u32StreamBufCnt = 4;
	chn_attr.stVencAttr.u32BufSize      = ctx->actual_width * ctx->actual_height / 2;

	chn_attr.stRcAttr.enRcMode                            = VENC_RC_MODE_H265CBR;
	chn_attr.stRcAttr.stH265Cbr.u32Gop                   = (RK_U32)(ctx->src->gop > 0 ? ctx->src->gop : 5);
	chn_attr.stRcAttr.stH265Cbr.u32BitRate                = (RK_U32)ctx->src->bitrate_kbps;
	/* src and dst fps must match — no HW frame dropping.
	 * We already snapped to a supported discrete fps in v4l2_snap_fps(). */
	RK_U32 enc_fps_num = ctx->actual_fps_num > 0 ? ctx->actual_fps_num : (RK_U32)(ctx->src->fps > 0 ? ctx->src->fps : 25);
	RK_U32 enc_fps_den = ctx->actual_fps_den > 0 ? ctx->actual_fps_den : 1;
	chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen       = enc_fps_den;
	chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum       = enc_fps_num;
	chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen        = enc_fps_den;
	chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum        = enc_fps_num;
	chn_attr.stGopAttr.enGopMode                          = ctx->src->smartp
		? VENC_GOPMODE_SMARTP : VENC_GOPMODE_NORMALP;
	if (ctx->src->smartp) {
		chn_attr.stGopAttr.s32VirIdrLen = (RK_S32)(chn_attr.stRcAttr.stH265Cbr.u32Gop > 1
			? chn_attr.stRcAttr.stH265Cbr.u32Gop / 2 : 1);
		chn_attr.stGopAttr.u32MaxLtrCount = 1;
	}

	RK_S32 ret = RK_MPI_VENC_CreateChn(VENC_CHN_ID, &chn_attr);
	if (ret != RK_SUCCESS) { RK_LOGE("RK_MPI_VENC_CreateChn failed %#X", ret); return ret; }

	/* Slice split disabled: RV1106 vepu540c triggers kernel panic
	 * (NULL deref in vepu540c_h265_set_hw_address) when slice split
	 * is enabled. */

	VENC_RECV_PIC_PARAM_S recv_param;
	memset(&recv_param, 0, sizeof(recv_param));
	recv_param.s32RecvPicNum = -1;
	ret = RK_MPI_VENC_StartRecvFrame(VENC_CHN_ID, &recv_param);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VENC_StartRecvFrame failed %#X", ret);
		RK_MPI_VENC_DestroyChn(VENC_CHN_ID);
		return ret;
	}

	if (ctx->src->intra_refresh_rows > 0) {
		VENC_INTRA_REFRESH_S intra_refresh;
		memset(&intra_refresh, 0, sizeof(intra_refresh));
		intra_refresh.bRefreshEnable = RK_TRUE;
		intra_refresh.enIntraRefreshMode = INTRA_REFRESH_ROW;
		intra_refresh.u32RefreshNum = (RK_U32)ctx->src->intra_refresh_rows;
		ret = RK_MPI_VENC_SetIntraRefresh(VENC_CHN_ID, &intra_refresh);
		if (ret != RK_SUCCESS) {
			fprintf(stderr,
				"vcpd: [mpp] warning: intra refresh setup failed %#X, continuing without it\n",
				ret);
		} else {
			fprintf(stderr,
				"vcpd: [mpp] recovery smartp=%s intra_refresh_rows=%d viridr=%d\n",
				ctx->src->smartp ? "on" : "off",
				ctx->src->intra_refresh_rows,
				ctx->src->smartp ? (int)chn_attr.stGopAttr.s32VirIdrLen : 0);
		}
	} else {
		fprintf(stderr,
			"vcpd: [mpp] recovery smartp=%s intra_refresh=off\n",
			ctx->src->smartp ? "on" : "off");
	}

	memset(ctx->venc_packs,  0, sizeof(ctx->venc_packs));
	memset(&ctx->venc_stream, 0, sizeof(ctx->venc_stream));
	ctx->venc_stream.pstPack = ctx->venc_packs;
	return RK_SUCCESS;
}

static void deinit_venc(void)
{
	RK_MPI_VENC_StopRecvFrame(VENC_CHN_ID);
	RK_MPI_VENC_DestroyChn(VENC_CHN_ID);
}

static void deinit_vdec(void)
{
	RK_MPI_VDEC_StopRecvStream(VDEC_CHN_ID);
	RK_MPI_VDEC_DestroyChn(VDEC_CHN_ID);
}

/* Submit one MJPEG packet to VDEC.
 * The caller supplies both the packet buffer and its lifetime callback:
 * - preallocated submit slot: opaque=slot, free_cb=mjpeg_submit_slot_release
 */
static RK_S32 send_mjpeg_to_vdec_buffer(mpp_ctx_t *ctx, void *packet, size_t len,
					void *opaque, RK_S32 (*free_cb)(void *))
{
	MB_EXT_CONFIG_S ext_config;
	VDEC_STREAM_S   stream;
	MB_BLK          mb_blk = RK_NULL;

	if (!packet || len == 0) return RK_FAILURE;

	memset(&ext_config, 0, sizeof(ext_config));
	ext_config.pu8VirAddr = packet;
	ext_config.u64Size    = len;
	ext_config.pOpaque    = opaque;
	ext_config.pFreeCB    = free_cb;

	RK_S32 ret = RK_MPI_SYS_CreateMB(&mb_blk, &ext_config);
	if (ret != RK_SUCCESS) {
		if (free_cb)
			(void)free_cb(opaque);
		return ret;
	}

	memset(&stream, 0, sizeof(stream));
	stream.pMbBlk        = mb_blk;
	stream.u32Len        = (RK_U32)len;
	stream.bEndOfStream  = RK_FALSE;
	stream.bEndOfFrame   = RK_TRUE;
	stream.bBypassMbBlk  = RK_TRUE;

	uint64_t t0 = mono_us();
	ret = RK_MPI_VDEC_SendStream(VDEC_CHN_ID, &stream, VDEC_SUBMIT_TIMEOUT_MS);
	mpp_diag_note_vdec_submit_latency(ctx, mono_us() - t0);
	RK_MPI_MB_ReleaseMB(mb_blk);  /* returns packet/slot via pFreeCB when released */
	return ret;
}

/*
 * Emit the current VENC stream to the output channel.
 *
 * All packs are concatenated into ONE ring slot so that
 * video_ring_pop_latest() always returns a complete frame (VPS+SPS+PPS+IDR
 * or P-frame group) and never a partial one. Used by both camera and
 * test-pattern mode — send_video_frame() in vcpd.c relies on each
 * vsrc_read() call returning exactly one whole H.265 frame.
 */
static uint64_t mpp_emit_venc_stream(mpp_ctx_t *ctx)
{
	RK_U32 npack = ctx->venc_stream.u32PackCount;
	if (npack == 0) npack = 1;
	if (npack > MAX_VENC_PACKS) npack = MAX_VENC_PACKS;

	uint64_t bytes_out = 0;

	/* Concatenate all packs into a single ring slot. */
	static uint8_t concat_buf[VIDEO_RING_SLOT_MAX];
	uint32_t concat_len = 0;
	for (RK_U32 p = 0; p < npack; p++) {
		void   *data = RK_MPI_MB_Handle2VirAddr(ctx->venc_packs[p].pMbBlk);
		RK_U32  dlen = ctx->venc_packs[p].u32Len;
		if (!data || dlen == 0) continue;
		uint32_t room = VIDEO_RING_SLOT_MAX - concat_len;
		if (dlen > room) dlen = room;
		if (dlen == 0) break;
		memcpy(concat_buf + concat_len, data, dlen);
		concat_len += dlen;
	}
	if (concat_len > 0) {
		bool pushed = video_ring_push(ctx->ring, concat_buf, concat_len);
		if (pushed) {
			uint8_t sig = 1;
			(void)write(ctx->pipe_wr, &sig, sizeof(sig));
		} else {
			mpp_diag_note_ring_drop(ctx);
		}
		bytes_out = concat_len;
	}

	mpp_diag_note_venc_stream(ctx, npack, bytes_out);
	return bytes_out;
}

static void fill_color_bars_nv12(uint8_t *y_plane, uint8_t *uv_plane,
				 uint32_t w, uint32_t h, int frame_num)
{
	static const uint8_t bars_r[] = {235,235, 54, 54,235,235, 54, 16};
	static const uint8_t bars_g[] = {235,235,235,235, 54, 54, 54, 16};
	static const uint8_t bars_b[] = {235, 54,235, 54,235, 54,235, 16};
	uint32_t bar_w = w / 8;

	for (uint32_t row = 0; row < h; row++) {
		for (uint32_t col = 0; col < w; col++) {
			int bar = (int)(col / bar_w);
			if (bar > 7) bar = 7;
			uint8_t r = bars_r[bar], g = bars_g[bar], b = bars_b[bar];
			uint32_t marker_y = (uint32_t)(frame_num * 4) % h;
			if (row >= marker_y && row < marker_y + 4)
				r = g = b = 235;
			uint8_t y = (uint8_t)(( 66*r + 129*g +  25*b + 128) / 256 + 16);
			y_plane[row * w + col] = y;
			if ((row & 1) == 0 && (col & 1) == 0) {
				uint8_t u = (uint8_t)((-38*r -  74*g + 112*b + 128) / 256 + 128);
				uint8_t v = (uint8_t)((112*r -  94*g -  18*b + 128) / 256 + 128);
				uint32_t uv_idx        = (row / 2) * w + col;
				uv_plane[uv_idx]     = u;
				uv_plane[uv_idx + 1] = v;
			}
		}
	}
}

/* ------------------------------------------------------------------ pipeline tasks
 *
 * Each task maps 1-to-1 to a Go channel operation:
 *
 *   task_capture_to_ring — select { case f := <-v4l2_ch: mjpeg_ch <- f }  (capture thread)
 *   task_submit_mjpeg    — for f := range tryRecv(mjpeg_ch) { vdec_ch <- f } (encode thread)
 *   task_decode          — for f := range tryRecv(vdec_ch) { venc_ch <- f }
 *   task_encode          — for s := range tryRecv(venc_ch) { ring_ch <- s }
 *   task_pattern_submit  — venc_ch <- generateColorBars(n)
 *
 * Threads call these tasks in a tight event loop — no blocking, no goto. */

/* Capture thread task: poll V4L2 (ENCODE_POLL_MS timeout = event-loop yield).
 * If a frame is ready: DQBUF, publish the raw MJPEG into the capture ring for the
 * encode thread, then re-queue the V4L2 buffer immediately. Keeping capture in
 * its own thread lets V4L2 be serviced at the full camera rate instead of being
 * backpressured by VDEC/VENC latency (which previously pinned cap/dec/enc together). */
static void task_capture_to_ring(mpp_ctx_t *ctx, struct pollfd *pfd)
{
	poll(pfd, 1, ENCODE_POLL_MS);
	if (!(pfd->revents & POLLIN))
		return;

	struct v4l2_buffer buf;
	memset(&buf, 0, sizeof(buf));
	buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	if (xioctl(ctx->v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {
		if (errno != EAGAIN) {
			fprintf(stderr, "vcpd: V4L2 DQBUF: %s\n", strerror(errno));
			ctx->stop = true;
		}
		return;
	}
	mpp_diag_inc_capture(ctx);

	size_t len = buf.bytesused;
	if (len > 0) {
		if (video_ring_push(ctx->mjpeg_ring, ctx->v4l2_bufs[buf.index].start,
				    (uint32_t)len)) {
			uint8_t sig = 1;
			(void)write(ctx->mjpeg_pipe_wr, &sig, sizeof(sig));
		} else {
			/* Encode thread is behind — drop this frame to bound latency. */
			mpp_diag_note_cap_ring_drop(ctx);
		}
	}
	xioctl(ctx->v4l2_fd, VIDIOC_QBUF, &buf);  /* re-queue immediately */
}

/* Drain all decoded NV12 frames from VDEC and submit each to VENC.
 * Both calls are non-blocking (timeout=0): returns immediately if nothing ready. */
static void task_decode(mpp_ctx_t *ctx)
{
	for (;;) {
		VIDEO_FRAME_INFO_S frame;
		memset(&frame, 0, sizeof(frame));
		uint64_t t0 = mono_us();
		RK_S32 ret = RK_MPI_VDEC_GetFrame(VDEC_CHN_ID, &frame, VDEC_GETFRAME_TIMEOUT_MS);
		mpp_diag_note_vdec_getframe_latency(ctx, mono_us() - t0);
		if (ret != RK_SUCCESS) break;
		mpp_diag_inc_vdec_frame(ctx);

		/*
		 * Hue-swap compensation: yellow renders green and blue renders
		 * pink/magenta on this board — the textbook symptom of the Cb/Cr
		 * (U/V) chroma planes being transposed. init_vdec()/init_venc()
		 * both declare RK_FMT_YUV420SP (NV12, U-before-V) and agree with
		 * each other and with Rockchip's own sample, so this isn't a
		 * mismatch in vcpd's own format declarations — it's the RV1106
		 * VDEC's actual JPEG-decode chroma byte order not matching what
		 * it declares. The bytes VDEC wrote aren't touched; only how VENC
		 * is told to interpret them, which corrects the swap without
		 * needing to know why the hardware does this. If this ever gets
		 * fixed upstream (or doesn't reproduce on other board revisions),
		 * this is the one line to remove.
		 */
		frame.stVFrame.enPixelFormat = RK_FMT_YUV420SP_VU;

		uint64_t vs = mono_us();
		RK_S32 sr = RK_MPI_VENC_SendFrame(VENC_CHN_ID, &frame, VENC_SEND_TIMEOUT_MS);
		mpp_diag_note_venc_submit_latency(ctx, mono_us() - vs);
		RK_MPI_VDEC_ReleaseFrame(VDEC_CHN_ID, &frame);
		if (sr == RK_SUCCESS) mpp_diag_inc_venc_submit(ctx);
	}
}

/* Encode thread task: pull queued MJPEG frames from the capture ring into a
 * fixed in-flight slot pool and submit them to VDEC in small bursts. This keeps
 * VDEC fed without per-frame malloc/free and removes one memcpy from the hot path
 * (ring -> heap). One V4L2->ring copy remains; eliminating that would require a
 * larger V4L2 ownership / DMABUF redesign.
 */
static void task_submit_mjpeg(mpp_ctx_t *ctx)
{
	for (int submitted = 0; submitted < MJPEG_SUBMIT_BURST; submitted++) {
		mjpeg_submit_slot_t *slot = mjpeg_submit_slot_acquire(ctx);
		if (slot == NULL)
			break;

		if (!video_ring_pop(ctx->mjpeg_ring, slot->data, sizeof(slot->data), &slot->len)) {
			atomic_store_explicit(&slot->in_use, false, memory_order_release);
			break;
		}
		if (slot->len == 0) {
			atomic_store_explicit(&slot->in_use, false, memory_order_release);
			continue;
		}

		RK_S32 ret = send_mjpeg_to_vdec_buffer(ctx, slot->data, slot->len,
						      slot, mjpeg_submit_slot_release);
		mpp_diag_note_vdec_submit(ctx, ret);
		if (ret != RK_SUCCESS) {
			/* On failure the slot is released by send_mjpeg_to_vdec_buffer() via
			 * the supplied callback; stop bursting and let decode/encode catch up. */
			break;
		}
	}
}

/* Drain all encoded H.265 streams from VENC and push to ring or pipe.
 * Non-blocking (timeout=0).  Shared by camera and pattern threads. */
static void task_encode(mpp_ctx_t *ctx, int *frame_num)
{
	for (;;) {
		ctx->venc_stream.pstPack = ctx->venc_packs;
		uint64_t t0 = mono_us();
		if (RK_MPI_VENC_GetStream(VENC_CHN_ID, &ctx->venc_stream,
					 VENC_GETSTREAM_TIMEOUT_MS) != RK_SUCCESS)
			break;
		mpp_diag_note_venc_getstream_latency(ctx, mono_us() - t0);
		mpp_emit_venc_stream(ctx);
		RK_MPI_VENC_ReleaseStream(VENC_CHN_ID, &ctx->venc_stream);
		if (*frame_num == 0)
			fprintf(stderr, "vcpd: [mpp] first frame encoded OK\n");
		(*frame_num)++;
	}
}

/* Generate one NV12 color-bar frame and submit to VENC (non-blocking). */
static void task_pattern_submit(mpp_ctx_t *ctx, int n)
{
	uint32_t w = ctx->actual_width, h = ctx->actual_height;
	uint32_t y_size = w * h, uv_size = w * h / 2;

	MB_BLK mb_blk = RK_NULL;
	if (RK_MPI_SYS_MmzAlloc(&mb_blk, NULL, NULL, y_size + uv_size) != RK_SUCCESS || !mb_blk)
		return;

	uint8_t *vaddr = (uint8_t *)RK_MPI_MB_Handle2VirAddr(mb_blk);
	fill_color_bars_nv12(vaddr, vaddr + y_size, w, h, n);

	VIDEO_FRAME_INFO_S fi;
	memset(&fi, 0, sizeof(fi));
	fi.stVFrame.u32Width      = w;
	fi.stVFrame.u32Height     = h;
	fi.stVFrame.u32VirWidth   = w;
	fi.stVFrame.u32VirHeight  = h;
	fi.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
	fi.stVFrame.pMbBlk        = mb_blk;

	RK_S32 ret = RK_MPI_VENC_SendFrame(VENC_CHN_ID, &fi, 0);
	RK_MPI_MB_ReleaseMB(mb_blk);
	if (ret == RK_SUCCESS) mpp_diag_inc_venc_submit(ctx);
}

/* ------------------------------------------------------------------ test-pattern thread */

static void *mpp_pattern_thread(void *arg)
{
	mpp_ctx_t *ctx = (mpp_ctx_t *)arg;
	int fps         = ctx->src->fps > 0 ? ctx->src->fps : 25;
	uint32_t interval_us = 1000000 / (uint32_t)fps;
	int submit_num  = 0;   /* color-bars animation frame index */
	int encode_num  = 0;   /* counts encoded frames (for first-frame log) */

	fprintf(stderr, "vcpd: test pattern %ux%u @ %d fps\n",
		ctx->actual_width, ctx->actual_height, fps);

	while (!ctx->stop) {
		task_pattern_submit(ctx, submit_num++);   /* generate NV12 → VENC   */
		task_encode(ctx, &encode_num);             /* drain VENC → pipe      */
		mpp_diag_maybe_report(ctx);
		usleep(interval_us);                       /* FPS timer              */
	}
	task_encode(ctx, &encode_num);                /* flush residual streams  */

	if (ctx->pipe_wr >= 0) { close(ctx->pipe_wr); ctx->pipe_wr = -1; }
	return NULL;
}

/* ------------------------------------------------------------------ camera capture thread */

/* Dedicated V4L2 capture thread: services the camera at its full frame rate and
 * publishes raw MJPEG frames into the capture ring, decoupled from VDEC/VENC. */
static void *mpp_capture_thread(void *arg)
{
	mpp_ctx_t *ctx = (mpp_ctx_t *)arg;
	struct pollfd pfd = { .fd = ctx->v4l2_fd, .events = POLLIN };

	while (!ctx->stop)
		task_capture_to_ring(ctx, &pfd);   /* select: V4L2 → MJPEG ring */

	/* EOF the MJPEG channel so the encode thread never blocks on shutdown. */
	if (ctx->mjpeg_pipe_wr >= 0) { close(ctx->mjpeg_pipe_wr); ctx->mjpeg_pipe_wr = -1; }
	return NULL;
}

/* ------------------------------------------------------------------ camera encode thread */

static void *mpp_encode_thread(void *arg)
{
	mpp_ctx_t *ctx = (mpp_ctx_t *)arg;
	struct pollfd pfd = { .fd = ctx->mjpeg_pipe_rd, .events = POLLIN };
	int frame_num = 0;

	while (!ctx->stop) {
		/* select: wait briefly for the capture thread to publish MJPEG frames,
		 * but keep draining VDEC/VENC even when no new frame has arrived. */
		poll(&pfd, 1, ENCODE_POLL_MS);
		if (pfd.revents & POLLIN) {
			uint8_t drain[64];
			(void)read(ctx->mjpeg_pipe_rd, drain, sizeof(drain));  /* clear wakeups */
		}
		task_submit_mjpeg(ctx);            /* drain: MJPEG ring → VDEC */
		task_decode(ctx);                  /* drain: VDEC → VENC       */
		task_encode(ctx, &frame_num);      /* drain: VENC → ring       */
		mpp_diag_maybe_report(ctx);
	}
	task_encode(ctx, &frame_num);         /* flush residual streams */

	mpp_diag_maybe_report(ctx);
	if (ctx->pipe_wr >= 0) { close(ctx->pipe_wr); ctx->pipe_wr = -1; }
	return NULL;
}

/* ------------------------------------------------------------------ public API */

int video_source_mpp_start(video_source_mpp_t *src)
{
	if (!src || src->running) return -1;

	if (!src->device[0]) strncpy(src->device, "/dev/video21", sizeof(src->device) - 1);
	if (src->bitrate_kbps <= 0) src->bitrate_kbps = 256;
	if (src->width  <= 0) src->width  = 480;
	if (src->height <= 0) src->height = 320;
	if (src->fps    <= 0) src->fps    = 15;

	mpp_ctx_t *ctx = calloc(1, sizeof(mpp_ctx_t));
	if (!ctx) return -1;
	ctx->src       = src;
	ctx->v4l2_fd   = -1;
	ctx->pipe_wr   = -1;
	ctx->mjpeg_pipe_rd = -1;
	ctx->mjpeg_pipe_wr = -1;

	if (pthread_mutex_init(&ctx->diag_lock, NULL) != 0) { free(ctx); return -1; }
	ctx->diag_lock_init = true;

	RK_S32 ret = RK_MPI_SYS_Init();
	if (ret != RK_SUCCESS) goto fail_early;

	int pipefd[2];
	if (pipe(pipefd) < 0) { RK_MPI_SYS_Exit(); goto fail_early; }
	src->pipe_fd = pipefd[0];
	ctx->pipe_wr = pipefd[1];

	if (src->test_pattern) {
		/* ---- test-pattern mode: VENC only, no camera / VDEC ---- */
		ctx->actual_width   = (uint32_t)src->width;
		ctx->actual_height  = (uint32_t)src->height;
		ctx->actual_fps_num = (uint32_t)src->fps;
		ctx->actual_fps_den = 1;
		ctx->actual_fps     = (uint32_t)src->fps;

		ret = init_venc(ctx);
		if (ret != RK_SUCCESS) { close(pipefd[0]); close(pipefd[1]); goto fail; }

		ctx->ring = video_ring_create(VIDEO_RING_FILE);
		if (!ctx->ring) {
			fprintf(stderr, "vcpd: [mpp] failed to create ring at %s: %s\n",
				VIDEO_RING_FILE, strerror(errno));
			deinit_venc();
			close(pipefd[0]); close(pipefd[1]);
			goto fail;
		}
		fprintf(stderr, "vcpd: [mpp] ring buffer %zu KB at %s\n",
			VIDEO_RING_MMAP_SIZE / 1024, VIDEO_RING_FILE);

		if (pthread_create(&ctx->encode_thread, NULL, mpp_pattern_thread, ctx) != 0) {
			video_ring_destroy(ctx->ring, VIDEO_RING_FILE); ctx->ring = NULL;
			deinit_venc();
			close(pipefd[0]); close(pipefd[1]);
			goto fail;
		}
		ctx->encode_thread_started = true;

	} else {
		/* ---- camera mode: V4L2 → VDEC → VENC → ring ---- */
		ctx->is_camera = true;
		if (v4l2_open(ctx) < 0) { close(pipefd[0]); close(pipefd[1]); goto fail; }

		fprintf(stderr,
			"vcpd: [mpp] capture req=%dx%d@%d fps, got=%ux%u @ %u/%u fps (~%u)\n",
			src->width, src->height, src->fps,
			ctx->actual_width, ctx->actual_height,
			ctx->actual_fps_num > 0 ? ctx->actual_fps_num : (uint32_t)src->fps,
			ctx->actual_fps_den > 0 ? ctx->actual_fps_den : 1,
			ctx->actual_fps);

		ret = init_vdec(ctx->actual_width, ctx->actual_height);
		if (ret != RK_SUCCESS) { v4l2_cleanup(ctx); close(pipefd[0]); close(pipefd[1]); goto fail; }

		ret = init_venc(ctx);
		if (ret != RK_SUCCESS) { deinit_vdec(); v4l2_cleanup(ctx); close(pipefd[0]); close(pipefd[1]); goto fail; }

		fprintf(stderr,
			"vcpd: [mpp] venc rc src_fps=%u/%u dst_fps=%d/1 bitrate=%d gop=%d\n",
			ctx->actual_fps_num > 0 ? ctx->actual_fps_num : (uint32_t)src->fps,
			ctx->actual_fps_den > 0 ? ctx->actual_fps_den : 1,
			src->fps, src->bitrate_kbps, src->gop > 0 ? src->gop : 5);

		ctx->ring = video_ring_create(VIDEO_RING_FILE);
		if (!ctx->ring) {
			fprintf(stderr, "vcpd: [mpp] failed to create ring at %s: %s\n",
				VIDEO_RING_FILE, strerror(errno));
			deinit_venc(); deinit_vdec(); v4l2_cleanup(ctx);
			close(pipefd[0]); close(pipefd[1]);
			goto fail;
		}
		fprintf(stderr, "vcpd: [mpp] ring buffer %zu KB at %s\n",
			VIDEO_RING_MMAP_SIZE / 1024, VIDEO_RING_FILE);

		if (v4l2_stream_on(ctx) < 0) {
			video_ring_destroy(ctx->ring, VIDEO_RING_FILE); ctx->ring = NULL;
			deinit_venc(); deinit_vdec(); v4l2_cleanup(ctx);
			close(pipefd[0]); close(pipefd[1]);
			goto fail;
		}

		/* Capture→decode channel: a dedicated capture thread feeds raw MJPEG
		 * frames through this ring so V4L2 runs at the full camera rate,
		 * decoupled from VDEC/VENC (mirrors the encoder→sender ring+pipe). */
		ctx->mjpeg_ring = video_ring_create(MJPEG_RING_FILE);
		if (!ctx->mjpeg_ring) {
			fprintf(stderr, "vcpd: [mpp] failed to create MJPEG ring at %s: %s\n",
				MJPEG_RING_FILE, strerror(errno));
			video_ring_destroy(ctx->ring, VIDEO_RING_FILE); ctx->ring = NULL;
			deinit_venc(); deinit_vdec(); v4l2_cleanup(ctx);
			close(pipefd[0]); close(pipefd[1]);
			goto fail;
		}

		int mjpeg_pipefd[2];
		if (pipe(mjpeg_pipefd) < 0) {
			video_ring_destroy(ctx->mjpeg_ring, MJPEG_RING_FILE); ctx->mjpeg_ring = NULL;
			video_ring_destroy(ctx->ring, VIDEO_RING_FILE); ctx->ring = NULL;
			deinit_venc(); deinit_vdec(); v4l2_cleanup(ctx);
			close(pipefd[0]); close(pipefd[1]);
			goto fail;
		}
		ctx->mjpeg_pipe_rd = mjpeg_pipefd[0];
		ctx->mjpeg_pipe_wr = mjpeg_pipefd[1];
		fprintf(stderr, "vcpd: [mpp] MJPEG ring buffer %zu KB at %s\n",
			VIDEO_RING_MMAP_SIZE / 1024, MJPEG_RING_FILE);

		/* Start capture first, then encode: on encode-create failure the capture
		 * thread rolls back cleanly (it only owns the MJPEG channel). */
		if (pthread_create(&ctx->capture_thread, NULL, mpp_capture_thread, ctx) != 0) {
			close(ctx->mjpeg_pipe_rd); ctx->mjpeg_pipe_rd = -1;
			close(ctx->mjpeg_pipe_wr); ctx->mjpeg_pipe_wr = -1;
			video_ring_destroy(ctx->mjpeg_ring, MJPEG_RING_FILE); ctx->mjpeg_ring = NULL;
			video_ring_destroy(ctx->ring, VIDEO_RING_FILE); ctx->ring = NULL;
			deinit_venc(); deinit_vdec(); v4l2_cleanup(ctx);
			close(pipefd[0]); close(pipefd[1]);
			goto fail;
		}
		ctx->capture_thread_started = true;

		if (pthread_create(&ctx->encode_thread, NULL, mpp_encode_thread, ctx) != 0) {
			ctx->stop = true;
			pthread_join(ctx->capture_thread, NULL);
			ctx->capture_thread_started = false;
			ctx->stop = false;
			if (ctx->mjpeg_pipe_rd >= 0) { close(ctx->mjpeg_pipe_rd); ctx->mjpeg_pipe_rd = -1; }
			if (ctx->mjpeg_pipe_wr >= 0) { close(ctx->mjpeg_pipe_wr); ctx->mjpeg_pipe_wr = -1; }
			video_ring_destroy(ctx->mjpeg_ring, MJPEG_RING_FILE); ctx->mjpeg_ring = NULL;
			video_ring_destroy(ctx->ring, VIDEO_RING_FILE); ctx->ring = NULL;
			v4l2_cleanup(ctx);
			deinit_venc(); deinit_vdec();
			close(pipefd[0]); close(pipefd[1]);
			goto fail;
		}
		ctx->encode_thread_started = true;
	}

	mpp_diag_reset(ctx, mono_ms());
	src->mpp_ctx = ctx;
	src->running = true;
	return 0;

fail:
	RK_MPI_SYS_Exit();
fail_early:
	ctx->stop = true;
	if (ctx->capture_thread_started) pthread_join(ctx->capture_thread, NULL);
	if (ctx->encode_thread_started) pthread_join(ctx->encode_thread, NULL);
	if (ctx->diag_lock_init) pthread_mutex_destroy(&ctx->diag_lock);
	free(ctx);
	return -1;
}

void video_source_mpp_stop(video_source_mpp_t *src)
{
	if (!src || !src->running || !src->mpp_ctx) return;

	mpp_ctx_t *ctx = (mpp_ctx_t *)src->mpp_ctx;
	ctx->stop = true;
	if (ctx->capture_thread_started) pthread_join(ctx->capture_thread, NULL);
	if (ctx->encode_thread_started) pthread_join(ctx->encode_thread, NULL);

	v4l2_cleanup(ctx);
	deinit_venc();
	if (ctx->is_camera) deinit_vdec();

	if (ctx->pipe_wr >= 0) { close(ctx->pipe_wr); ctx->pipe_wr = -1; }
	if (src->pipe_fd >= 0) { close(src->pipe_fd); src->pipe_fd = -1; }
	if (ctx->mjpeg_pipe_wr >= 0) { close(ctx->mjpeg_pipe_wr); ctx->mjpeg_pipe_wr = -1; }
	if (ctx->mjpeg_pipe_rd >= 0) { close(ctx->mjpeg_pipe_rd); ctx->mjpeg_pipe_rd = -1; }

	if (ctx->ring) { video_ring_destroy(ctx->ring, VIDEO_RING_FILE); ctx->ring = NULL; }
	if (ctx->mjpeg_ring) { video_ring_destroy(ctx->mjpeg_ring, MJPEG_RING_FILE); ctx->mjpeg_ring = NULL; }

	RK_MPI_SYS_Exit();
	if (ctx->diag_lock_init) pthread_mutex_destroy(&ctx->diag_lock);
	free(ctx);
	src->mpp_ctx = NULL;
	src->running = false;
}

void video_source_mpp_request_idr(video_source_mpp_t *src)
{
	if (!src || !src->running || !src->mpp_ctx)
		return;
	/* Signal VENC to emit VPS+SPS+PPS+IDR at the start of the next frame.
	 * Called when UVCP READY is received so the receiver can start decoding
	 * within one frame interval instead of waiting for the next natural IDR. */
	RK_MPI_VENC_RequestIDR(VENC_CHN_ID, RK_TRUE);
}

ssize_t video_source_mpp_read(video_source_mpp_t *src, uint8_t *buf, size_t len)
{
	if (!src || !src->running || src->pipe_fd < 0) return -1;

	mpp_ctx_t *ctx = (mpp_ctx_t *)src->mpp_ctx;
	if (!ctx || !ctx->ring) return -1;

	/*
	 * Pipe carries 1-byte wake signals only (both camera and test-pattern
	 * mode). EOF on pipe means the encoder thread exited → shutdown.
	 * Use pop_latest so the sender always transmits the freshest frame.
	 */
	uint8_t sig;
	ssize_t r = read(src->pipe_fd, &sig, sizeof(sig));
	if (r == 0) return 0;           /* EOF → shutdown */
	if (r < 0) {
		if (errno == EINTR || errno == EAGAIN) return 0;
		return -1;
	}
	uint32_t chunk_len = 0;
	if (!video_ring_pop_latest(ctx->ring, buf, len, &chunk_len))
		return 0;  /* spurious signal */
	return (ssize_t)chunk_len;
}

#endif /* VCPD_WITH_MPP */
