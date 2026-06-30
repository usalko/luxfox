#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MPEGTS_PACKET_SIZE 188
#define VIDEO_TS_GROUP_SIZE 7
#define VIDEO_TS_GROUP_BYTES (MPEGTS_PACKET_SIZE * VIDEO_TS_GROUP_SIZE)

/*
 * In MPP (ring-buffer) mode video_source_mpp_read() returns one ring slot per
 * call — up to VIDEO_RING_SLOT_MAX bytes.  Callers must size their read buffer
 * to at least this value when VCPD_WITH_MPP is defined.
 */
#ifdef VCPD_WITH_MPP
#define VIDEO_MPP_READ_MAX 65536
#endif

typedef struct {
	pid_t ffmpeg_pid;
	int pipe_fd;
	char device[64];
	char codec[16];
	int bitrate_kbps;
	int width;
	int height;
	int fps;
	bool running;
} video_source_ffmpeg_t;

int video_source_ffmpeg_start(video_source_ffmpeg_t *src);
void video_source_ffmpeg_stop(video_source_ffmpeg_t *src);
ssize_t video_source_ffmpeg_read(video_source_ffmpeg_t *src, uint8_t *buf, size_t len);

typedef struct {
	int pipe_fd;
	char device[64];
	int bitrate_kbps;
	int width;
	int height;
	int fps;
	int gop;
	bool running;
	bool test_pattern;
	void *mpp_ctx;
} video_source_mpp_t;

#ifdef VCPD_WITH_MPP
int video_source_mpp_start(video_source_mpp_t *src);
void video_source_mpp_stop(video_source_mpp_t *src);
ssize_t video_source_mpp_read(video_source_mpp_t *src, uint8_t *buf, size_t len);
typedef video_source_mpp_t video_source_t;
#else
typedef video_source_ffmpeg_t video_source_t;
#endif
