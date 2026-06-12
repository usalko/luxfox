#include "ulama/ulama_frame.h"

uint16_t ulama_crc16_ccitt(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xFFFF;

	for (size_t index = 0; index < len; index++) {
		crc ^= ((uint16_t)data[index]) << 8;
		for (int bit = 0; bit < 8; bit++) {
			if ((crc & 0x8000U) != 0U) {
				crc = (uint16_t)((crc << 1) ^ 0x1021U);
			} else {
				crc <<= 1;
			}
		}
	}

	return crc;
}

bool ulama_frame_pack(const ulama_frame_view_t *in, uint8_t *out, size_t out_capacity, size_t *out_len)
{
	uint16_t crc;

	if (in == NULL || out == NULL || out_len == NULL) {
		return false;
	}
	if (in->payload_len > ULAMA_FRAME_MAX_PAYLOAD) {
		return false;
	}
	if (ULAMA_FRAME_HEADER_SIZE + in->payload_len > out_capacity) {
		return false;
	}

	out[0] = ULAMA_FRAME_MAGIC;
	out[1] = ULAMA_FRAME_VERSION;
	out[2] = in->src_node;
	out[3] = in->dst_node;
	out[4] = in->flags;
	out[5] = in->traffic_class;
	out[6] = (uint8_t)(in->seq & 0xFFU);
	out[7] = (uint8_t)((in->seq >> 8) & 0xFFU);
	out[8] = in->frag_idx;
	out[9] = in->frag_total;
	out[10] = in->ttl;
	out[11] = 0;

	crc = ulama_crc16_ccitt(out, 12);
	out[12] = (uint8_t)(crc & 0xFFU);
	out[13] = (uint8_t)((crc >> 8) & 0xFFU);

	for (size_t index = 0; index < in->payload_len; index++) {
		out[ULAMA_FRAME_HEADER_SIZE + index] = in->payload[index];
	}

	*out_len = ULAMA_FRAME_HEADER_SIZE + in->payload_len;
	return true;
}

ulama_frame_status_t ulama_frame_unpack_ex(const uint8_t *in, size_t in_len, ulama_frame_view_t *out)
{
	uint16_t got_crc;
	uint16_t exp_crc;

	if (in == NULL || out == NULL) {
		return ULAMA_FRAME_STATUS_INVALID_ARGS;
	}
	if (in_len < 2U) {
		return ULAMA_FRAME_STATUS_SHORT_FRAME;
	}
	if (in[0] != ULAMA_FRAME_MAGIC) {
		return ULAMA_FRAME_STATUS_BAD_MAGIC;
	}
	if (in[1] != ULAMA_FRAME_VERSION) {
		return ULAMA_FRAME_STATUS_VERSION_MISMATCH;
	}
	if (in_len < ULAMA_FRAME_HEADER_SIZE) {
		return ULAMA_FRAME_STATUS_SHORT_FRAME;
	}

	got_crc = (uint16_t)in[12] | ((uint16_t)in[13] << 8);
	exp_crc = ulama_crc16_ccitt(in, 12);
	if (got_crc != exp_crc) {
		return ULAMA_FRAME_STATUS_BAD_CRC;
	}

	out->src_node = in[2];
	out->dst_node = in[3];
	out->flags = in[4];
	out->traffic_class = in[5];
	out->seq = (uint16_t)in[6] | ((uint16_t)in[7] << 8);
	out->frag_idx = in[8];
	out->frag_total = in[9];
	out->ttl = in[10];
	out->payload = &in[ULAMA_FRAME_HEADER_SIZE];
	out->payload_len = in_len - ULAMA_FRAME_HEADER_SIZE;

	return ULAMA_FRAME_STATUS_OK;
}

bool ulama_frame_unpack(const uint8_t *in, size_t in_len, ulama_frame_view_t *out)
{
	return ulama_frame_unpack_ex(in, in_len, out) == ULAMA_FRAME_STATUS_OK;
}