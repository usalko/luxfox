/*
 * MPP hardware video backend for vcpd.
 *
 * Pipeline: V4L2 MJPEG capture → VDEC (HW MJPEG decode) → VENC (HW H.264 encode)
 * Based on working sample: samples/example/demo/sample_demo_v4l2_mjpeg_vdec_venc.c
 *
 * Uses RK MPI directly — no dependency on sample_comm.h.
 */

#ifdef VCPD_WITH_MPP

#include "vcpd/video_source.h"
#include "vcpd/video_ring.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "rk_debug.h"
#include "rockchip/mpp_err.h"
#include "rk_mpi_cal.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vdec.h"
#include "rk_mpi_venc.h"

#define V4L2_BUFFER_COUNT 4
#define VDEC_CHN_ID 0
#define VENC_CHN_ID 0
#define MJPEG_QUEUE_SIZE 16

/* Frames to batch-submit to VDEC per worker iteration */
#define BATCH_FEED 4
/* Timeout (ms) when waiting for first decoded frame after a batch submit */
#define VDEC_DRAIN_TIMEOUT_MS 40
/* Timeout (ms) when waiting for first encoded stream after VENC sends */
#define VENC_DRAIN_TIMEOUT_MS 40

typedef struct {
	void *start;
	size_t length;
} mpp_v4l2_buffer_t;

typedef struct {
	uint8_t *data;
	size_t len;
} mjpeg_packet_t;

#define MAX_VENC_PACKS 8

typedef struct {
	uint64_t window_start_ms;
	uint32_t v4l2_dq_ok;
	uint32_t vdec_submit_ok;
	uint32_t vdec_submit_busy;
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
	uint32_t queue_drop;
	uint32_t queue_depth_peak;
	uint32_t ring_drop;
} mpp_diag_t;

typedef struct {
	video_source_mpp_t *src;

	int v4l2_fd;
	mpp_v4l2_buffer_t v4l2_bufs[V4L2_BUFFER_COUNT];
	uint32_t actual_width;
	uint32_t actual_height;
	uint32_t actual_fps;
	uint32_t actual_fps_num;
	uint32_t actual_fps_den;

	VENC_STREAM_S venc_stream;
	VENC_PACK_S venc_packs[MAX_VENC_PACKS];

	int pipe_wr;
	pthread_t capture_thread;
	pthread_t process_thread;
	bool capture_thread_started;
	bool process_thread_started;
	volatile bool stop;
	bool diag_lock_init;
	pthread_mutex_t diag_lock;
	bool queue_sync_init;
	pthread_mutex_t queue_lock;
	pthread_cond_t queue_cond;
	mjpeg_packet_t mjpeg_queue[MJPEG_QUEUE_SIZE];
	uint16_t mjpeg_head;
	uint16_t mjpeg_tail;
	uint16_t mjpeg_count;
	mpp_diag_t diag;

	/* Ring buffer used in camera mode; NULL in test-pattern mode */
	video_ring_t *ring;
} mpp_ctx_t;

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

static void mpp_diag_note_vdec_getframe_latency(mpp_ctx_t *ctx, uint64_t duration_us)
{
	pthread_mutex_lock(&ctx->diag_lock);
	ctx->diag.vdec_getframe_calls++;
	ctx->diag.vdec_getframe_us_total += duration_us;
	if (duration_us > ctx->diag.vdec_getframe_us_max)
		ctx->diag.vdec_getframe_us_max = (uint32_t)duration_us;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_note_vdec_submit(mpp_ctx_t *ctx, RK_S32 ret)
{
	pthread_mutex_lock(&ctx->diag_lock);
	if (ret == RK_SUCCESS)
		ctx->diag.vdec_submit_ok++;
	else if (ret == MPP_ERR_TIMEOUT)
		ctx->diag.vdec_submit_busy++;
	else
		ctx->diag.vdec_submit_drop++;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_note_vdec_submit_latency(mpp_ctx_t *ctx, uint64_t duration_us)
{
	pthread_mutex_lock(&ctx->diag_lock);
	ctx->diag.vdec_submit_calls++;
	ctx->diag.vdec_submit_us_total += duration_us;
	if (duration_us > ctx->diag.vdec_submit_us_max)
		ctx->diag.vdec_submit_us_max = (uint32_t)duration_us;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_inc_venc_submit(mpp_ctx_t *ctx)
{
	pthread_mutex_lock(&ctx->diag_lock);
	ctx->diag.venc_send_ok++;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_note_venc_submit_latency(mpp_ctx_t *ctx, uint64_t duration_us)
{
	pthread_mutex_lock(&ctx->diag_lock);
	ctx->diag.venc_send_calls++;
	ctx->diag.venc_send_us_total += duration_us;
	if (duration_us > ctx->diag.venc_send_us_max)
		ctx->diag.venc_send_us_max = (uint32_t)duration_us;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_note_venc_stream(mpp_ctx_t *ctx, uint32_t nals, uint64_t bytes)
{
	pthread_mutex_lock(&ctx->diag_lock);
	ctx->diag.venc_stream_ok++;
	ctx->diag.nal_writes += nals;
	ctx->diag.nal_bytes += bytes;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_note_venc_getstream_latency(mpp_ctx_t *ctx, uint64_t duration_us)
{
	pthread_mutex_lock(&ctx->diag_lock);
	ctx->diag.venc_getstream_calls++;
	ctx->diag.venc_getstream_us_total += duration_us;
	if (duration_us > ctx->diag.venc_getstream_us_max)
		ctx->diag.venc_getstream_us_max = (uint32_t)duration_us;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_note_queue(mpp_ctx_t *ctx, uint32_t dropped, uint32_t depth)
{
	pthread_mutex_lock(&ctx->diag_lock);
	ctx->diag.queue_drop += dropped;
	if (depth > ctx->diag.queue_depth_peak)
		ctx->diag.queue_depth_peak = depth;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_note_ring_drop(mpp_ctx_t *ctx)
{
	pthread_mutex_lock(&ctx->diag_lock);
	ctx->diag.ring_drop++;
	pthread_mutex_unlock(&ctx->diag_lock);
}

static void mpp_diag_maybe_report(mpp_ctx_t *ctx)
{
	mpp_diag_t snapshot;
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

	snapshot = ctx->diag;
	mpp_diag_reset_locked(ctx, now_ms);
	pthread_mutex_unlock(&ctx->diag_lock);

	uint32_t fps_cap_x10 = (uint32_t)(snapshot.v4l2_dq_ok * 10000 / (dt > 0 ? dt : 1));
	uint32_t fps_vdec_in_x10 = (uint32_t)(snapshot.vdec_submit_ok * 10000 / (dt > 0 ? dt : 1));
	uint32_t fps_dec_x10 = (uint32_t)(snapshot.vdec_frame_ok * 10000 / (dt > 0 ? dt : 1));
	uint32_t fps_sub_x10 = (uint32_t)(snapshot.venc_send_ok * 10000 / (dt > 0 ? dt : 1));
	uint32_t fps_enc_x10 = (uint32_t)(snapshot.venc_stream_ok * 10000 / (dt > 0 ? dt : 1));
	uint32_t avg_vdec_submit_us = snapshot.vdec_submit_calls > 0
		? (uint32_t)(snapshot.vdec_submit_us_total / snapshot.vdec_submit_calls) : 0;
	uint32_t avg_vdec_getframe_us = snapshot.vdec_getframe_calls > 0
		? (uint32_t)(snapshot.vdec_getframe_us_total / snapshot.vdec_getframe_calls) : 0;
	uint32_t avg_venc_submit_us = snapshot.venc_send_calls > 0
		? (uint32_t)(snapshot.venc_send_us_total / snapshot.venc_send_calls) : 0;
	uint32_t avg_venc_getstream_us = snapshot.venc_getstream_calls > 0
		? (uint32_t)(snapshot.venc_getstream_us_total / snapshot.venc_getstream_calls) : 0;
	uint32_t negotiated_num = ctx->actual_fps_num > 0
		? ctx->actual_fps_num : (uint32_t)(ctx->src->fps > 0 ? ctx->src->fps : 15);
	uint32_t negotiated_den = ctx->actual_fps_den > 0 ? ctx->actual_fps_den : 1;
	char ts[9]; { time_t t = time(NULL); strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t)); }
	fprintf(stderr,
		"%s vcpd: [mpp] cap=%u vin=%u busy=%u drop=%u dec=%u sub=%u enc=%u nals=%u bytes=%llu qdrop=%u qpeak=%u rdrop=%u | "
		"fps cap=%u.%u vin=%u.%u dec=%u.%u sub=%u.%u enc=%u.%u | "
		"lat_us vdec_in=%u/%u vdec_out=%u/%u venc_in=%u/%u venc_out=%u/%u | negotiated=%u/%u fps\n",
		ts,
		snapshot.v4l2_dq_ok,
		snapshot.vdec_submit_ok,
		snapshot.vdec_submit_busy,
		snapshot.vdec_submit_drop,
		snapshot.vdec_frame_ok,
		snapshot.venc_send_ok,
		snapshot.venc_stream_ok,
		snapshot.nal_writes,
		(unsigned long long)snapshot.nal_bytes,
		snapshot.queue_drop,
		snapshot.queue_depth_peak,
		snapshot.ring_drop,
		fps_cap_x10 / 10, fps_cap_x10 % 10,
		fps_vdec_in_x10 / 10, fps_vdec_in_x10 % 10,
		fps_dec_x10 / 10, fps_dec_x10 % 10,
		fps_sub_x10 / 10, fps_sub_x10 % 10,
		fps_enc_x10 / 10, fps_enc_x10 % 10,
		avg_vdec_submit_us, snapshot.vdec_submit_us_max,
		avg_vdec_getframe_us, snapshot.vdec_getframe_us_max,
		avg_venc_submit_us, snapshot.venc_send_us_max,
		avg_venc_getstream_us, snapshot.venc_getstream_us_max,
		negotiated_num,
		negotiated_den);
}

static int mpp_queue_init(mpp_ctx_t *ctx)
{
	if (pthread_mutex_init(&ctx->queue_lock, NULL) != 0)
		return -1;
	if (pthread_cond_init(&ctx->queue_cond, NULL) != 0) {
		pthread_mutex_destroy(&ctx->queue_lock);
		return -1;
	}
	ctx->queue_sync_init = true;
	return 0;
}

static void mpp_queue_wake(mpp_ctx_t *ctx)
{
	if (!ctx->queue_sync_init)
		return;
	pthread_mutex_lock(&ctx->queue_lock);
	pthread_cond_broadcast(&ctx->queue_cond);
	pthread_mutex_unlock(&ctx->queue_lock);
}

static void mpp_queue_clear(mpp_ctx_t *ctx)
{
	if (!ctx->queue_sync_init)
		return;
	pthread_mutex_lock(&ctx->queue_lock);
	while (ctx->mjpeg_count > 0) {
		mjpeg_packet_t *pkt = &ctx->mjpeg_queue[ctx->mjpeg_tail];
		free(pkt->data);
		pkt->data = NULL;
		pkt->len = 0;
		ctx->mjpeg_tail = (uint16_t)((ctx->mjpeg_tail + 1U) % MJPEG_QUEUE_SIZE);
		ctx->mjpeg_count--;
	}
	ctx->mjpeg_head = 0;
	ctx->mjpeg_tail = 0;
	pthread_mutex_unlock(&ctx->queue_lock);
}

static void mpp_queue_destroy(mpp_ctx_t *ctx)
{
	if (!ctx->queue_sync_init)
		return;
	mpp_queue_clear(ctx);
	pthread_cond_destroy(&ctx->queue_cond);
	pthread_mutex_destroy(&ctx->queue_lock);
	ctx->queue_sync_init = false;
}

static bool mpp_queue_push(mpp_ctx_t *ctx, uint8_t *data, size_t len)
{
	uint32_t dropped = 0;
	uint32_t depth = 0;

	pthread_mutex_lock(&ctx->queue_lock);
	if (ctx->mjpeg_count == MJPEG_QUEUE_SIZE) {
		mjpeg_packet_t *oldest = &ctx->mjpeg_queue[ctx->mjpeg_tail];
		free(oldest->data);
		oldest->data = NULL;
		oldest->len = 0;
		ctx->mjpeg_tail = (uint16_t)((ctx->mjpeg_tail + 1U) % MJPEG_QUEUE_SIZE);
		ctx->mjpeg_count--;
		dropped = 1;
	}

	mjpeg_packet_t *slot = &ctx->mjpeg_queue[ctx->mjpeg_head];
	slot->data = data;
	slot->len = len;
	ctx->mjpeg_head = (uint16_t)((ctx->mjpeg_head + 1U) % MJPEG_QUEUE_SIZE);
	ctx->mjpeg_count++;
	depth = ctx->mjpeg_count;
	pthread_cond_signal(&ctx->queue_cond);
	pthread_mutex_unlock(&ctx->queue_lock);

	mpp_diag_note_queue(ctx, dropped, depth);
	return true;
}

static bool mpp_queue_pop(mpp_ctx_t *ctx, mjpeg_packet_t *out)
{
	bool have_packet = false;

	pthread_mutex_lock(&ctx->queue_lock);
	if (ctx->mjpeg_count > 0) {
		*out = ctx->mjpeg_queue[ctx->mjpeg_tail];
		ctx->mjpeg_queue[ctx->mjpeg_tail].data = NULL;
		ctx->mjpeg_queue[ctx->mjpeg_tail].len = 0;
		ctx->mjpeg_tail = (uint16_t)((ctx->mjpeg_tail + 1U) % MJPEG_QUEUE_SIZE);
		ctx->mjpeg_count--;
		have_packet = true;
	}
	pthread_mutex_unlock(&ctx->queue_lock);
	return have_packet;
}

static void mpp_queue_wait(mpp_ctx_t *ctx, int timeout_ms)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += timeout_ms / 1000;
	ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec += 1;
		ts.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&ctx->queue_lock);
	if (!ctx->stop && ctx->mjpeg_count == 0)
		pthread_cond_timedwait(&ctx->queue_cond, &ctx->queue_lock, &ts);
	pthread_mutex_unlock(&ctx->queue_lock);
}

static void mpp_update_actual_fps(mpp_ctx_t *ctx, const struct v4l2_streamparm *parm)
{
	uint32_t tpf_num = parm->parm.capture.timeperframe.numerator;
	uint32_t tpf_den = parm->parm.capture.timeperframe.denominator;

	if (tpf_num == 0 || tpf_den == 0) {
		ctx->actual_fps_num = (uint32_t)(ctx->src->fps > 0 ? ctx->src->fps : 15);
		ctx->actual_fps_den = 1;
		ctx->actual_fps = ctx->actual_fps_num;
		return;
	}

	ctx->actual_fps_num = tpf_den;
	ctx->actual_fps_den = tpf_num;
	ctx->actual_fps = (tpf_den + (tpf_num / 2)) / tpf_num;
}

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;
	do {
		ret = ioctl(fd, request, arg);
	} while (ret == -1 && errno == EINTR);
	return ret;
}

static RK_S32 mjpeg_packet_free(void *opaque)
{
	free(opaque);
	return RK_SUCCESS;
}

static int v4l2_open(mpp_ctx_t *ctx)
{
	struct v4l2_capability cap;
	struct v4l2_format fmt;
	struct v4l2_streamparm parm;
	struct v4l2_requestbuffers req;

	ctx->v4l2_fd = open(ctx->src->device, O_RDWR, 0);
	if (ctx->v4l2_fd < 0)
		return -1;

	if (xioctl(ctx->v4l2_fd, VIDIOC_QUERYCAP, &cap) < 0)
		return -1;

	if (!(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) ||
	    !(cap.device_caps & V4L2_CAP_STREAMING))
		return -1;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = (unsigned)ctx->src->width;
	fmt.fmt.pix.height = (unsigned)ctx->src->height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if (xioctl(ctx->v4l2_fd, VIDIOC_S_FMT, &fmt) < 0)
		return -1;

	ctx->actual_width = fmt.fmt.pix.width;
	ctx->actual_height = fmt.fmt.pix.height;

	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = (unsigned)ctx->src->fps;
	if (xioctl(ctx->v4l2_fd, VIDIOC_S_PARM, &parm) == 0) {
		struct v4l2_streamparm actual_parm;
		memset(&actual_parm, 0, sizeof(actual_parm));
		actual_parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (xioctl(ctx->v4l2_fd, VIDIOC_G_PARM, &actual_parm) == 0)
			parm = actual_parm;
	}
	mpp_update_actual_fps(ctx, &parm);

	memset(&req, 0, sizeof(req));
	req.count = V4L2_BUFFER_COUNT;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req) < 0)
		return -1;

	for (int i = 0; i < V4L2_BUFFER_COUNT; i++) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = (unsigned)i;

		if (xioctl(ctx->v4l2_fd, VIDIOC_QUERYBUF, &buf) < 0)
			return -1;

		ctx->v4l2_bufs[i].length = buf.length;
		ctx->v4l2_bufs[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
						MAP_SHARED, ctx->v4l2_fd, buf.m.offset);
		if (ctx->v4l2_bufs[i].start == MAP_FAILED) {
			ctx->v4l2_bufs[i].start = NULL;
			return -1;
		}
		if (xioctl(ctx->v4l2_fd, VIDIOC_QBUF, &buf) < 0)
			return -1;
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

static RK_S32 init_vdec(uint32_t width, uint32_t height)
{
	VDEC_PIC_BUF_ATTR_S pic_buf_attr;
	MB_PIC_CAL_S pic_buf_result;
	VDEC_CHN_ATTR_S chn_attr;
	VDEC_CHN_PARAM_S chn_param;

	memset(&pic_buf_attr, 0, sizeof(pic_buf_attr));
	pic_buf_attr.enCodecType = RK_VIDEO_ID_MJPEG;
	pic_buf_attr.stPicBufAttr.u32Width = width;
	pic_buf_attr.stPicBufAttr.u32Height = height;
	pic_buf_attr.stPicBufAttr.enPixelFormat = RK_FMT_YUV420SP;
	pic_buf_attr.stPicBufAttr.enCompMode = COMPRESS_MODE_NONE;

	RK_S32 ret = RK_MPI_CAL_VDEC_GetPicBufferSize(&pic_buf_attr, &pic_buf_result);
	if (ret != RK_SUCCESS)
		return ret;

	memset(&chn_attr, 0, sizeof(chn_attr));
	chn_attr.enMode = VIDEO_MODE_FRAME;
	chn_attr.enType = RK_VIDEO_ID_MJPEG;
	chn_attr.u32PicWidth = width;
	chn_attr.u32PicHeight = height;
	chn_attr.u32FrameBufCnt = 6;
	chn_attr.u32StreamBufCnt = 4;
	chn_attr.u32FrameBufSize = pic_buf_result.u32MBSize;

	ret = RK_MPI_VDEC_CreateChn(VDEC_CHN_ID, &chn_attr);
	if (ret != RK_SUCCESS)
		return ret;

	memset(&chn_param, 0, sizeof(chn_param));
	chn_param.enType = RK_VIDEO_ID_MJPEG;
	chn_param.stVdecPictureParam.enPixelFormat = RK_FMT_YUV420SP;
	ret = RK_MPI_VDEC_SetChnParam(VDEC_CHN_ID, &chn_param);
	if (ret != RK_SUCCESS) {
		RK_MPI_VDEC_DestroyChn(VDEC_CHN_ID);
		return ret;
	}

	ret = RK_MPI_VDEC_StartRecvStream(VDEC_CHN_ID);
	if (ret != RK_SUCCESS) {
		RK_MPI_VDEC_DestroyChn(VDEC_CHN_ID);
		return ret;
	}

	return RK_SUCCESS;
}

static RK_S32 init_venc(mpp_ctx_t *ctx)
{
	VENC_CHN_ATTR_S chn_attr;
	memset(&chn_attr, 0, sizeof(chn_attr));

	chn_attr.stVencAttr.enType = RK_VIDEO_ID_HEVC;
	chn_attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
	chn_attr.stVencAttr.u32Profile = 0;
	chn_attr.stVencAttr.u32PicWidth = ctx->actual_width;
	chn_attr.stVencAttr.u32PicHeight = ctx->actual_height;
	chn_attr.stVencAttr.u32VirWidth = ctx->actual_width;
	chn_attr.stVencAttr.u32VirHeight = ctx->actual_height;
	chn_attr.stVencAttr.u32StreamBufCnt = 4;
	chn_attr.stVencAttr.u32BufSize = ctx->actual_width * ctx->actual_height / 2;

	chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
	chn_attr.stRcAttr.stH265Cbr.u32Gop = (RK_U32)(ctx->src->gop > 0 ? ctx->src->gop : 5);
	chn_attr.stRcAttr.stH265Cbr.u32BitRate = (RK_U32)ctx->src->bitrate_kbps;
	chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen = 1;
	chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum = (RK_U32)ctx->src->fps;
	chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen = ctx->actual_fps_den > 0
		? ctx->actual_fps_den : 1;
	chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum = ctx->actual_fps_num > 0
		? ctx->actual_fps_num : (RK_U32)ctx->src->fps;

	RK_S32 ret = RK_MPI_VENC_CreateChn(VENC_CHN_ID, &chn_attr);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VENC_CreateChn failed %#X", ret);
		return ret;
	}

	/* Slice split disabled: RV1106 vepu540c triggers kernel panic
	 * (NULL deref in vepu540c_h265_set_hw_address) when slice split
	 * is enabled.  NACK retransmission handles reliability without it. */

	VENC_RECV_PIC_PARAM_S recv_param;
	memset(&recv_param, 0, sizeof(recv_param));
	recv_param.s32RecvPicNum = -1;
	ret = RK_MPI_VENC_StartRecvFrame(VENC_CHN_ID, &recv_param);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VENC_StartRecvFrame failed %#X", ret);
		RK_MPI_VENC_DestroyChn(VENC_CHN_ID);
		return ret;
	}

	memset(ctx->venc_packs, 0, sizeof(ctx->venc_packs));
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

static RK_S32 send_mjpeg_to_vdec_owned(mpp_ctx_t *ctx, void *packet, size_t len,
					uint8_t **retry_packet, size_t *retry_len)
{
	MB_EXT_CONFIG_S ext_config;
	VDEC_STREAM_S stream;
	MB_BLK mb_blk = RK_NULL;

	if (!packet || len == 0)
		return RK_FAILURE;

	memset(&ext_config, 0, sizeof(ext_config));
	ext_config.pu8VirAddr = packet;
	ext_config.u64Size = len;
	ext_config.pOpaque = packet;
	ext_config.pFreeCB = mjpeg_packet_free;

	RK_S32 ret = RK_MPI_SYS_CreateMB(&mb_blk, &ext_config);
	if (ret != RK_SUCCESS) {
		free(packet);
		return ret;
	}

	memset(&stream, 0, sizeof(stream));
	stream.pMbBlk = mb_blk;
	stream.u32Len = (RK_U32)len;
	stream.bEndOfStream = RK_FALSE;
	stream.bEndOfFrame = RK_TRUE;
	stream.bBypassMbBlk = RK_TRUE;

	uint64_t start_us = mono_us();
	ret = RK_MPI_VDEC_SendStream(VDEC_CHN_ID, &stream, 0);
	mpp_diag_note_vdec_submit_latency(ctx, mono_us() - start_us);
	if (ret == MPP_ERR_TIMEOUT && retry_packet != NULL && retry_len != NULL) {
		uint8_t *retry = (uint8_t *)malloc(len);
		if (retry != NULL) {
			memcpy(retry, packet, len);
			*retry_packet = retry;
			*retry_len = len;
		}
	}
	RK_MPI_MB_ReleaseMB(mb_blk);
	return ret;
}

/*
 * Emit one VENC stream to the output (ring or pipe).
 * The stream must already be held (GetStream succeeded).
 * Returns the number of bytes written.
 */
static uint64_t mpp_emit_venc_stream(mpp_ctx_t *ctx)
{
	RK_U32 npack = ctx->venc_stream.u32PackCount;
	if (npack == 0) npack = 1;
	if (npack > MAX_VENC_PACKS) npack = MAX_VENC_PACKS;

	uint64_t bytes_out = 0;
	for (RK_U32 p = 0; p < npack; p++) {
		void *nal_data = RK_MPI_MB_Handle2VirAddr(ctx->venc_packs[p].pMbBlk);
		RK_U32 nal_len = ctx->venc_packs[p].u32Len;
		if (!nal_data || nal_len == 0)
			continue;

		if (ctx->ring) {
			bool pushed = video_ring_push(ctx->ring, nal_data, nal_len);
			if (pushed) {
				/* 1-byte signal wakes the consumer without carrying data */
				uint8_t sig = 1;
				(void)write(ctx->pipe_wr, &sig, sizeof(sig));
			} else {
				mpp_diag_note_ring_drop(ctx);
			}
		} else {
			(void)write(ctx->pipe_wr, nal_data, nal_len);
		}
		bytes_out += nal_len;
	}

	mpp_diag_note_venc_stream(ctx, (uint32_t)npack, bytes_out);
	return bytes_out;
}

/*
 * Drain up to `max` encoded streams from VENC.
 * `first_timeout_ms`: how long to wait for the very first stream;
 * subsequent streams use timeout=0 (greedy, non-blocking).
 * Returns the number of streams drained.
 */
static int mpp_drain_venc(mpp_ctx_t *ctx, int max, int first_timeout_ms)
{
	int count = 0;
	int timeout = first_timeout_ms;

	for (int i = 0; i < max && !ctx->stop; i++, timeout = 0) {
		ctx->venc_stream.pstPack = ctx->venc_packs;
		uint64_t t0 = mono_us();
		RK_S32 ret = RK_MPI_VENC_GetStream(VENC_CHN_ID, &ctx->venc_stream, timeout);
		if (ret != RK_SUCCESS)
			break;
		mpp_diag_note_venc_getstream_latency(ctx, mono_us() - t0);
		mpp_emit_venc_stream(ctx);
		RK_MPI_VENC_ReleaseStream(VENC_CHN_ID, &ctx->venc_stream);
		count++;
	}
	return count;
}

/* YUV420SP (NV12) color bars: 8 vertical stripes */
static void fill_color_bars_nv12(uint8_t *y_plane, uint8_t *uv_plane,
				 uint32_t w, uint32_t h, int frame_num)
{
	/* RGB values for 8 bars: white,yellow,cyan,green,magenta,red,blue,black */
	static const uint8_t bars_r[] = {235,235, 54, 54,235,235, 54, 16};
	static const uint8_t bars_g[] = {235,235,235,235, 54, 54, 54, 16};
	static const uint8_t bars_b[] = {235, 54,235, 54,235, 54,235, 16};

	uint32_t bar_w = w / 8;

	for (uint32_t row = 0; row < h; row++) {
		for (uint32_t col = 0; col < w; col++) {
			int bar = (col / bar_w);
			if (bar > 7) bar = 7;
			uint8_t r = bars_r[bar], g = bars_g[bar], b = bars_b[bar];

			/* Moving marker: white horizontal line */
			uint32_t marker_y = (frame_num * 4) % h;
			if (row >= marker_y && row < marker_y + 4) {
				r = 235; g = 235; b = 235;
			}

			uint8_t y  = (uint8_t)(( 66*r + 129*g +  25*b + 128) / 256 + 16);
			y_plane[row * w + col] = y;

			if ((row & 1) == 0 && (col & 1) == 0) {
				uint8_t u = (uint8_t)((-38*r -  74*g + 112*b + 128) / 256 + 128);
				uint8_t v = (uint8_t)((112*r -  94*g -  18*b + 128) / 256 + 128);
				uint32_t uv_idx = (row/2) * w + col;
				uv_plane[uv_idx]     = u;
				uv_plane[uv_idx + 1] = v;
			}
		}
	}
}

static void *mpp_pattern_thread(void *arg)
{
	mpp_ctx_t *ctx = (mpp_ctx_t *)arg;
	uint32_t w = ctx->actual_width;
	uint32_t h = ctx->actual_height;
	uint32_t y_size = w * h;
	uint32_t uv_size = w * h / 2;
	int frame_num = 0;
	int fps = ctx->src->fps > 0 ? ctx->src->fps : 25;
	uint32_t interval_us = 1000000 / (uint32_t)fps;

	fprintf(stderr, "vcpd: test pattern %ux%u @ %d fps\n", w, h, fps);

	while (!ctx->stop) {
		MB_BLK mb_blk = RK_NULL;
		RK_S32 ret = RK_MPI_SYS_MmzAlloc(&mb_blk, NULL, NULL, y_size + uv_size);
		if (ret != RK_SUCCESS || !mb_blk) {
			if (frame_num == 0)
				fprintf(stderr, "vcpd: [mpp] MmzAlloc failed ret=%#X\n", ret);
			usleep(10000);
			continue;
		}

		uint8_t *vaddr = (uint8_t *)RK_MPI_MB_Handle2VirAddr(mb_blk);
		fill_color_bars_nv12(vaddr, vaddr + y_size, w, h, frame_num);

		VIDEO_FRAME_INFO_S frame_info;
		memset(&frame_info, 0, sizeof(frame_info));
		frame_info.stVFrame.u32Width = w;
		frame_info.stVFrame.u32Height = h;
		frame_info.stVFrame.u32VirWidth = w;
		frame_info.stVFrame.u32VirHeight = h;
		frame_info.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
		frame_info.stVFrame.pMbBlk = mb_blk;

		ret = RK_MPI_VENC_SendFrame(VENC_CHN_ID, &frame_info, 1000);
		if (ret != RK_SUCCESS) {
			if (frame_num == 0)
				fprintf(stderr, "vcpd: [mpp] VENC_SendFrame failed ret=%#X frame=%d\n", ret, frame_num);
			RK_MPI_MB_ReleaseMB(mb_blk);
			usleep(interval_us);
			frame_num++;
			continue;
		}
		mpp_diag_inc_venc_submit(ctx);

		ctx->venc_stream.pstPack = ctx->venc_packs;
		ret = RK_MPI_VENC_GetStream(VENC_CHN_ID, &ctx->venc_stream, 1000);
		if (ret == RK_SUCCESS) {
			mpp_emit_venc_stream(ctx);
			RK_MPI_VENC_ReleaseStream(VENC_CHN_ID, &ctx->venc_stream);
		} else if (frame_num == 0) {
			fprintf(stderr, "vcpd: [mpp] VENC_GetStream failed ret=%#X\n", ret);
		}

		RK_MPI_MB_ReleaseMB(mb_blk);
		frame_num++;
		if (frame_num == 1)
			fprintf(stderr, "vcpd: [mpp] first frame encoded OK\n");
		mpp_diag_maybe_report(ctx);
		usleep(interval_us);
	}

	if (ctx->pipe_wr >= 0) {
		close(ctx->pipe_wr);
		ctx->pipe_wr = -1;
	}
	return NULL;
}

/*
 * Camera pipeline worker.
 *
 * Runs in three phases per iteration to keep the hardware pipeline full:
 *
 *   Phase 1 — batch-feed: pop up to BATCH_FEED MJPEG packets from the
 *              software queue and submit them to VDEC in a tight loop.
 *              VDEC_SendStream uses timeout=0 because submission is fast;
 *              the hardware decodes asynchronously.
 *
 *   Phase 2 — drain VDEC → VENC: call VDEC_GetFrame with a blocking timeout
 *              on the first attempt (giving the hardware time to decode the
 *              batch we just submitted), then non-blocking for the rest.
 *              Each decoded frame is forwarded to VENC immediately.
 *
 *   Phase 3 — drain VENC: same blocking-first pattern; emit each encoded
 *              stream to the ring buffer and signal the consumer.
 *
 * This mirrors what mpp_pattern_thread does for the test path: submit one
 * frame, block waiting for encoded output.  Here we batch N frames to amortise
 * the per-frame scheduling overhead and keep both VDEC and VENC full.
 */
static void *mpp_process_thread(void *arg)
{
	mpp_ctx_t *ctx = (mpp_ctx_t *)arg;
	bool pipeline_active = false;

	while (!ctx->stop) {
		/* Phase 1: batch-submit MJPEG to VDEC */
		int fed = 0;
		for (int i = 0; i < BATCH_FEED && !ctx->stop; i++) {
			mjpeg_packet_t pkt = {0};
			if (!mpp_queue_pop(ctx, &pkt))
				break;
			uint8_t *retry = NULL;
			size_t retry_len = 0;
			RK_S32 ret = send_mjpeg_to_vdec_owned(ctx, pkt.data, pkt.len,
							       &retry, &retry_len);
			mpp_diag_note_vdec_submit(ctx, ret);
			if (ret == RK_SUCCESS) {
				fed++;
				pipeline_active = true;
			} else if (ret == MPP_ERR_TIMEOUT && retry != NULL) {
				(void)mpp_queue_push(ctx, retry, retry_len);
			}
		}

		/* Nothing to do: wait for new capture data */
		if (fed == 0 && !pipeline_active) {
			mpp_queue_wait(ctx, 20);
			continue;
		}

		/* Phase 2: drain VDEC output → VENC input
		 * Block on the first GetFrame so the hardware has time to decode;
		 * subsequent frames in the batch are typically already ready. */
		int decoded = 0;
		int vdec_timeout = (fed > 0) ? VDEC_DRAIN_TIMEOUT_MS : 0;
		int vdec_max = (fed > 0) ? (fed + 2) : 4;

		for (int i = 0; i < vdec_max && !ctx->stop; i++, vdec_timeout = 0) {
			VIDEO_FRAME_INFO_S frame;
			memset(&frame, 0, sizeof(frame));
			uint64_t t0 = mono_us();
			RK_S32 ret = RK_MPI_VDEC_GetFrame(VDEC_CHN_ID, &frame, vdec_timeout);
			mpp_diag_note_vdec_getframe_latency(ctx, mono_us() - t0);
			if (ret != RK_SUCCESS) {
				if (decoded == 0)
					pipeline_active = false;
				break;
			}
			mpp_diag_inc_vdec_frame(ctx);
			decoded++;

			uint64_t vs = mono_us();
			ret = RK_MPI_VENC_SendFrame(VENC_CHN_ID, &frame, VDEC_DRAIN_TIMEOUT_MS);
			mpp_diag_note_venc_submit_latency(ctx, mono_us() - vs);
			RK_MPI_VDEC_ReleaseFrame(VDEC_CHN_ID, &frame);
			if (ret == RK_SUCCESS)
				mpp_diag_inc_venc_submit(ctx);
		}

		/* Phase 3: drain VENC output → ring / pipe */
		int enc_timeout = (decoded > 0) ? VENC_DRAIN_TIMEOUT_MS : 0;
		int enc_max = (decoded > 0) ? (decoded + 2) : 4;
		int encoded = mpp_drain_venc(ctx, enc_max, enc_timeout);
		if (encoded == 0 && decoded == 0)
			pipeline_active = false;

		mpp_diag_maybe_report(ctx);
	}

	/* Flush residual frames left in the pipeline */
	for (int pass = 0; pass < 16; pass++) {
		int decoded = 0;
		for (int i = 0; i < 4; i++) {
			VIDEO_FRAME_INFO_S frame;
			memset(&frame, 0, sizeof(frame));
			if (RK_MPI_VDEC_GetFrame(VDEC_CHN_ID, &frame, 0) != RK_SUCCESS)
				break;
			decoded++;
			mpp_diag_inc_vdec_frame(ctx);
			if (RK_MPI_VENC_SendFrame(VENC_CHN_ID, &frame, 20) == RK_SUCCESS)
				mpp_diag_inc_venc_submit(ctx);
			RK_MPI_VDEC_ReleaseFrame(VDEC_CHN_ID, &frame);
		}
		int encoded = mpp_drain_venc(ctx, 4, 0);
		if (decoded == 0 && encoded == 0)
			break;
	}

	mpp_diag_maybe_report(ctx);
	if (ctx->pipe_wr >= 0) {
		close(ctx->pipe_wr);
		ctx->pipe_wr = -1;
	}
	return NULL;
}

static void *mpp_capture_thread(void *arg)
{
	mpp_ctx_t *ctx = (mpp_ctx_t *)arg;

	while (!ctx->stop) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;

		if (xioctl(ctx->v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {
			if (errno == EAGAIN)
				continue;
			fprintf(stderr, "vcpd: V4L2 capture failed: %s\n", strerror(errno));
			ctx->stop = true;
			mpp_queue_wake(ctx);
			break;
		}
		mpp_diag_inc_capture(ctx);

		if (buf.index >= V4L2_BUFFER_COUNT || !ctx->v4l2_bufs[buf.index].start) {
			xioctl(ctx->v4l2_fd, VIDIOC_QBUF, &buf);
			continue;
		}

		size_t packet_len = buf.bytesused;
		uint8_t *packet = NULL;
		if (packet_len > 0) {
			packet = (uint8_t *)malloc(packet_len);
			if (packet != NULL)
				memcpy(packet, ctx->v4l2_bufs[buf.index].start, packet_len);
			}

		xioctl(ctx->v4l2_fd, VIDIOC_QBUF, &buf);
		if (packet != NULL)
			(void)mpp_queue_push(ctx, packet, packet_len);
	}

	return NULL;
}

int video_source_mpp_start(video_source_mpp_t *src)
{
	if (!src || src->running)
		return -1;

	if (!src->device[0])
		strncpy(src->device, "/dev/video21", sizeof(src->device) - 1);
	if (src->bitrate_kbps <= 0)
		src->bitrate_kbps = 256;
	if (src->width <= 0) src->width = 480;
	if (src->height <= 0) src->height = 320;
	if (src->fps <= 0) src->fps = 15;

	mpp_ctx_t *ctx = calloc(1, sizeof(mpp_ctx_t));
	if (!ctx)
		return -1;
	ctx->src = src;
	ctx->v4l2_fd = -1;
	ctx->pipe_wr = -1;
	if (pthread_mutex_init(&ctx->diag_lock, NULL) != 0) {
		free(ctx);
		return -1;
	}
	ctx->diag_lock_init = true;
	if (mpp_queue_init(ctx) != 0) {
		pthread_mutex_destroy(&ctx->diag_lock);
		free(ctx);
		return -1;
	}

	RK_S32 ret = RK_MPI_SYS_Init();
	if (ret != RK_SUCCESS) {
		mpp_queue_destroy(ctx);
		pthread_mutex_destroy(&ctx->diag_lock);
		free(ctx);
		return -1;
	}

	int pipefd[2];
	if (pipe(pipefd) < 0) {
		RK_MPI_SYS_Exit();
		mpp_queue_destroy(ctx);
		pthread_mutex_destroy(&ctx->diag_lock);
		free(ctx);
		return -1;
	}
	src->pipe_fd = pipefd[0];
	ctx->pipe_wr = pipefd[1];

	if (src->test_pattern) {
		/* Test pattern mode: VENC only, no camera/VDEC */
		ctx->actual_width = (uint32_t)src->width;
		ctx->actual_height = (uint32_t)src->height;
		ctx->actual_fps_num = (uint32_t)src->fps;
		ctx->actual_fps_den = 1;
		ctx->actual_fps = (uint32_t)src->fps;

		ret = init_venc(ctx);
		if (ret != RK_SUCCESS) {
			close(pipefd[0]); close(pipefd[1]);
			goto fail;
		}

		ctx->stop = false;
		if (pthread_create(&ctx->capture_thread, NULL, mpp_pattern_thread, ctx) != 0) {
			deinit_venc();
			close(pipefd[0]); close(pipefd[1]);
			goto fail;
		}
		ctx->capture_thread_started = true;
	} else {
		/* Camera mode: V4L2 → VDEC → VENC → ring */
		if (v4l2_open(ctx) < 0)
			goto fail;

		fprintf(stderr,
			"vcpd: [mpp] capture req=%dx%d@%d fps, got=%ux%u @ %u/%u fps (~%u)\n",
			src->width, src->height, src->fps,
			ctx->actual_width, ctx->actual_height,
			ctx->actual_fps_num > 0 ? ctx->actual_fps_num : (uint32_t)src->fps,
			ctx->actual_fps_den > 0 ? ctx->actual_fps_den : 1,
			ctx->actual_fps);

		ret = init_vdec(ctx->actual_width, ctx->actual_height);
		if (ret != RK_SUCCESS)
			goto fail;

		ret = init_venc(ctx);
		if (ret != RK_SUCCESS) {
			deinit_vdec();
			goto fail;
		}

		fprintf(stderr,
			"vcpd: [mpp] venc rc src_fps=%u/%u dst_fps=%d/1 bitrate=%d gop=%d\n",
			ctx->actual_fps_num > 0 ? ctx->actual_fps_num : (uint32_t)src->fps,
			ctx->actual_fps_den > 0 ? ctx->actual_fps_den : 1,
			src->fps, src->bitrate_kbps, src->gop > 0 ? src->gop : 5);

		/* Ring buffer: encoded frames travel here instead of through the pipe.
		 * The pipe still carries 1-byte wake signals and its EOF triggers
		 * consumer shutdown exactly as before. */
		ctx->ring = video_ring_create(VIDEO_RING_FILE);
		if (!ctx->ring) {
			fprintf(stderr, "vcpd: [mpp] failed to create ring buffer at %s: %s\n",
				VIDEO_RING_FILE, strerror(errno));
			deinit_venc();
			deinit_vdec();
			goto fail;
		}
		fprintf(stderr, "vcpd: [mpp] ring buffer %zu KB at %s\n",
			VIDEO_RING_MMAP_SIZE / 1024, VIDEO_RING_FILE);

		if (v4l2_stream_on(ctx) < 0) {
			video_ring_destroy(ctx->ring, VIDEO_RING_FILE);
			ctx->ring = NULL;
			close(pipefd[0]); close(pipefd[1]);
			deinit_venc(); deinit_vdec();
			goto fail;
		}

		ctx->stop = false;
		if (pthread_create(&ctx->process_thread, NULL, mpp_process_thread, ctx) != 0) {
			v4l2_cleanup(ctx);
			video_ring_destroy(ctx->ring, VIDEO_RING_FILE);
			ctx->ring = NULL;
			close(pipefd[0]); close(pipefd[1]);
			deinit_venc(); deinit_vdec();
			goto fail;
		}
		ctx->process_thread_started = true;
		if (pthread_create(&ctx->capture_thread, NULL, mpp_capture_thread, ctx) != 0) {
			ctx->stop = true;
			mpp_queue_wake(ctx);
			pthread_join(ctx->process_thread, NULL);
			ctx->process_thread_started = false;
			v4l2_cleanup(ctx);
			video_ring_destroy(ctx->ring, VIDEO_RING_FILE);
			ctx->ring = NULL;
			close(pipefd[0]); close(pipefd[1]);
			deinit_venc(); deinit_vdec();
			goto fail;
		}
		ctx->capture_thread_started = true;
	}

	mpp_diag_reset(ctx, mono_ms());
	src->mpp_ctx = ctx;
	src->running = true;
	return 0;

fail:
	ctx->stop = true;
	mpp_queue_wake(ctx);
	if (ctx->capture_thread_started)
		pthread_join(ctx->capture_thread, NULL);
	if (ctx->process_thread_started)
		pthread_join(ctx->process_thread, NULL);
	v4l2_cleanup(ctx);
	RK_MPI_SYS_Exit();
	mpp_queue_destroy(ctx);
	if (ctx->diag_lock_init)
		pthread_mutex_destroy(&ctx->diag_lock);
	free(ctx);
	return -1;
}

void video_source_mpp_stop(video_source_mpp_t *src)
{
	if (!src || !src->running || !src->mpp_ctx)
		return;

	mpp_ctx_t *ctx = (mpp_ctx_t *)src->mpp_ctx;
	ctx->stop = true;
	mpp_queue_wake(ctx);
	if (ctx->capture_thread_started)
		pthread_join(ctx->capture_thread, NULL);
	if (ctx->process_thread_started)
		pthread_join(ctx->process_thread, NULL);

	v4l2_cleanup(ctx);
	deinit_venc();
	deinit_vdec();

	/* pipe_wr is closed by the worker thread on exit; guard against double-close */
	if (ctx->pipe_wr >= 0) {
		close(ctx->pipe_wr);
		ctx->pipe_wr = -1;
	}
	if (src->pipe_fd >= 0) {
		close(src->pipe_fd);
		src->pipe_fd = -1;
	}

	if (ctx->ring) {
		video_ring_destroy(ctx->ring, VIDEO_RING_FILE);
		ctx->ring = NULL;
	}

	RK_MPI_SYS_Exit();
	mpp_queue_destroy(ctx);
	if (ctx->diag_lock_init)
		pthread_mutex_destroy(&ctx->diag_lock);
	free(ctx);
	src->mpp_ctx = NULL;
	src->running = false;
}

ssize_t video_source_mpp_read(video_source_mpp_t *src, uint8_t *buf, size_t len)
{
	if (!src || !src->running || src->pipe_fd < 0)
		return -1;

	mpp_ctx_t *ctx = (mpp_ctx_t *)src->mpp_ctx;

	if (ctx && ctx->ring) {
		/*
		 * In ring mode the pipe carries only 1-byte wake signals.
		 * Read the signal byte first; if the pipe is at EOF the
		 * producer has exited, so return 0 to trigger shutdown.
		 */
		uint8_t sig;
		ssize_t r = read(src->pipe_fd, &sig, sizeof(sig));
		if (r == 0)
			return 0;  /* EOF — producer closed pipe_wr */
		if (r < 0) {
			if (errno == EINTR || errno == EAGAIN)
				return 0;
			return -1;
		}

		/* Pop one encoded NAL chunk from the ring */
		uint32_t chunk_len = 0;
		if (!video_ring_pop(ctx->ring, buf, len, &chunk_len))
			return 0;  /* spurious signal */
		return (ssize_t)chunk_len;
	}

	/* Legacy / test-pattern path: raw H.265 bitstream in the pipe */
	ssize_t n = read(src->pipe_fd, buf, len);
	if (n < 0 && errno == EINTR)
		return 0;
	return n;
}

#endif /* VCPD_WITH_MPP */
