#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ULAMA_ESPNOW_MAX_PAYLOAD 240

typedef struct {
	uint8_t src_mac[6];
	uint8_t data[ULAMA_ESPNOW_MAX_PAYLOAD];
	size_t len;
	int8_t rssi;
} radio_espnow_frame_t;

typedef void (*radio_espnow_rx_cb_t)(const radio_espnow_frame_t *frame, void *user_ctx);
typedef bool (*radio_espnow_control_cb_t)(const uint8_t src_mac[6], int8_t rssi, const uint8_t *data, size_t len, void *user_ctx);

typedef struct {
	uint32_t tx_enqueue_attempts;
	uint32_t tx_enqueue_ok;
	uint32_t tx_enqueue_drop;
	uint32_t tx_enqueue_evict;
	uint32_t tx_dequeue;
	uint32_t tx_send_calls;
	uint32_t tx_send_ok;
	uint32_t tx_send_fail;
	uint32_t tx_send_cb_ok;
	uint32_t tx_send_cb_fail;
	uint32_t rx_cb_frames;
	uint32_t rx_cb_bytes;
	uint32_t rx_invalid_len;
	uint32_t rx_queue_ok;
	uint32_t rx_queue_drop;
	uint32_t rx_dequeue;
	uint32_t rx_delivered;
	int32_t last_rssi;
	int32_t min_rssi;
	int32_t max_rssi;
	uint32_t tx_queue_depth;
	uint32_t rx_queue_depth;
	uint32_t tx_queue_depth_peak;
	uint32_t rx_queue_depth_peak;
	uint8_t peer_known;
} radio_espnow_stats_t;

esp_err_t unow_configure_iface(const char *iface);
const char *unow_get_iface(void);
esp_err_t unow_init_iface(uint8_t node_id, const char *iface);

esp_err_t radio_espnow_init(uint8_t node_id);
esp_err_t radio_espnow_send(const uint8_t *dst_mac, const uint8_t *payload, size_t len);
esp_err_t radio_espnow_add_peer(const uint8_t mac[6]);
bool radio_espnow_peer_known(void);
bool radio_espnow_get_peer_mac(uint8_t out_mac[6]);
void radio_espnow_set_control_callback(radio_espnow_control_cb_t cb, void *user_ctx);
void radio_espnow_set_rx_callback(radio_espnow_rx_cb_t cb, void *user_ctx);
void radio_espnow_get_stats(radio_espnow_stats_t *out);