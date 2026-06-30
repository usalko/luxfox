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
#include "rockchip/mpp_err.h"
#include "rk_mpi_cal.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vdec.h"
#include "rk_mpi_venc.h"

#define V4L2_BUFFER_COUNT  4
#define VDEC_CHN_ID        0
#define VENC_CHN_ID        0

/* Event-loop poll interval (ms) — the "yield" between non-blocking drain passes.
 * At 15fps V4L2 delivers a frame every ~66ms; 5ms gives ~13 drain attempts per frame. */
#define ENCODE_POLL_MS    5

typedef struct {
	void  *start;
	size_t length;
} mpp_v4l2_buffer_t;

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
	uint32_t ring_drop;
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

	bool               diag_lock_init;
	pthread_mutex_t    diag_lock;
	mpp_diag_t         diag;

	/* Ring buffer used in camera mode; NULL in test-pattern mode. */
	video_ring_t      *ring;
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
	if (ret == RK_SUCCESS)         ctx->diag.vdec_submit_ok++;
	else if (ret == MPP_ERR_TIMEOUT) ctx->diag.vdec_submit_busy++;
	else                             ctx->diag.vdec_submit_drop++;
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
		"%s vcpd: [mpp] cap=%u dec=%u enc=%u nals=%u bytes=%llu rdrop=%u | "
		"fps cap=%u.%u dec=%u.%u enc=%u.%u | "
		"lat_us vdec_in=%u/%u vdec_out=%u/%u venc_in=%u/%u venc_out=%u/%u | "
		"negotiated=%u/%u fps\n",
		ts,
		s.v4l2_dq_ok, s.vdec_frame_ok, s.venc_stream_ok,
		s.nal_writes, (unsigned long long)s.nal_bytes, s.ring_drop,
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

	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.timeperframe.numerator   = 1;
	parm.parm.capture.timeperframe.denominator = (unsigned)ctx->src->fps;
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

/* ------------------------------------------------------------------ MPP helpers */

static RK_S32 mjpeg_packet_free(void *opaque) { free(opaque); return RK_SUCCESS; }

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
	chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen       = 1;
	chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum       = (RK_U32)ctx->src->fps;
	chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen        =
		ctx->actual_fps_den > 0 ? ctx->actual_fps_den : 1;
	chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum        =
		ctx->actual_fps_num > 0 ? ctx->actual_fps_num : (RK_U32)ctx->src->fps;

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

/*
 * Submit one MJPEG packet to VDEC.  Ownership of `packet` (heap-allocated)
 * is transferred to the MPI layer via pFreeCB; it will be freed when VDEC
 * is done with it.  Returns RK_SUCCESS or an error code.
 */
static RK_S32 send_mjpeg_to_vdec_owned(mpp_ctx_t *ctx, void *packet, size_t len)
{
	MB_EXT_CONFIG_S ext_config;
	VDEC_STREAM_S   stream;
	MB_BLK          mb_blk = RK_NULL;

	if (!packet || len == 0) return RK_FAILURE;

	memset(&ext_config, 0, sizeof(ext_config));
	ext_config.pu8VirAddr = packet;
	ext_config.u64Size    = len;
	ext_config.pOpaque    = packet;
	ext_config.pFreeCB    = mjpeg_packet_free;

	RK_S32 ret = RK_MPI_SYS_CreateMB(&mb_blk, &ext_config);
	if (ret != RK_SUCCESS) { free(packet); return ret; }

	memset(&stream, 0, sizeof(stream));
	stream.pMbBlk        = mb_blk;
	stream.u32Len        = (RK_U32)len;
	stream.bEndOfStream  = RK_FALSE;
	stream.bEndOfFrame   = RK_TRUE;
	stream.bBypassMbBlk  = RK_TRUE;

	uint64_t t0 = mono_us();
	ret = RK_MPI_VDEC_SendStream(VDEC_CHN_ID, &stream, 0);
	mpp_diag_note_vdec_submit_latency(ctx, mono_us() - t0);
	RK_MPI_MB_ReleaseMB(mb_blk);  /* frees packet via pFreeCB if not taken by VDEC */
	return ret;
}

/*
 * Emit the current VENC stream to the output channel.
 *
 * Camera mode: all packs are concatenated into ONE ring slot so that
 * video_ring_pop_latest() always returns a complete frame (VPS+SPS+PPS+IDR
 * or P-frame group) and never a partial one.
 *
 * Test-pattern mode: packs are written directly into the pipe as raw bytes.
 */
static uint64_t mpp_emit_venc_stream(mpp_ctx_t *ctx)
{
	RK_U32 npack = ctx->venc_stream.u32PackCount;
	if (npack == 0) npack = 1;
	if (npack > MAX_VENC_PACKS) npack = MAX_VENC_PACKS;

	uint64_t bytes_out = 0;

	if (ctx->ring) {
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
	} else {
		/* Test-pattern: raw byte stream into the pipe. */
		for (RK_U32 p = 0; p < npack; p++) {
			void   *data = RK_MPI_MB_Handle2VirAddr(ctx->venc_packs[p].pMbBlk);
			RK_U32  dlen = ctx->venc_packs[p].u32Len;
			if (!data || dlen == 0) continue;
			(void)write(ctx->pipe_wr, data, dlen);
			bytes_out += dlen;
		}
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
 *   task_capture        — select { case f := <-v4l2_ch: vdec_ch <- f }
 *   task_decode         — for f := range tryRecv(vdec_ch) { venc_ch <- f }
 *   task_encode         — for s := range tryRecv(venc_ch) { ring_ch <- s }
 *   task_pattern_submit — venc_ch <- generateColorBars(n)
 *
 * Threads call these tasks in a tight event loop — no blocking, no goto. */

/* Poll V4L2 (ENCODE_POLL_MS timeout = event-loop yield).
 * If a frame is ready: DQBUF, copy, re-queue, submit MJPEG to VDEC. */
static void task_capture(mpp_ctx_t *ctx, struct pollfd *pfd)
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

	size_t   len = buf.bytesused;
	uint8_t *pkt = len > 0 ? (uint8_t *)malloc(len) : NULL;
	if (pkt) memcpy(pkt, ctx->v4l2_bufs[buf.index].start, len);
	xioctl(ctx->v4l2_fd, VIDIOC_QBUF, &buf);  /* re-queue before encode */

	if (pkt && len > 0) {
		RK_S32 ret = send_mjpeg_to_vdec_owned(ctx, pkt, len);
		mpp_diag_note_vdec_submit(ctx, ret);
		/* pkt freed by MPI callback */
	} else {
		free(pkt);
	}
}

/* Drain all decoded NV12 frames from VDEC and submit each to VENC.
 * Both calls are non-blocking (timeout=0): returns immediately if nothing ready. */
static void task_decode(mpp_ctx_t *ctx)
{
	for (;;) {
		VIDEO_FRAME_INFO_S frame;
		memset(&frame, 0, sizeof(frame));
		uint64_t t0 = mono_us();
		RK_S32 ret = RK_MPI_VDEC_GetFrame(VDEC_CHN_ID, &frame, 0);
		mpp_diag_note_vdec_getframe_latency(ctx, mono_us() - t0);
		if (ret != RK_SUCCESS) break;
		mpp_diag_inc_vdec_frame(ctx);

		uint64_t vs = mono_us();
		RK_S32 sr = RK_MPI_VENC_SendFrame(VENC_CHN_ID, &frame, 0);
		mpp_diag_note_venc_submit_latency(ctx, mono_us() - vs);
		RK_MPI_VDEC_ReleaseFrame(VDEC_CHN_ID, &frame);
		if (sr == RK_SUCCESS) mpp_diag_inc_venc_submit(ctx);
	}
}

/* Drain all encoded H.265 streams from VENC and push to ring or pipe.
 * Non-blocking (timeout=0).  Shared by camera and pattern threads. */
static void task_encode(mpp_ctx_t *ctx, int *frame_num)
{
	for (;;) {
		ctx->venc_stream.pstPack = ctx->venc_packs;
		uint64_t t0 = mono_us();
		if (RK_MPI_VENC_GetStream(VENC_CHN_ID, &ctx->venc_stream, 0) != RK_SUCCESS)
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

/* ------------------------------------------------------------------ camera encode thread */

static void *mpp_encode_thread(void *arg)
{
	mpp_ctx_t *ctx = (mpp_ctx_t *)arg;
	struct pollfd pfd = { .fd = ctx->v4l2_fd, .events = POLLIN };
	int frame_num = 0;

	while (!ctx->stop) {
		task_capture(ctx, &pfd);          /* select: V4L2 → VDEC   */
		task_decode(ctx);                  /* drain: VDEC → VENC    */
		task_encode(ctx, &frame_num);      /* drain: VENC → ring    */
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

		if (pthread_create(&ctx->encode_thread, NULL, mpp_pattern_thread, ctx) != 0) {
			deinit_venc();
			close(pipefd[0]); close(pipefd[1]);
			goto fail;
		}
		ctx->encode_thread_started = true;

	} else {
		/* ---- camera mode: V4L2 → VDEC → VENC → ring ---- */
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

		if (pthread_create(&ctx->encode_thread, NULL, mpp_encode_thread, ctx) != 0) {
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
	if (ctx->encode_thread_started) pthread_join(ctx->encode_thread, NULL);

	v4l2_cleanup(ctx);
	deinit_venc();
	if (ctx->ring) deinit_vdec();

	if (ctx->pipe_wr >= 0) { close(ctx->pipe_wr); ctx->pipe_wr = -1; }
	if (src->pipe_fd >= 0) { close(src->pipe_fd); src->pipe_fd = -1; }

	if (ctx->ring) { video_ring_destroy(ctx->ring, VIDEO_RING_FILE); ctx->ring = NULL; }

	RK_MPI_SYS_Exit();
	if (ctx->diag_lock_init) pthread_mutex_destroy(&ctx->diag_lock);
	free(ctx);
	src->mpp_ctx = NULL;
	src->running = false;
}

ssize_t video_source_mpp_read(video_source_mpp_t *src, uint8_t *buf, size_t len)
{
	if (!src || !src->running || src->pipe_fd < 0) return -1;

	mpp_ctx_t *ctx = (mpp_ctx_t *)src->mpp_ctx;

	if (ctx && ctx->ring) {
		/*
		 * Camera mode: pipe carries 1-byte wake signals only.
		 * EOF on pipe means the encoder thread exited → shutdown.
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

	/* Test-pattern: raw H.265 byte stream in the pipe. */
	ssize_t n = read(src->pipe_fd, buf, len);
	if (n < 0 && errno == EINTR) return 0;
	return n;
}

#endif /* VCPD_WITH_MPP */
