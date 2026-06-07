#pragma once

#include <sys/types.h>

#include <net/if.h>
#include <pcap/pcap.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifndef IFNAMSIZ
#define IFNAMSIZ IF_NAMESIZE
#endif

#include "unow/radio_unow.h"
#include "unow/unow_diag.h"
#include "unow/unow_wire.h"

typedef struct {
	char name[IFNAMSIZ];
	int ifindex;
	unsigned int flags;
	uint8_t mac[6];
} unow_iface_info_t;

typedef struct {
	pthread_mutex_t lock;
	bool initialized;
	bool rssi_valid;
	uint8_t node_id;
	unow_iface_info_t iface;
	pcap_t *pcap;
	int datalink;
	radio_espnow_rx_cb_t rx_cb;
	void *rx_cb_ctx;
	radio_espnow_control_cb_t control_cb;
	void *control_cb_ctx;
	radio_espnow_stats_t stats;
	bool peer_known;
	uint8_t peer_mac[6];
} unow_context_t;

extern unow_context_t g_unow;

int unow_iface_query(const char *iface, unow_iface_info_t *out, char *error_buf, size_t error_buf_len);
int unow_iface_open_pcap(const char *iface, pcap_t **out_handle, int *out_datalink, char *error_buf, size_t error_buf_len);
int unow_iface_apply_filter(pcap_t *handle, char *error_buf, size_t error_buf_len);
void unow_iface_close_pcap(pcap_t **handle);

bool unow_parse_mac(const char *text, uint8_t mac[6]);
void unow_format_mac(const uint8_t mac[6], char *buffer, size_t buffer_size);
bool unow_mac_is_broadcast(const uint8_t mac[6]);

size_t unow_build_action_frame(uint8_t *buffer, size_t buffer_size, const uint8_t src_mac[6], const uint8_t dst_mac[6], const uint8_t *payload, size_t payload_len, uint8_t rate_500kbps);
bool unow_parse_action_frame(const uint8_t *packet, size_t packet_len, unow_diag_frame_t *frame);
bool unow_radiotap_parse_dbm_signal(const uint8_t *packet, size_t packet_len, int8_t *out_rssi);