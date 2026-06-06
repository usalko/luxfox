/*
 * Copyright 2024 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* End of #ifdef __cplusplus */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "sample_comm.h"

#define V4L2_BUFFER_COUNT 4
#define VDEC_CHN_ID 0
#define VENC_CHN_ID 0

typedef struct rk_v4l2_buffer_s {
	void *start;
	size_t length;
} V4L2_BUFFER_S;

typedef struct rk_demo_context_s {
	const char *device_name;
	const char *output_path;
	RK_U32 width;
	RK_U32 height;
	RK_U32 fps;
	RK_U32 bitrate_kbps;
	RK_S32 frame_count;
	int v4l2_fd;
	V4L2_BUFFER_S buffers[V4L2_BUFFER_COUNT];
	RK_S32 queued_buffers;
	SAMPLE_VENC_CTX_S venc;
	FILE *output_fp;
} DEMO_CTX_S;

static volatile bool quit = false;

static void sigterm_handler(int sig) {
	fprintf(stderr, "signal %d\n", sig);
	quit = true;
}

static int xioctl(int fd, unsigned long request, void *arg) {
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret == -1 && errno == EINTR);

	return ret;
}

static void print_usage(const char *name) {
	printf("usage:\n");
	printf("  %s -d /dev/video21 -o /tmp/easycap.h265\n", name);
	printf("options:\n");
	printf("  -d | --device       v4l2 capture node, default: /dev/video21\n");
	printf("  -o | --output       output h265 file, default: /tmp/easycap.h265\n");
	printf("  -w | --width        capture width, default: 640\n");
	printf("  -h | --height       capture height, default: 480\n");
	printf("  -f | --fps          target fps, default: 25\n");
	printf("  -b | --bitrate      h265 bitrate in kbps, default: 512\n");
	printf("  -n | --count        frames to process, default: 200\n");
	printf("  -? | --help         show help\n");
}

static RK_S32 mjpeg_packet_free(void *opaque) {
	if (opaque != RK_NULL) {
		free(opaque);
	}
	return RK_SUCCESS;
}

static RK_S32 init_vdec(RK_U32 width, RK_U32 height) {
	VDEC_PIC_BUF_ATTR_S pic_buf_attr;
	MB_PIC_CAL_S pic_buf_result;
	VDEC_CHN_ATTR_S chn_attr;
	VDEC_CHN_PARAM_S chn_param;
	RK_S32 ret;

	memset(&pic_buf_attr, 0, sizeof(pic_buf_attr));
	pic_buf_attr.enCodecType = RK_VIDEO_ID_MJPEG;
	pic_buf_attr.stPicBufAttr.u32Width = width;
	pic_buf_attr.stPicBufAttr.u32Height = height;
	pic_buf_attr.stPicBufAttr.enPixelFormat = RK_FMT_YUV420SP;
	pic_buf_attr.stPicBufAttr.enCompMode = COMPRESS_MODE_NONE;

	ret = RK_MPI_CAL_VDEC_GetPicBufferSize(&pic_buf_attr, &pic_buf_result);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_CAL_VDEC_GetPicBufferSize failed %#X", ret);
		return ret;
	}

	memset(&chn_attr, 0, sizeof(chn_attr));
	chn_attr.enMode = VIDEO_MODE_FRAME;
	chn_attr.enType = RK_VIDEO_ID_MJPEG;
	chn_attr.u32PicWidth = width;
	chn_attr.u32PicHeight = height;
	chn_attr.u32FrameBufCnt = 6;
	chn_attr.u32StreamBufCnt = 4;
	chn_attr.u32FrameBufSize = pic_buf_result.u32MBSize;

	ret = RK_MPI_VDEC_CreateChn(VDEC_CHN_ID, &chn_attr);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VDEC_CreateChn failed %#X", ret);
		return ret;
	}

	memset(&chn_param, 0, sizeof(chn_param));
	chn_param.enType = RK_VIDEO_ID_MJPEG;
	chn_param.stVdecPictureParam.enPixelFormat = RK_FMT_YUV420SP;
	ret = RK_MPI_VDEC_SetChnParam(VDEC_CHN_ID, &chn_param);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VDEC_SetChnParam failed %#X", ret);
		RK_MPI_VDEC_DestroyChn(VDEC_CHN_ID);
		return ret;
	}

	ret = RK_MPI_VDEC_StartRecvStream(VDEC_CHN_ID);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VDEC_StartRecvStream failed %#X", ret);
		RK_MPI_VDEC_DestroyChn(VDEC_CHN_ID);
		return ret;
	}

	return RK_SUCCESS;
}

static void deinit_vdec(void) {
	RK_MPI_VDEC_StopRecvStream(VDEC_CHN_ID);
	RK_MPI_VDEC_DestroyChn(VDEC_CHN_ID);
}

static RK_S32 init_venc(DEMO_CTX_S *ctx) {
	memset(&ctx->venc, 0, sizeof(ctx->venc));
	ctx->venc.s32ChnId = VENC_CHN_ID;
	ctx->venc.u32Width = ctx->width;
	ctx->venc.u32Height = ctx->height;
	ctx->venc.u32Fps = ctx->fps;
	ctx->venc.u32Gop = ctx->fps;
	ctx->venc.u32BitRate = ctx->bitrate_kbps;
	ctx->venc.enCodecType = RK_CODEC_TYPE_H265;
	ctx->venc.enRcMode = VENC_RC_MODE_H265CBR;
	ctx->venc.enPixelFormat = RK_FMT_YUV420SP;
	ctx->venc.stChnAttr.stVencAttr.u32Profile = 0;

	RK_S32 ret = SAMPLE_COMM_VENC_CreateChn(&ctx->venc);
	if (ret != RK_SUCCESS) {
		return ret;
	}

	ctx->venc.stFrame.pstPack = calloc(1, sizeof(VENC_PACK_S));
	if (ctx->venc.stFrame.pstPack == RK_NULL) {
		SAMPLE_COMM_VENC_DestroyChn(&ctx->venc);
		return RK_FAILURE;
	}

	return RK_SUCCESS;
}

static void deinit_venc(DEMO_CTX_S *ctx) {
	SAMPLE_COMM_VENC_DestroyChn(&ctx->venc);
}

static RK_S32 open_output(DEMO_CTX_S *ctx) {
	ctx->output_fp = fopen(ctx->output_path, "wb");
	if (ctx->output_fp == RK_NULL) {
		RK_LOGE("open %s failed", ctx->output_path);
		return RK_FAILURE;
	}
	return RK_SUCCESS;
}

static void close_output(DEMO_CTX_S *ctx) {
	if (ctx->output_fp != RK_NULL) {
		fclose(ctx->output_fp);
		ctx->output_fp = RK_NULL;
	}
}

static RK_S32 v4l2_open_device(DEMO_CTX_S *ctx) {
	struct v4l2_capability cap;
	struct v4l2_format fmt;
	struct v4l2_streamparm parm;
	struct v4l2_requestbuffers req;
	RK_S32 i;

	ctx->v4l2_fd = open(ctx->device_name, O_RDWR, 0);
	if (ctx->v4l2_fd < 0) {
		RK_LOGE("open %s failed: %s", ctx->device_name, strerror(errno));
		return RK_FAILURE;
	}

	if (xioctl(ctx->v4l2_fd, VIDIOC_QUERYCAP, &cap) < 0) {
		RK_LOGE("VIDIOC_QUERYCAP failed: %s", strerror(errno));
		return RK_FAILURE;
	}

	if (!(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) ||
	    !(cap.device_caps & V4L2_CAP_STREAMING)) {
		RK_LOGE("device %s is not streaming video capture", ctx->device_name);
		return RK_FAILURE;
	}

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = ctx->width;
	fmt.fmt.pix.height = ctx->height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if (xioctl(ctx->v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
		RK_LOGE("VIDIOC_S_FMT failed: %s", strerror(errno));
		return RK_FAILURE;
	}

	ctx->width = fmt.fmt.pix.width;
	ctx->height = fmt.fmt.pix.height;

	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = ctx->fps;
	if (xioctl(ctx->v4l2_fd, VIDIOC_S_PARM, &parm) < 0) {
		RK_LOGW("VIDIOC_S_PARM failed: %s", strerror(errno));
	}

	memset(&req, 0, sizeof(req));
	req.count = V4L2_BUFFER_COUNT;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req) < 0) {
		RK_LOGE("VIDIOC_REQBUFS failed: %s", strerror(errno));
		return RK_FAILURE;
	}

	if (req.count < V4L2_BUFFER_COUNT) {
		RK_LOGE("insufficient v4l2 buffers: %u", req.count);
		return RK_FAILURE;
	}

	for (i = 0; i < V4L2_BUFFER_COUNT; i++) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (xioctl(ctx->v4l2_fd, VIDIOC_QUERYBUF, &buf) < 0) {
			RK_LOGE("VIDIOC_QUERYBUF[%d] failed: %s", i, strerror(errno));
			return RK_FAILURE;
		}

		ctx->buffers[i].length = buf.length;
		ctx->buffers[i].start = mmap(RK_NULL, buf.length, PROT_READ | PROT_WRITE,
		                             MAP_SHARED, ctx->v4l2_fd, buf.m.offset);
		if (ctx->buffers[i].start == MAP_FAILED) {
			ctx->buffers[i].start = RK_NULL;
			RK_LOGE("mmap[%d] failed: %s", i, strerror(errno));
			return RK_FAILURE;
		}

		if (xioctl(ctx->v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
			RK_LOGE("VIDIOC_QBUF[%d] failed: %s", i, strerror(errno));
			return RK_FAILURE;
		}
	}

	ctx->queued_buffers = V4L2_BUFFER_COUNT;
	return RK_SUCCESS;
}

static RK_S32 v4l2_start_stream(DEMO_CTX_S *ctx) {
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(ctx->v4l2_fd, VIDIOC_STREAMON, &type) < 0) {
		RK_LOGE("VIDIOC_STREAMON failed: %s", strerror(errno));
		return RK_FAILURE;
	}
	return RK_SUCCESS;
}

static void v4l2_stop_stream(DEMO_CTX_S *ctx) {
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ctx->v4l2_fd >= 0) {
		xioctl(ctx->v4l2_fd, VIDIOC_STREAMOFF, &type);
	}
}

static void v4l2_close_device(DEMO_CTX_S *ctx) {
	RK_S32 i;

	for (i = 0; i < V4L2_BUFFER_COUNT; i++) {
		if (ctx->buffers[i].start != RK_NULL) {
			munmap(ctx->buffers[i].start, ctx->buffers[i].length);
			ctx->buffers[i].start = RK_NULL;
			ctx->buffers[i].length = 0;
		}
	}

	if (ctx->v4l2_fd >= 0) {
		close(ctx->v4l2_fd);
		ctx->v4l2_fd = -1;
	}
}

static RK_S32 send_mjpeg_to_vdec(const void *data, size_t data_len) {
	MB_EXT_CONFIG_S ext_config;
	VDEC_STREAM_S stream;
	MB_BLK mb_blk = RK_NULL;
	RK_U8 *packet;
	RK_S32 ret;

	packet = malloc(data_len);
	if (packet == RK_NULL) {
		return RK_FAILURE;
	}
	memcpy(packet, data, data_len);

	memset(&ext_config, 0, sizeof(ext_config));
	ext_config.pu8VirAddr = packet;
	ext_config.u64Size = data_len;
	ext_config.pOpaque = packet;
	ext_config.pFreeCB = mjpeg_packet_free;

	ret = RK_MPI_SYS_CreateMB(&mb_blk, &ext_config);
	if (ret != RK_SUCCESS) {
		free(packet);
		RK_LOGE("RK_MPI_SYS_CreateMB failed %#X", ret);
		return ret;
	}

	memset(&stream, 0, sizeof(stream));
	stream.pMbBlk = mb_blk;
	stream.u32Len = data_len;
	stream.bEndOfStream = RK_FALSE;
	stream.bEndOfFrame = RK_TRUE;
	stream.bBypassMbBlk = RK_TRUE;

	ret = RK_MPI_VDEC_SendStream(VDEC_CHN_ID, &stream, 1000);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VDEC_SendStream failed %#X", ret);
	}

	RK_MPI_MB_ReleaseMB(mb_blk);
	return ret;
}

static RK_S32 encode_decoded_frame(DEMO_CTX_S *ctx, VIDEO_FRAME_INFO_S *frame) {
	RK_S32 ret;
	void *stream_data;

	ret = RK_MPI_VENC_SendFrame(ctx->venc.s32ChnId, frame, 1000);
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_VENC_SendFrame failed %#X", ret);
		return ret;
	}

	ret = SAMPLE_COMM_VENC_GetStream(&ctx->venc, &stream_data);
	if (ret != RK_SUCCESS) {
		RK_LOGE("SAMPLE_COMM_VENC_GetStream failed %#X", ret);
		return ret;
	}

	if (ctx->output_fp != RK_NULL) {
		fwrite(stream_data, 1, ctx->venc.stFrame.pstPack->u32Len, ctx->output_fp);
		fflush(ctx->output_fp);
	}

	return SAMPLE_COMM_VENC_ReleaseStream(&ctx->venc);
}

static RK_S32 process_one_capture_buffer(DEMO_CTX_S *ctx, RK_S32 frame_index) {
	struct v4l2_buffer buf;
	VIDEO_FRAME_INFO_S decoded_frame;
	RK_S32 ret;

	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;

	if (xioctl(ctx->v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {
		RK_LOGE("VIDIOC_DQBUF failed: %s", strerror(errno));
		return RK_FAILURE;
	}

	if (buf.index >= V4L2_BUFFER_COUNT || ctx->buffers[buf.index].start == RK_NULL) {
		RK_LOGE("invalid buffer index %u", buf.index);
		return RK_FAILURE;
	}

	ret = send_mjpeg_to_vdec(ctx->buffers[buf.index].start, buf.bytesused);
	if (ret == RK_SUCCESS) {
		memset(&decoded_frame, 0, sizeof(decoded_frame));
		ret = RK_MPI_VDEC_GetFrame(VDEC_CHN_ID, &decoded_frame, 1000);
		if (ret == RK_SUCCESS) {
			ret = encode_decoded_frame(ctx, &decoded_frame);
			RK_MPI_VDEC_ReleaseFrame(VDEC_CHN_ID, &decoded_frame);
			if (ret == RK_SUCCESS) {
				RK_LOGI("frame %d encoded, mjpg=%u h265=%u", frame_index,
				        buf.bytesused, ctx->venc.stFrame.pstPack->u32Len);
			}
		} else {
			RK_LOGW("RK_MPI_VDEC_GetFrame timeout/fail %#X", ret);
		}
	}

	if (xioctl(ctx->v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
		RK_LOGE("VIDIOC_QBUF failed: %s", strerror(errno));
		return RK_FAILURE;
	}

	return ret;
}

static RK_S32 parse_cmd_args(int argc, char *argv[], DEMO_CTX_S *ctx) {
	static const char *optstr = "?:d:o:w:h:f:b:n:";
	static const struct option options[] = {
	    {"device", required_argument, RK_NULL, 'd'},
	    {"output", required_argument, RK_NULL, 'o'},
	    {"width", required_argument, RK_NULL, 'w'},
	    {"height", required_argument, RK_NULL, 'h'},
	    {"fps", required_argument, RK_NULL, 'f'},
	    {"bitrate", required_argument, RK_NULL, 'b'},
	    {"count", required_argument, RK_NULL, 'n'},
	    {"help", no_argument, RK_NULL, '?'},
	    {RK_NULL, 0, RK_NULL, 0},
	};
	int c;

	ctx->device_name = "/dev/video21";
	ctx->output_path = "/tmp/easycap.h265";
	ctx->width = 640;
	ctx->height = 480;
	ctx->fps = 25;
	ctx->bitrate_kbps = 512;
	ctx->frame_count = 200;
	ctx->v4l2_fd = -1;

	while ((c = getopt_long(argc, argv, optstr, options, RK_NULL)) != -1) {
		switch (c) {
		case 'd':
			ctx->device_name = optarg;
			break;
		case 'o':
			ctx->output_path = optarg;
			break;
		case 'w':
			ctx->width = atoi(optarg);
			break;
		case 'h':
			ctx->height = atoi(optarg);
			break;
		case 'f':
			ctx->fps = atoi(optarg);
			break;
		case 'b':
			ctx->bitrate_kbps = atoi(optarg);
			break;
		case 'n':
			ctx->frame_count = atoi(optarg);
			break;
		case '?':
		default:
			print_usage(argv[0]);
			return RK_FAILURE;
		}
	}

	return RK_SUCCESS;
}

int main(int argc, char *argv[]) {
	DEMO_CTX_S ctx;
	RK_S32 ret;
	RK_S32 frame_index = 0;

	memset(&ctx, 0, sizeof(ctx));
	if (parse_cmd_args(argc, argv, &ctx) != RK_SUCCESS) {
		return EXIT_FAILURE;
	}

	signal(SIGINT, sigterm_handler);
	signal(SIGTERM, sigterm_handler);

	ret = RK_MPI_SYS_Init();
	if (ret != RK_SUCCESS) {
		RK_LOGE("RK_MPI_SYS_Init failed %#X", ret);
		return EXIT_FAILURE;
	}

	ret = open_output(&ctx);
	if (ret != RK_SUCCESS) {
		goto __FAILED;
	}

	ret = v4l2_open_device(&ctx);
	if (ret != RK_SUCCESS) {
		goto __FAILED;
	}

	ret = init_vdec(ctx.width, ctx.height);
	if (ret != RK_SUCCESS) {
		goto __FAILED;
	}

	ret = init_venc(&ctx);
	if (ret != RK_SUCCESS) {
		goto __FAILED_VDEC;
	}

	ret = v4l2_start_stream(&ctx);
	if (ret != RK_SUCCESS) {
		goto __FAILED_VENC;
	}

	while (!quit && (ctx.frame_count < 0 || frame_index < ctx.frame_count)) {
		ret = process_one_capture_buffer(&ctx, frame_index);
		if (ret != RK_SUCCESS) {
			break;
		}
		frame_index++;
	}

	v4l2_stop_stream(&ctx);
__FAILED_VENC:
	deinit_venc(&ctx);
__FAILED_VDEC:
	deinit_vdec();
__FAILED:
	v4l2_close_device(&ctx);
	close_output(&ctx);
	RK_MPI_SYS_Exit();

	return ret == RK_SUCCESS ? EXIT_SUCCESS : EXIT_FAILURE;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* End of #ifdef __cplusplus */