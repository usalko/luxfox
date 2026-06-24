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

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "rk_debug.h"
#include "rk_mpi_cal.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vdec.h"
#include "rk_mpi_venc.h"

#define V4L2_BUFFER_COUNT 4
#define VDEC_CHN_ID 0
#define VENC_CHN_ID 0

typedef struct {
	void *start;
	size_t length;
} mpp_v4l2_buffer_t;

typedef struct {
	video_source_mpp_t *src;

	int v4l2_fd;
	mpp_v4l2_buffer_t v4l2_bufs[V4L2_BUFFER_COUNT];
	uint32_t actual_width;
	uint32_t actual_height;

	VENC_STREAM_S venc_stream;
	VENC_PACK_S venc_pack;

	int pipe_wr;
	pthread_t thread;
	volatile bool stop;
} mpp_ctx_t;

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
	xioctl(ctx->v4l2_fd, VIDIOC_S_PARM, &parm);

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
	chn_attr.stRcAttr.stH265Cbr.u32Gop = 5;
	chn_attr.stRcAttr.stH265Cbr.u32BitRate = (RK_U32)ctx->src->bitrate_kbps;
	chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen = 1;
	chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum = (RK_U32)ctx->src->fps;
	chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen = 1;
	chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum = (RK_U32)ctx->src->fps;

	RK_S32 ret = RK_MPI_VENC_CreateChn(VENC_CHN_ID, &chn_attr);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VENC_CreateChn failed %#X", ret);
		return ret;
	}

	/* H.265 slice split: 4 slices per frame.
	 * Each slice = separate NAL unit = smaller LTS group.
	 * Loss of one slice causes a localized artifact, not a full frame drop.
	 * CTU size is 64x64 for H.265. For height H: CTU_rows = ceil(H/64).
	 * u32SplitSize = ceil(CTU_rows/4) gives ~4 slices. */
	{
		uint32_t ctu_rows = (ctx->actual_height + 63) / 64;
		uint32_t rows_per_slice = (ctu_rows + 3) / 4;
		if (rows_per_slice < 1) rows_per_slice = 1;

		VENC_SLICE_SPLIT_S split;
		memset(&split, 0, sizeof(split));
		split.bSplitEnable = RK_TRUE;
		split.u32SplitMode = 1;
		split.u32SplitSize = rows_per_slice;

		ret = RK_MPI_VENC_SetSliceSplit(VENC_CHN_ID, &split);
		if (ret != RK_SUCCESS)
			RK_LOGE("RK_MPI_VENC_SetSliceSplit failed %#X (non-fatal)", ret);
		else
			fprintf(stderr, "vcpd: H.265 slice split enabled: %u CTU rows/slice (~%u slices)\n",
				rows_per_slice, (ctu_rows + rows_per_slice - 1) / rows_per_slice);
	}

	VENC_RECV_PIC_PARAM_S recv_param;
	memset(&recv_param, 0, sizeof(recv_param));
	recv_param.s32RecvPicNum = -1;
	ret = RK_MPI_VENC_StartRecvFrame(VENC_CHN_ID, &recv_param);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VENC_StartRecvFrame failed %#X", ret);
		RK_MPI_VENC_DestroyChn(VENC_CHN_ID);
		return ret;
	}

	memset(&ctx->venc_pack, 0, sizeof(ctx->venc_pack));
	memset(&ctx->venc_stream, 0, sizeof(ctx->venc_stream));
	ctx->venc_stream.pstPack = &ctx->venc_pack;

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

static RK_S32 send_mjpeg_to_vdec(const void *data, size_t len)
{
	MB_EXT_CONFIG_S ext_config;
	VDEC_STREAM_S stream;
	MB_BLK mb_blk = RK_NULL;

	RK_U8 *packet = malloc(len);
	if (!packet)
		return RK_FAILURE;
	memcpy(packet, data, len);

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

	ret = RK_MPI_VDEC_SendStream(VDEC_CHN_ID, &stream, 1000);
	RK_MPI_MB_ReleaseMB(mb_blk);
	return ret;
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
		if (ret == RK_SUCCESS) {
			ctx->venc_stream.pstPack = &ctx->venc_pack;
			ret = RK_MPI_VENC_GetStream(VENC_CHN_ID, &ctx->venc_stream, 1000);
			if (ret == RK_SUCCESS) {
				void *nal_data = RK_MPI_MB_Handle2VirAddr(ctx->venc_pack.pMbBlk);
				RK_U32 nal_len = ctx->venc_pack.u32Len;
				if (nal_data && nal_len > 0)
					write(ctx->pipe_wr, nal_data, nal_len);
				RK_MPI_VENC_ReleaseStream(VENC_CHN_ID, &ctx->venc_stream);
			}
		}

		RK_MPI_MB_ReleaseMB(mb_blk);
		frame_num++;
		usleep(interval_us);
	}

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
			break;
		}

		if (buf.index >= V4L2_BUFFER_COUNT || !ctx->v4l2_bufs[buf.index].start) {
			xioctl(ctx->v4l2_fd, VIDIOC_QBUF, &buf);
			continue;
		}

		RK_S32 ret = send_mjpeg_to_vdec(ctx->v4l2_bufs[buf.index].start, buf.bytesused);
		if (ret == RK_SUCCESS) {
			VIDEO_FRAME_INFO_S decoded_frame;
			memset(&decoded_frame, 0, sizeof(decoded_frame));

			ret = RK_MPI_VDEC_GetFrame(VDEC_CHN_ID, &decoded_frame, 1000);
			if (ret == RK_SUCCESS) {
				ret = RK_MPI_VENC_SendFrame(VENC_CHN_ID, &decoded_frame, 1000);
				RK_MPI_VDEC_ReleaseFrame(VDEC_CHN_ID, &decoded_frame);

				if (ret == RK_SUCCESS) {
					ctx->venc_stream.pstPack = &ctx->venc_pack;
					ret = RK_MPI_VENC_GetStream(VENC_CHN_ID, &ctx->venc_stream, 1000);
					if (ret == RK_SUCCESS) {
						void *nal_data = RK_MPI_MB_Handle2VirAddr(ctx->venc_pack.pMbBlk);
						RK_U32 nal_len = ctx->venc_pack.u32Len;
						if (nal_data && nal_len > 0)
							write(ctx->pipe_wr, nal_data, nal_len);
						RK_MPI_VENC_ReleaseStream(VENC_CHN_ID, &ctx->venc_stream);
					}
				}
			}
		}

		xioctl(ctx->v4l2_fd, VIDIOC_QBUF, &buf);
	}

	/* Close pipe write end so main loop sees EOF and triggers recovery */
	if (ctx->pipe_wr >= 0) {
		close(ctx->pipe_wr);
		ctx->pipe_wr = -1;
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
		src->bitrate_kbps = 512;
	if (src->width <= 0) src->width = 640;
	if (src->height <= 0) src->height = 480;
	if (src->fps <= 0) src->fps = 25;

	mpp_ctx_t *ctx = calloc(1, sizeof(mpp_ctx_t));
	if (!ctx)
		return -1;
	ctx->src = src;
	ctx->v4l2_fd = -1;

	RK_S32 ret = RK_MPI_SYS_Init();
	if (ret != RK_SUCCESS) {
		free(ctx);
		return -1;
	}

	int pipefd[2];
	if (pipe(pipefd) < 0) {
		RK_MPI_SYS_Exit();
		free(ctx);
		return -1;
	}
	src->pipe_fd = pipefd[0];
	ctx->pipe_wr = pipefd[1];

	if (src->test_pattern) {
		/* Test pattern mode: VENC only, no camera/VDEC */
		ctx->actual_width = (uint32_t)src->width;
		ctx->actual_height = (uint32_t)src->height;

		ret = init_venc(ctx);
		if (ret != RK_SUCCESS) {
			close(pipefd[0]); close(pipefd[1]);
			goto fail;
		}

		ctx->stop = false;
		if (pthread_create(&ctx->thread, NULL, mpp_pattern_thread, ctx) != 0) {
			deinit_venc();
			close(pipefd[0]); close(pipefd[1]);
			goto fail;
		}
	} else {
		/* Camera mode: V4L2 → VDEC → VENC */
		if (v4l2_open(ctx) < 0)
			goto fail;

		ret = init_vdec(ctx->actual_width, ctx->actual_height);
		if (ret != RK_SUCCESS)
			goto fail;

		ret = init_venc(ctx);
		if (ret != RK_SUCCESS) {
			deinit_vdec();
			goto fail;
		}

		if (v4l2_stream_on(ctx) < 0) {
			close(pipefd[0]); close(pipefd[1]);
			deinit_venc(); deinit_vdec();
			goto fail;
		}

		ctx->stop = false;
		if (pthread_create(&ctx->thread, NULL, mpp_capture_thread, ctx) != 0) {
			v4l2_cleanup(ctx);
			close(pipefd[0]); close(pipefd[1]);
			deinit_venc(); deinit_vdec();
			goto fail;
		}
	}

	src->mpp_ctx = ctx;
	src->running = true;
	return 0;

fail:
	v4l2_cleanup(ctx);
	RK_MPI_SYS_Exit();
	free(ctx);
	return -1;
}

void video_source_mpp_stop(video_source_mpp_t *src)
{
	if (!src || !src->running || !src->mpp_ctx)
		return;

	mpp_ctx_t *ctx = (mpp_ctx_t *)src->mpp_ctx;
	ctx->stop = true;
	pthread_join(ctx->thread, NULL);

	v4l2_cleanup(ctx);
	deinit_venc();
	deinit_vdec();

	if (ctx->pipe_wr >= 0) {
		close(ctx->pipe_wr);
		ctx->pipe_wr = -1;
	}
	if (src->pipe_fd >= 0) {
		close(src->pipe_fd);
		src->pipe_fd = -1;
	}

	RK_MPI_SYS_Exit();
	free(ctx);
	src->mpp_ctx = NULL;
	src->running = false;
}

ssize_t video_source_mpp_read(video_source_mpp_t *src, uint8_t *buf, size_t len)
{
	if (!src || !src->running || src->pipe_fd < 0)
		return -1;

	ssize_t n = read(src->pipe_fd, buf, len);
	if (n < 0 && errno == EINTR)
		return 0;
	return n;
}

#endif /* VCPD_WITH_MPP */
