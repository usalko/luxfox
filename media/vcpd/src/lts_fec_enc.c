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

	if (fec->count == 0) {
		fec->group_start_seq = pkt->pkt_seq;
		memcpy(fec->xor_buf, pkt->data, pkt->len);
		fec->max_len = pkt->len;
	} else {
		size_t xor_len = pkt->len > fec->max_len ? pkt->len : fec->max_len;
		for (size_t i = 0; i < xor_len; i++) {
			uint8_t a = (i < fec->max_len) ? fec->xor_buf[i] : 0;
			uint8_t b = (i < pkt->len) ? pkt->data[i] : 0;
			fec->xor_buf[i] = a ^ b;
		}
		fec->max_len = xor_len;
	}
	fec->count++;

	if (fec->count < fec->group_size)
		return false;

	uint8_t payload[LTS_FEC_HEADER_SIZE + LTS_ENC_HEADER_SIZE + LTS_ENC_MAX_PAYLOAD];
	payload[0] = (uint8_t)(fec->group_start_seq >> 8);
	payload[1] = (uint8_t)(fec->group_start_seq & 0xFF);
	payload[2] = (uint8_t)fec->count;
	memcpy(payload + LTS_FEC_HEADER_SIZE, fec->xor_buf, fec->max_len);

	size_t total = LTS_FEC_HEADER_SIZE + fec->max_len;
	if (total > LTS_ENC_MAX_PAYLOAD)
		total = LTS_ENC_MAX_PAYLOAD;

	uint16_t fec_seq = enc->next_seq++;
	fec_out->pkt_seq = fec_seq;
	fec_out->len = lts_encode_single(enc->stream_id, fec_seq, LTS_ENC_FLAG_FEC,
					 payload, total,
					 fec_out->data, sizeof(fec_out->data));

	fec->count = 0;
	fec->max_len = 0;

	return fec_out->len > 0;
}
