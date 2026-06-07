#include "unow_internal.h"

#include <string.h>

static size_t unow_align_offset(size_t offset, size_t alignment)
{
	if (alignment <= 1U) {
		return offset;
	}
	return (offset + alignment - 1U) & ~(alignment - 1U);
}

static uint16_t unow_read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t unow_read_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] |
		((uint32_t)bytes[1] << 8) |
		((uint32_t)bytes[2] << 16) |
		((uint32_t)bytes[3] << 24);
}

size_t unow_build_action_frame(uint8_t *buffer, size_t buffer_size, const uint8_t src_mac[6], const uint8_t dst_mac[6], const uint8_t *payload, size_t payload_len, uint8_t rate_500kbps)
{
	struct unow_radiotap_tx_header *rt_header;
	struct unow_dot11_mgmt_header *mgmt_header;
	struct unow_action_vendor_header *vendor_header;
	size_t required_len;

	if (buffer == NULL || src_mac == NULL || dst_mac == NULL || payload == NULL) {
		return 0U;
	}
	required_len = sizeof(*rt_header) + sizeof(*mgmt_header) + sizeof(*vendor_header) + payload_len;
	if (buffer_size < required_len) {
		return 0U;
	}
	rt_header = (struct unow_radiotap_tx_header *)buffer;
	rt_header->version = 0U;
	rt_header->pad = 0U;
	rt_header->len = (uint16_t)sizeof(*rt_header);
	rt_header->present = UNOW_RADIOTAP_PRESENT_RATE | UNOW_RADIOTAP_PRESENT_TX_FLAGS;
	rt_header->rate = rate_500kbps;
	rt_header->rate_pad = 0U;
	rt_header->tx_flags = UNOW_RADIOTAP_TX_FLAGS_NOACK;

	mgmt_header = (struct unow_dot11_mgmt_header *)(buffer + sizeof(*rt_header));
	mgmt_header->frame_control = UNOW_DOT11_FC_ACTION;
	mgmt_header->duration = 0U;
	memcpy(mgmt_header->addr1, dst_mac, sizeof(mgmt_header->addr1));
	memcpy(mgmt_header->addr2, src_mac, sizeof(mgmt_header->addr2));
	memcpy(mgmt_header->addr3, k_unow_bssid, sizeof(mgmt_header->addr3));
	mgmt_header->seq_ctrl = 0U;

	vendor_header = (struct unow_action_vendor_header *)(buffer + sizeof(*rt_header) + sizeof(*mgmt_header));
	vendor_header->category = UNOW_VENDOR_CATEGORY;
	memcpy(vendor_header->oui, k_unow_oui, sizeof(vendor_header->oui));
	vendor_header->subtype = UNOW_VENDOR_SUBTYPE_DATA;

	memcpy(buffer + sizeof(*rt_header) + sizeof(*mgmt_header) + sizeof(*vendor_header), payload, payload_len);
	return required_len;
}

bool unow_radiotap_parse_dbm_signal(const uint8_t *packet, size_t packet_len, int8_t *out_rssi)
{
	static const uint8_t field_alignments[] = {8U, 1U, 1U, 2U, 1U, 1U};
	static const uint8_t field_sizes[] = {8U, 1U, 1U, 4U, 2U, 1U};
	uint16_t radiotap_len;
	uint32_t present;
	size_t offset;
	unsigned int bit;

	if (packet == NULL || out_rssi == NULL || packet_len < 8U) {
		return false;
	}
	radiotap_len = unow_read_le16(packet + 2U);
	if (radiotap_len > packet_len || radiotap_len < 8U) {
		return false;
	}
	present = unow_read_le32(packet + 4U);
	if ((present & UNOW_RADIOTAP_PRESENT_EXT) != 0U) {
		return false;
	}
	offset = 8U;
	for (bit = 0U; bit < 6U; ++bit) {
		if ((present & (1U << bit)) == 0U) {
			continue;
		}
		offset = unow_align_offset(offset, field_alignments[bit]);
		if (offset + field_sizes[bit] > radiotap_len) {
			return false;
		}
		if (bit == 5U) {
			*out_rssi = (int8_t)packet[offset];
			return true;
		}
		offset += field_sizes[bit];
	}
	return false;
}

bool unow_parse_action_frame(const uint8_t *packet, size_t packet_len, unow_diag_frame_t *frame)
{
	const struct unow_dot11_mgmt_header *mgmt_header;
	const struct unow_action_vendor_header *vendor_header;
	uint16_t radiotap_len;
	uint16_t frame_control;
	const uint8_t *payload;
	size_t payload_len;
	int8_t rssi = 0;

	if (packet == NULL || frame == NULL || packet_len < 8U) {
		return false;
	}
	radiotap_len = unow_read_le16(packet + 2U);
	if (radiotap_len > packet_len) {
		return false;
	}
	if (packet_len < radiotap_len + sizeof(*mgmt_header) + sizeof(*vendor_header)) {
		return false;
	}
	mgmt_header = (const struct unow_dot11_mgmt_header *)(packet + radiotap_len);
	frame_control = mgmt_header->frame_control;
	if ((frame_control & 0x00FCU) != UNOW_DOT11_FC_ACTION) {
		return false;
	}
	if (memcmp(mgmt_header->addr3, k_unow_bssid, sizeof(mgmt_header->addr3)) != 0) {
		return false;
	}
	vendor_header = (const struct unow_action_vendor_header *)(packet + radiotap_len + sizeof(*mgmt_header));
	if (vendor_header->category != UNOW_VENDOR_CATEGORY) {
		return false;
	}
	if (memcmp(vendor_header->oui, k_unow_oui, sizeof(vendor_header->oui)) != 0) {
		return false;
	}
	if (vendor_header->subtype != UNOW_VENDOR_SUBTYPE_DATA) {
		return false;
	}
	payload = packet + radiotap_len + sizeof(*mgmt_header) + sizeof(*vendor_header);
	payload_len = packet_len - radiotap_len - sizeof(*mgmt_header) - sizeof(*vendor_header);
	if (payload_len > ULAMA_ESPNOW_MAX_PAYLOAD) {
		return false;
	}
	memset(frame, 0, sizeof(*frame));
	memcpy(frame->src_mac, mgmt_header->addr2, sizeof(frame->src_mac));
	memcpy(frame->dst_mac, mgmt_header->addr1, sizeof(frame->dst_mac));
	memcpy(frame->payload, payload, payload_len);
	frame->len = payload_len;
	if (unow_radiotap_parse_dbm_signal(packet, packet_len, &rssi)) {
		frame->rssi = rssi;
	}
	return true;
}