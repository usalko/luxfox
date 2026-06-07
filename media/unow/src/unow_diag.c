#include "unow_internal.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

static unow_log_level_t g_unow_log_level = UNOW_LOG_INFO;

static int64_t unow_diag_now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000LL + (int64_t)tv.tv_usec / 1000LL;
}

void unow_diag_set_level(unow_log_level_t level)
{
	if (level < UNOW_LOG_ERROR) {
		g_unow_log_level = UNOW_LOG_ERROR;
		return;
	}
	if (level > UNOW_LOG_TRACE) {
		g_unow_log_level = UNOW_LOG_TRACE;
		return;
	}
	g_unow_log_level = level;
}

unow_log_level_t unow_diag_get_level(void)
{
	return g_unow_log_level;
}

const char *unow_diag_level_name(unow_log_level_t level)
{
	switch (level) {
	case UNOW_LOG_ERROR:
		return "ERROR";
	case UNOW_LOG_WARN:
		return "WARN";
	case UNOW_LOG_INFO:
		return "INFO";
	case UNOW_LOG_DEBUG:
		return "DEBUG";
	case UNOW_LOG_TRACE:
		return "TRACE";
	default:
		return "UNKNOWN";
	}
}

void unow_diag_set_level_from_env(void)
{
	const char *level = getenv("UNOW_LOG_LEVEL");

	if (level == NULL || level[0] == '\0') {
		return;
	}
	if (strcasecmp(level, "error") == 0) {
		unow_diag_set_level(UNOW_LOG_ERROR);
	} else if (strcasecmp(level, "warn") == 0 || strcasecmp(level, "warning") == 0) {
		unow_diag_set_level(UNOW_LOG_WARN);
	} else if (strcasecmp(level, "info") == 0) {
		unow_diag_set_level(UNOW_LOG_INFO);
	} else if (strcasecmp(level, "debug") == 0) {
		unow_diag_set_level(UNOW_LOG_DEBUG);
	} else if (strcasecmp(level, "trace") == 0) {
		unow_diag_set_level(UNOW_LOG_TRACE);
	}
}

void unow_diag_log(unow_log_level_t level, const char *file, int line, const char *fmt, ...)
{
	va_list args;
	int64_t now_ms;
	FILE *stream;

	if (level > g_unow_log_level) {
		return;
	}
	now_ms = unow_diag_now_ms();
	stream = stderr;
	fprintf(stream, "[%lld.%03lld] %-5s %s:%d ",
		(long long)(now_ms / 1000LL),
		(long long)(now_ms % 1000LL),
		unow_diag_level_name(level),
		file,
		line);
	va_start(args, fmt);
	vfprintf(stream, fmt, args);
	va_end(args);
	fputc('\n', stream);
}

void unow_diag_hexdump(unow_log_level_t level, const char *prefix, const void *data, size_t len, size_t max_len)
{
	const uint8_t *bytes = data;
	char line[3 * 16 + 1];
	size_t shown;
	size_t offset;
	size_t i;
	char *cursor;

	if (level > g_unow_log_level) {
		return;
	}
	shown = len;
	if (shown > max_len) {
		shown = max_len;
	}
	for (offset = 0; offset < shown; offset += 16U) {
		cursor = line;
		for (i = offset; i < shown && i < offset + 16U; ++i) {
			cursor += snprintf(cursor, (size_t)(line + sizeof(line) - cursor), "%02x ", bytes[i]);
		}
		UNOW_LOGD("%s%s%s", prefix != NULL ? prefix : "", prefix != NULL ? " " : "", line);
	}
	if (shown < len) {
		UNOW_LOGD("%s... truncated %zu/%zu bytes", prefix != NULL ? prefix : "payload", shown, len);
	}
}

bool unow_diag_parse_mac(const char *text, uint8_t mac[6])
{
	return unow_parse_mac(text, mac);
}

void unow_diag_dump_frame(FILE *stream, const unow_diag_frame_t *frame)
{
	char src[18];
	char dst[18];

	if (stream == NULL) {
		stream = stdout;
	}
	if (frame == NULL) {
		fprintf(stream, "frame: <null>\n");
		return;
	}
	unow_format_mac(frame->src_mac, src, sizeof(src));
	unow_format_mac(frame->dst_mac, dst, sizeof(dst));
	fprintf(stream, "frame src=%s dst=%s len=%zu rssi=%d\n", src, dst, frame->len, frame->rssi);
	if (frame->len > 0U) {
		size_t i;

		fprintf(stream, "payload=");
		for (i = 0; i < frame->len; ++i) {
			fprintf(stream, "%02x", frame->payload[i]);
		}
		fputc('\n', stream);
	}
}

esp_err_t unow_diag_recv(unow_diag_frame_t *frame, int timeout_ms)
{
	int64_t deadline_ms;
	int status;
	struct pcap_pkthdr *header;
	const u_char *packet;

	if (frame == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	memset(frame, 0, sizeof(*frame));
	if (!g_unow.initialized || g_unow.pcap == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	deadline_ms = timeout_ms < 0 ? -1 : unow_diag_now_ms() + (int64_t)timeout_ms;
	for (;;) {
		status = pcap_next_ex(g_unow.pcap, &header, &packet);
		if (status == 1) {
			if (!unow_parse_action_frame(packet, header->caplen, frame)) {
				continue;
			}
			pthread_mutex_lock(&g_unow.lock);
			if (!g_unow.peer_known && memcmp(frame->src_mac, g_unow.iface.mac, sizeof(frame->src_mac)) != 0) {
				memcpy(g_unow.peer_mac, frame->src_mac, sizeof(g_unow.peer_mac));
				g_unow.peer_known = true;
				g_unow.stats.peer_known = 1U;
			}
			g_unow.stats.last_rssi = frame->rssi;
			if (!g_unow.rssi_valid) {
				g_unow.stats.min_rssi = frame->rssi;
				g_unow.stats.max_rssi = frame->rssi;
				g_unow.rssi_valid = true;
			} else {
				if (frame->rssi < g_unow.stats.min_rssi) {
					g_unow.stats.min_rssi = frame->rssi;
				}
				if (frame->rssi > g_unow.stats.max_rssi) {
					g_unow.stats.max_rssi = frame->rssi;
				}
			}
			pthread_mutex_unlock(&g_unow.lock);
			return ESP_OK;
		}
		if (status == 0) {
			if (timeout_ms == 0) {
				return ESP_ERR_NOT_FOUND;
			}
			if (deadline_ms >= 0 && unow_diag_now_ms() >= deadline_ms) {
				return ESP_ERR_NOT_FOUND;
			}
			continue;
		}
		if (status == -2) {
			return ESP_ERR_NOT_FOUND;
		}
		UNOW_LOGE("pcap_next_ex failed: %s", pcap_geterr(g_unow.pcap));
		return ESP_FAIL;
	}
}