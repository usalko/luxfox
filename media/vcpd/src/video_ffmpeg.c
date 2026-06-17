#include "vcpd/video_source.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int video_source_ffmpeg_start(video_source_ffmpeg_t *src)
{
	if (!src || src->running)
		return -1;

	if (!src->device[0])
		strncpy(src->device, "/dev/video0", sizeof(src->device) - 1);
	if (!src->codec[0])
		strncpy(src->codec, "libx264", sizeof(src->codec) - 1);
	if (src->bitrate_kbps <= 0)
		src->bitrate_kbps = 512;
	if (src->width <= 0) src->width = 640;
	if (src->height <= 0) src->height = 480;
	if (src->fps <= 0) src->fps = 25;

	int pipefd[2];
	if (pipe(pipefd) < 0)
		return -1;

	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}

	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[1]);

		int devnull = open("/dev/null", 0);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}

		char bitrate[32], resolution[32], framerate[16];
		snprintf(bitrate, sizeof(bitrate), "%dk", src->bitrate_kbps);
		snprintf(resolution, sizeof(resolution), "%dx%d", src->width, src->height);
		snprintf(framerate, sizeof(framerate), "%d", src->fps);

		execlp("ffmpeg", "ffmpeg",
		       "-f", "v4l2",
		       "-input_format", "mjpeg",
		       "-video_size", resolution,
		       "-framerate", framerate,
		       "-i", src->device,
		       "-c:v", src->codec,
		       "-preset", "ultrafast",
		       "-tune", "zerolatency",
		       "-b:v", bitrate,
		       "-maxrate", bitrate,
		       "-bufsize", bitrate,
		       "-g", "30",
		       "-f", "mpegts",
		       "-muxdelay", "0",
		       "-flush_packets", "1",
		       "pipe:1",
		       NULL);
		_exit(127);
	}

	close(pipefd[1]);
	src->ffmpeg_pid = pid;
	src->pipe_fd = pipefd[0];
	src->running = true;
	return 0;
}

void video_source_ffmpeg_stop(video_source_ffmpeg_t *src)
{
	if (!src || !src->running)
		return;

	if (src->ffmpeg_pid > 0) {
		kill(src->ffmpeg_pid, SIGTERM);
		int status;
		waitpid(src->ffmpeg_pid, &status, 0);
		src->ffmpeg_pid = 0;
	}

	if (src->pipe_fd >= 0) {
		close(src->pipe_fd);
		src->pipe_fd = -1;
	}

	src->running = false;
}

ssize_t video_source_ffmpeg_read(video_source_ffmpeg_t *src, uint8_t *buf, size_t len)
{
	if (!src || !src->running || src->pipe_fd < 0)
		return -1;

	size_t total = 0;
	while (total < len) {
		ssize_t n = read(src->pipe_fd, buf + total, len - total);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return (total > 0) ? (ssize_t)total : n;
		}
		total += (size_t)n;
	}
	return (ssize_t)total;
}
