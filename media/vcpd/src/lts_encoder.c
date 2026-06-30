#include "vcpd/lts_encoder.h"

#include <string.h>

void lts_encoder_init(lts_encoder_t *enc, uint8_t stream_id)
{
	enc->stream_id = stream_id;
	enc->next_seq = 0;
	enc->max_payload = LTS_ENC_MAX_PAYLOAD;
}

size_t lts_encoder_packet_count(const lts_encoder_t *enc, size_t payload_len)
{
	if (payload_len == 0)
		return 0;

	size_t mtu = (enc && enc->max_payload > 0) ? enc->max_payload : LTS_ENC_MAX_PAYLOAD;
	return (payload_len + mtu - 1) / mtu;
}

size_t lts_encode_single(uint8_t stream_id, uint16_t pkt_seq, uint8_t flags,
			 const uint8_t *payload, size_t payload_len,
			 uint8_t *out, size_t out_capacity)
{
	size_t total = LTS_ENC_HEADER_SIZE + payload_len;
	if (total > out_capacity || payload_len > LTS_ENC_MAX_PAYLOAD)
		return 0;

	out[0] = stream_id;
	out[1] = (uint8_t)(pkt_seq >> 8);
	out[2] = (uint8_t)(pkt_seq & 0xFF);
	out[3] = flags;

	if (payload_len > 0 && payload)
		memcpy(out + LTS_ENC_HEADER_SIZE, payload, payload_len);

	return total;
}

size_t lts_encoder_encode(lts_encoder_t *enc, const uint8_t *payload, size_t payload_len,
			  uint8_t flags, lts_encoded_packet_t *out, size_t max_out)
{
	if (!enc || !payload || !out || max_out == 0)
		return 0;

	size_t offset = 0;
	size_t count = 0;

	size_t mtu = enc->max_payload > 0 ? enc->max_payload : LTS_ENC_MAX_PAYLOAD;

	while (offset < payload_len && count < max_out) {
		size_t chunk = payload_len - offset;
		if (chunk > mtu)
			chunk = mtu;

		uint8_t pkt_flags = flags;
		if (offset == 0)
			pkt_flags |= LTS_ENC_FLAG_FIRST_OF_FRAME;
		if (offset + chunk >= payload_len)
			pkt_flags |= LTS_ENC_FLAG_LAST_OF_FRAME;

		out[count].pkt_seq = enc->next_seq;
		out[count].len = lts_encode_single(enc->stream_id, enc->next_seq, pkt_flags,
						   payload + offset, chunk,
						   out[count].data, sizeof(out[count].data));

		enc->next_seq++;
		offset += chunk;
		count++;
	}

	return count;
}

bool lts_enc_is_nack(const uint8_t *data, size_t len)
{
	return len >= 2 && data[0] == LTS_ENC_NACK_MAGIC0 && data[1] == LTS_ENC_NACK_MAGIC1;
}

bool lts_enc_decode_nack(const uint8_t *data, size_t len, lts_enc_nack_t *out)
{
	if (!data || !out || len < LTS_ENC_NACK_SIZE)
		return false;
	if (data[0] != LTS_ENC_NACK_MAGIC0 || data[1] != LTS_ENC_NACK_MAGIC1)
		return false;

	out->stream_id = data[2];
	out->start_seq = ((uint16_t)data[3] << 8) | data[4];
	out->bitmask = ((uint16_t)data[5] << 8) | data[6];
	return true;
}

void lts_retx_buf_init(lts_retx_buf_t *buf)
{
	memset(buf, 0, sizeof(*buf));
}

void lts_retx_buf_store(lts_retx_buf_t *buf, const lts_encoded_packet_t *pkt)
{
	if (!buf || !pkt)
		return;
	int idx = pkt->pkt_seq % LTS_RETX_SLOTS;
	buf->packets[idx] = *pkt;
	buf->valid[idx] = true;
}

const lts_encoded_packet_t *lts_retx_buf_find(const lts_retx_buf_t *buf, uint16_t seq)
{
	if (!buf)
		return NULL;
	int idx = seq % LTS_RETX_SLOTS;
	if (buf->valid[idx] && buf->packets[idx].pkt_seq == seq)
		return &buf->packets[idx];
	return NULL;
}
