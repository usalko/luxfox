#include "unow_internal.h"

#include <net/if.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

unow_context_t g_unow = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.iface = {
		.name = UNOW_DEFAULT_IFACE,
	},
	.ack_timeout_us = UNOW_ACK_TIMEOUT_US,
	.ack_max_retry = UNOW_ACK_MAX_RETRY,
};

static const uint8_t k_broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static void unow_reset_stats_locked(void)
{
	memset(&g_unow.stats, 0, sizeof(g_unow.stats));
	g_unow.stats.last_rssi = 0;
	g_unow.stats.min_rssi = 0;
	g_unow.stats.max_rssi = 0;
	g_unow.stats.peer_known = g_unow.peer_known ? 1U : 0U;
	g_unow.rssi_valid = false;
}

esp_err_t unow_configure_iface(const char *iface)
{
	size_t len;

	if (iface == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	len = strlen(iface);
	if (len == 0U || len >= sizeof(g_unow.iface.name)) {
		return ESP_ERR_INVALID_ARG;
	}
	pthread_mutex_lock(&g_unow.lock);
	if (g_unow.initialized) {
		bool same = (strcmp(g_unow.iface.name, iface) == 0);
		pthread_mutex_unlock(&g_unow.lock);
		return same ? ESP_OK : ESP_ERR_INVALID_STATE;
	}
	memcpy(g_unow.iface.name, iface, len + 1U);
	pthread_mutex_unlock(&g_unow.lock);
	return ESP_OK;
}

const char *unow_get_iface(void)
{
	if (g_unow.iface.name[0] == '\0') {
		return UNOW_DEFAULT_IFACE;
	}
	return g_unow.iface.name;
}

esp_err_t unow_init_iface(uint8_t node_id, const char *iface)
{
	char error_buf[256] = {0};
	unow_iface_info_t iface_info;
	pcap_t *pcap_handle = NULL;
	int datalink = 0;
	char mac_string[18];
	const char *active_iface;

	unow_diag_set_level_from_env();
	if (iface != NULL) {
		esp_err_t err = unow_configure_iface(iface);

		if (err != ESP_OK) {
			return err;
		}
	}
	pthread_mutex_lock(&g_unow.lock);
	if (g_unow.initialized && g_unow.pcap != NULL) {
		pthread_mutex_unlock(&g_unow.lock);
		return ESP_OK;
	}
	pthread_mutex_unlock(&g_unow.lock);
	active_iface = unow_get_iface();
	if (unow_iface_query(active_iface, &iface_info, error_buf, sizeof(error_buf)) != 0) {
		UNOW_LOGE("interface probe failed for %s: %s", active_iface, error_buf);
		return ESP_ERR_NOT_FOUND;
	}
	if (unow_iface_open_pcap(active_iface, &pcap_handle, &datalink, error_buf, sizeof(error_buf)) != 0) {
		UNOW_LOGE("pcap open failed for %s: %s", active_iface, error_buf);
		return ESP_FAIL;
	}
	UNOW_LOGI("DEBUG: opened pcap for %s, datalink=%d DLT_IEEE802_11_RADIO=%d", active_iface, datalink, DLT_IEEE802_11_RADIO);
	if (datalink != DLT_IEEE802_11_RADIO) {
		UNOW_LOGE("interface %s is not radiotap/monitor (datalink=%d:%s, expected=%d:%s)", 
			active_iface, datalink, pcap_datalink_val_to_name(datalink), 
			DLT_IEEE802_11_RADIO, pcap_datalink_val_to_name(DLT_IEEE802_11_RADIO));
		UNOW_LOGE("TIP: Check with 'iw dev %s link' and 'iw %s info' on device", active_iface, active_iface);
		unow_iface_close_pcap(&pcap_handle);
		return ESP_ERR_INVALID_STATE;
	}
	pthread_mutex_lock(&g_unow.lock);
	if (g_unow.pcap != NULL) {
		unow_iface_close_pcap(&g_unow.pcap);
	}
	g_unow.node_id = node_id;
	g_unow.iface = iface_info;
	g_unow.pcap = pcap_handle;
	g_unow.datalink = datalink;
	g_unow.initialized = true;
	unow_reset_stats_locked();
	pthread_mutex_unlock(&g_unow.lock);
	unow_format_mac(g_unow.iface.mac, mac_string, sizeof(mac_string));
	UNOW_LOGI("initialized iface=%s ifindex=%d mac=%s datalink=%s node_id=%u",
		active_iface,
		g_unow.iface.ifindex,
		mac_string,
		pcap_datalink_val_to_name(g_unow.datalink),
		(unsigned int)node_id);
	return ESP_OK;
}

void unow_deinit(void)
{
	pthread_mutex_lock(&g_unow.lock);
	if (g_unow.pcap != NULL) {
		unow_iface_close_pcap(&g_unow.pcap);
	}
	g_unow.initialized = false;
	pthread_mutex_unlock(&g_unow.lock);
}

esp_err_t radio_espnow_init(uint8_t node_id)
{
	return unow_init_iface(node_id, NULL);
}

esp_err_t radio_espnow_send(const uint8_t *dst_mac, const uint8_t *payload, size_t len)
{
	uint8_t packet[sizeof(struct unow_radiotap_tx_header) + sizeof(struct unow_dot11_mgmt_header) + sizeof(struct unow_action_vendor_header) + ULAMA_ESPNOW_MAX_PAYLOAD];
	size_t packet_len;
	const uint8_t *actual_dst = dst_mac != NULL ? dst_mac : k_broadcast_mac;
	int injected;
	char dst_string[18];

	if (payload == NULL || len == 0U || len > ULAMA_ESPNOW_MAX_PAYLOAD) {
		return ESP_ERR_INVALID_ARG;
	}
	pthread_mutex_lock(&g_unow.lock);
	g_unow.stats.tx_enqueue_attempts++;
	if (!g_unow.initialized || g_unow.pcap == NULL) {
		pthread_mutex_unlock(&g_unow.lock);
		return ESP_ERR_INVALID_STATE;
	}
	g_unow.stats.tx_enqueue_ok++;
	g_unow.stats.tx_dequeue++;
	g_unow.stats.tx_send_calls++;
	packet_len = unow_build_action_frame(packet, sizeof(packet), g_unow.iface.mac, actual_dst, payload, len, UNOW_TX_RATE_1MBPS);
	if (packet_len == 0U) {
		g_unow.stats.tx_send_fail++;
		pthread_mutex_unlock(&g_unow.lock);
		return ESP_FAIL;
	}
	injected = pcap_inject(g_unow.pcap, packet, packet_len);
	if (injected < 0) {
		g_unow.stats.tx_send_fail++;
		unow_format_mac(actual_dst, dst_string, sizeof(dst_string));
		UNOW_LOGE("pcap_inject failed dst=%s: %s", dst_string, pcap_geterr(g_unow.pcap));
		pthread_mutex_unlock(&g_unow.lock);
		return ESP_FAIL;
	}
	g_unow.stats.tx_send_ok++;
	pthread_mutex_unlock(&g_unow.lock);
	unow_format_mac(actual_dst, dst_string, sizeof(dst_string));
	UNOW_LOGD("sent %zu bytes over %s to %s", len, g_unow.iface.name, dst_string);
	UNOW_LOGT("wire packet len=%zu", packet_len);
	unow_diag_hexdump(UNOW_LOG_TRACE, "tx", packet, packet_len, packet_len);
	return ESP_OK;
}

int64_t unow_now_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

void unow_async_tick_locked(void)
{
	int64_t now = unow_now_us();

	for (int i = 0; i < UNOW_ASYNC_SLOTS; i++) {
		unow_async_slot_t *slot = &g_unow.async_slots[i];
		if (!slot->active)
			continue;
		int64_t elapsed = now - slot->sent_us;
		if (elapsed < (int64_t)g_unow.ack_timeout_us)
			continue;
		if (slot->attempts_left > 0) {
			int injected = pcap_inject(g_unow.pcap, slot->frame, slot->frame_len);
			if (injected >= 0) {
				g_unow.stats.tx_retries++;
				g_unow.stats.tx_send_calls++;
			}
			slot->sent_us = now;
			slot->attempts_left--;
			UNOW_LOGT("async retx seq=%u attempts_left=%u", slot->seq, slot->attempts_left);
		} else {
			slot->active = false;
			g_unow.stats.tx_ack_timeout++;
			UNOW_LOGT("async timeout seq=%u", slot->seq);
		}
	}
}

void unow_async_ack_locked(uint16_t ack_seq)
{
	for (int i = 0; i < UNOW_ASYNC_SLOTS; i++) {
		unow_async_slot_t *slot = &g_unow.async_slots[i];
		if (slot->active && slot->seq == ack_seq) {
			slot->active = false;
			g_unow.stats.tx_ack_ok++;
			UNOW_LOGT("async ack seq=%u", ack_seq);
			return;
		}
	}
}

esp_err_t radio_espnow_send_reliable(const uint8_t *dst_mac, const uint8_t *payload, size_t len)
{
	uint8_t seq_payload[2U + ULAMA_ESPNOW_MAX_PAYLOAD];
	const uint8_t *actual_dst = dst_mac != NULL ? dst_mac : k_broadcast_mac;
	uint16_t seq;
	int slot_idx;

	if (payload == NULL || len == 0U || len > ULAMA_ESPNOW_MAX_PAYLOAD - 2U) {
		return ESP_ERR_INVALID_ARG;
	}
	pthread_mutex_lock(&g_unow.lock);
	g_unow.stats.tx_enqueue_attempts++;
	if (!g_unow.initialized || g_unow.pcap == NULL) {
		pthread_mutex_unlock(&g_unow.lock);
		return ESP_ERR_INVALID_STATE;
	}

	unow_async_tick_locked();

	slot_idx = -1;
	for (int i = 0; i < UNOW_ASYNC_SLOTS; i++) {
		if (!g_unow.async_slots[i].active) {
			slot_idx = i;
			break;
		}
	}

	g_unow.stats.tx_enqueue_ok++;
	g_unow.stats.tx_dequeue++;

	seq = ++g_unow.tx_seq;
	seq_payload[0] = (uint8_t)(seq >> 8);
	seq_payload[1] = (uint8_t)(seq & 0xFFU);
	memcpy(seq_payload + 2U, payload, len);

	if (slot_idx >= 0) {
		unow_async_slot_t *slot = &g_unow.async_slots[slot_idx];
		slot->frame_len = unow_build_action_frame_ex(
			slot->frame, sizeof(slot->frame),
			g_unow.iface.mac, actual_dst,
			seq_payload, len + 2U,
			UNOW_TX_RATE_1MBPS, UNOW_VENDOR_SUBTYPE_DATA_SEQ);
		if (slot->frame_len == 0U) {
			g_unow.stats.tx_send_fail++;
			pthread_mutex_unlock(&g_unow.lock);
			return ESP_FAIL;
		}
		g_unow.stats.tx_send_calls++;
		int injected = pcap_inject(g_unow.pcap, slot->frame, slot->frame_len);
		if (injected < 0) {
			g_unow.stats.tx_send_fail++;
			pthread_mutex_unlock(&g_unow.lock);
			return ESP_FAIL;
		}
		slot->seq = seq;
		slot->sent_us = unow_now_us();
		slot->attempts_left = (uint8_t)g_unow.ack_max_retry;
		slot->active = true;
		g_unow.stats.tx_send_ok++;
	} else {
		uint8_t packet[sizeof(struct unow_radiotap_tx_header) +
			       sizeof(struct unow_dot11_mgmt_header) +
			       sizeof(struct unow_action_vendor_header) +
			       2U + ULAMA_ESPNOW_MAX_PAYLOAD];
		size_t packet_len = unow_build_action_frame_ex(
			packet, sizeof(packet),
			g_unow.iface.mac, actual_dst,
			seq_payload, len + 2U,
			UNOW_TX_RATE_1MBPS, UNOW_VENDOR_SUBTYPE_DATA_SEQ);
		if (packet_len > 0U) {
			g_unow.stats.tx_send_calls++;
			pcap_inject(g_unow.pcap, packet, packet_len);
		}
		g_unow.stats.tx_send_ok++;
		g_unow.stats.tx_ack_timeout++;
		UNOW_LOGD("async slots full seq=%u, sent unreliable", seq);
	}

	pthread_mutex_unlock(&g_unow.lock);
	return ESP_OK;
}

esp_err_t radio_espnow_add_peer(const uint8_t mac[6])
{
	if (mac == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	pthread_mutex_lock(&g_unow.lock);
	memcpy(g_unow.peer_mac, mac, sizeof(g_unow.peer_mac));
	g_unow.peer_known = true;
	g_unow.stats.peer_known = 1U;
	pthread_mutex_unlock(&g_unow.lock);
	return ESP_OK;
}

bool radio_espnow_peer_known(void)
{
	bool known;

	pthread_mutex_lock(&g_unow.lock);
	known = g_unow.peer_known;
	pthread_mutex_unlock(&g_unow.lock);
	return known;
}

bool radio_espnow_get_peer_mac(uint8_t out_mac[6])
{
	bool known;

	if (out_mac == NULL) {
		return false;
	}
	pthread_mutex_lock(&g_unow.lock);
	known = g_unow.peer_known;
	if (known) {
		memcpy(out_mac, g_unow.peer_mac, sizeof(g_unow.peer_mac));
	}
	pthread_mutex_unlock(&g_unow.lock);
	return known;
}

void radio_espnow_set_control_callback(radio_espnow_control_cb_t cb, void *user_ctx)
{
	pthread_mutex_lock(&g_unow.lock);
	g_unow.control_cb = cb;
	g_unow.control_cb_ctx = user_ctx;
	pthread_mutex_unlock(&g_unow.lock);
}

void radio_espnow_set_rx_callback(radio_espnow_rx_cb_t cb, void *user_ctx)
{
	pthread_mutex_lock(&g_unow.lock);
	g_unow.rx_cb = cb;
	g_unow.rx_cb_ctx = user_ctx;
	pthread_mutex_unlock(&g_unow.lock);
}

void radio_espnow_get_stats(radio_espnow_stats_t *out)
{
	if (out == NULL) {
		return;
	}
	pthread_mutex_lock(&g_unow.lock);
	g_unow.stats.peer_known = g_unow.peer_known ? 1U : 0U;
	*out = g_unow.stats;
	pthread_mutex_unlock(&g_unow.lock);
}

void unow_set_ack_params(uint32_t timeout_us, uint32_t max_retry)
{
	pthread_mutex_lock(&g_unow.lock);
	g_unow.ack_timeout_us = timeout_us > 0 ? timeout_us : UNOW_ACK_TIMEOUT_US;
	g_unow.ack_max_retry = max_retry;
	pthread_mutex_unlock(&g_unow.lock);
}

void unow_dump_state(FILE *stream)
{
	radio_espnow_stats_t stats;
	char mac_string[18];
	char peer_string[18];

	if (stream == NULL) {
		stream = stdout;
	}
	radio_espnow_get_stats(&stats);
	pthread_mutex_lock(&g_unow.lock);
	unow_format_mac(g_unow.iface.mac, mac_string, sizeof(mac_string));
	if (g_unow.peer_known) {
		unow_format_mac(g_unow.peer_mac, peer_string, sizeof(peer_string));
	} else {
		snprintf(peer_string, sizeof(peer_string), "<none>");
	}
	fprintf(stream, "UNOW state\n");
	fprintf(stream, "  initialized: %s\n", g_unow.initialized ? "yes" : "no");
	fprintf(stream, "  iface: %s\n", g_unow.iface.name[0] != '\0' ? g_unow.iface.name : UNOW_DEFAULT_IFACE);
	fprintf(stream, "  ifindex: %d\n", g_unow.iface.ifindex);
	fprintf(stream, "  mac: %s\n", mac_string);
	fprintf(stream, "  datalink: %d (%s)\n", g_unow.datalink, pcap_datalink_val_to_name(g_unow.datalink));
	fprintf(stream, "  node_id: %u\n", (unsigned int)g_unow.node_id);
	fprintf(stream, "  peer: %s\n", peer_string);
	fprintf(stream, "  stats tx_ok=%u tx_fail=%u rx_frames=%u last_rssi=%d min_rssi=%d max_rssi=%d\n",
		stats.tx_send_ok,
		stats.tx_send_fail,
		stats.rx_cb_frames,
		stats.last_rssi,
		stats.min_rssi,
		stats.max_rssi);
	fprintf(stream, "  ack tx_ack_ok=%u tx_ack_timeout=%u tx_retries=%u rx_ack_sent=%u rx_dedup_dropped=%u\n",
		stats.tx_ack_ok,
		stats.tx_ack_timeout,
		stats.tx_retries,
		stats.rx_ack_sent,
		stats.rx_dedup_dropped);
	pthread_mutex_unlock(&g_unow.lock);
}