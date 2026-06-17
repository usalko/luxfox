#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MPEGTS_PACKET_SIZE 188
#define VIDEO_TS_GROUP_SIZE 7
#define VIDEO_TS_GROUP_BYTES (MPEGTS_PACKET_SIZE * VIDEO_TS_GROUP_SIZE)

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
