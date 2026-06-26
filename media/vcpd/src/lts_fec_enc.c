#include "vcpd/lts_fec_enc.h"

#include <string.h>

void lts_fec_encoder_init(lts_fec_encoder_t *fec, int group_size)
{
	memset(fec, 0, sizeof(*fec));
	fec->group_size = (group_size >= 2 && group_size <= LTS_FEC_MAX_GROUP) ? group_size : 4;
}

bool lts_fec_encoder_add(lts_fec_encoder_t *fec, const lts_encoded_packet_t *pkt,
			 lts_encoder_t *enc, lts_encoded_packet_t *fec_out)
{
	if (!fec || !pkt || !enc || !fec_out || fec->group_size < 2)
		return false;
	if (pkt->len <= LTS_ENC_HEADER_SIZE)
		return false;

	const uint8_t *payload = pkt->data + LTS_ENC_HEADER_SIZE;
	size_t payload_len = pkt->len - LTS_ENC_HEADER_SIZE;
	if (payload_len > LTS_FEC_MAX_XOR)
		payload_len = LTS_FEC_MAX_XOR;

	uint8_t flags = pkt->data[3];

	if (fec->count == 0) {
		fec->group_start_seq = pkt->pkt_seq;
		memcpy(fec->xor_buf, payload, payload_len);
		fec->max_len = payload_len;
		fec->flags_xor = flags;
	} else {
		size_t xor_len = payload_len > fec->max_len ? payload_len : fec->max_len;
		for (size_t i = 0; i < xor_len; i++) {
			uint8_t a = (i < fec->max_len) ? fec->xor_buf[i] : 0;
			uint8_t b = (i < payload_len) ? payload[i] : 0;
			fec->xor_buf[i] = a ^ b;
		}
		fec->max_len = xor_len;
		fec->flags_xor ^= flags;
	}
	fec->count++;

	if (fec->count < fec->group_size)
		return false;

	uint8_t fec_payload[LTS_ENC_MAX_PAYLOAD];
	fec_payload[0] = (uint8_t)(fec->group_start_seq >> 8);
	fec_payload[1] = (uint8_t)(fec->group_start_seq & 0xFF);
	fec_payload[2] = (uint8_t)fec->count;
	fec_payload[3] = fec->flags_xor;
	memcpy(fec_payload + LTS_FEC_HEADER_SIZE, fec->xor_buf, fec->max_len);

	size_t total = LTS_FEC_HEADER_SIZE + fec->max_len;

	uint16_t fec_seq = enc->next_seq++;
	fec_out->pkt_seq = fec_seq;
	fec_out->len = lts_encode_single(enc->stream_id, fec_seq, LTS_ENC_FLAG_FEC,
					 fec_payload, total,
					 fec_out->data, sizeof(fec_out->data));

	fec->count = 0;
	fec->max_len = 0;
	fec->flags_xor = 0;

	return fec_out->len > 0;
}
