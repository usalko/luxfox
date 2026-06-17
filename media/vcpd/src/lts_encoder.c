#include "vcpd/lts_encoder.h"

#include <string.h>

void lts_encoder_init(lts_encoder_t *enc, uint8_t stream_id)
{
	enc->stream_id = stream_id;
	enc->next_seq = 0;
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

	while (offset < payload_len && count < max_out) {
		size_t chunk = payload_len - offset;
		if (chunk > LTS_ENC_MAX_PAYLOAD)
			chunk = LTS_ENC_MAX_PAYLOAD;

		uint8_t pkt_flags = flags;
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
