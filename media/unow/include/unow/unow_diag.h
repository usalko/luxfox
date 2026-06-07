#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "unow/radio_unow.h"

typedef enum {
	UNOW_LOG_ERROR = 0,
	UNOW_LOG_WARN,
	UNOW_LOG_INFO,
	UNOW_LOG_DEBUG,
	UNOW_LOG_TRACE,
} unow_log_level_t;

typedef struct {
	uint8_t src_mac[6];
	uint8_t dst_mac[6];
	uint8_t payload[ULAMA_ESPNOW_MAX_PAYLOAD];
	size_t len;
	int8_t rssi;
} unow_diag_frame_t;

void unow_diag_set_level(unow_log_level_t level);
void unow_diag_set_level_from_env(void);
unow_log_level_t unow_diag_get_level(void);
const char *unow_diag_level_name(unow_log_level_t level);
void unow_diag_log(unow_log_level_t level, const char *file, int line, const char *fmt, ...);
void unow_diag_hexdump(unow_log_level_t level, const char *prefix, const void *data, size_t len, size_t max_len);
bool unow_diag_parse_mac(const char *text, uint8_t mac[6]);
void unow_dump_state(FILE *stream);
esp_err_t unow_diag_recv(unow_diag_frame_t *frame, int timeout_ms);
void unow_diag_dump_frame(FILE *stream, const unow_diag_frame_t *frame);

#define UNOW_LOGE(...) unow_diag_log(UNOW_LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define UNOW_LOGW(...) unow_diag_log(UNOW_LOG_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define UNOW_LOGI(...) unow_diag_log(UNOW_LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define UNOW_LOGD(...) unow_diag_log(UNOW_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define UNOW_LOGT(...) unow_diag_log(UNOW_LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)