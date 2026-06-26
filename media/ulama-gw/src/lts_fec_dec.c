#include "ulama_gw/lts_fec_dec.h"

#include <string.h>

void lts_fec_decoder_init(lts_fec_decoder_t *dec)
{
	memset(dec, 0, sizeof(*dec));
}

void lts_fec_decoder_add_data(lts_fec_decoder_t *dec, uint16_t seq,
			      const uint8_t *raw, size_t raw_len)
{
	if (!dec || !raw || raw_len == 0)
		return;

	int idx = seq % LTS_FEC_RAW_RING_SIZE;
	lts_fec_raw_slot_t *slot = &dec->ring[idx];
	size_t copy = raw_len;
	if (copy > sizeof(slot->data))
		copy = sizeof(slot->data);
	memcpy(slot->data, raw, copy);
	slot->len = copy;
	slot->seq = seq;
	slot->valid = true;
}

static const lts_fec_raw_slot_t *lookup_raw(const lts_fec_decoder_t *dec, uint16_t seq)
{
	int idx = seq % LTS_FEC_RAW_RING_SIZE;
	const lts_fec_raw_slot_t *slot = &dec->ring[idx];
	if (slot->valid && slot->seq == seq)
		return slot;
	return NULL;
}

bool lts_fec_decoder_add_parity(lts_fec_decoder_t *dec, const uint8_t *payload,
				size_t payload_len, lts_packet_t *recovered)
{
	if (!dec || !payload || payload_len < LTS_FEC_HEADER_SIZE || !recovered)
		return false;

	uint16_t start_seq = ((uint16_t)payload[0] << 8) | payload[1];
	uint8_t count = payload[2];
	if (count < 2 || count > LTS_FEC_DEC_MAX_GROUP)
		return false;

	const uint8_t *xor_data = payload + LTS_FEC_HEADER_SIZE;
	size_t xor_len = payload_len - LTS_FEC_HEADER_SIZE;

	int missing_idx = -1;
	int missing_count = 0;
	for (int i = 0; i < count; i++) {
		uint16_t seq = start_seq + (uint16_t)i;
		if (!lookup_raw(dec, seq)) {
			missing_count++;
			missing_idx = i;
		}
	}

	if (missing_count != 1) {
		if (missing_count > 1)
			dec->unrecoverable++;
		return false;
	}

	uint8_t result[LTS_HEADER_SIZE + LTS_MAX_PAYLOAD];
	size_t result_len = xor_len;
	if (result_len > sizeof(result))
		result_len = sizeof(result);
	memcpy(result, xor_data, result_len);

	for (int i = 0; i < count; i++) {
		if (i == missing_idx)
			continue;
		const lts_fec_raw_slot_t *slot = lookup_raw(dec, start_seq + (uint16_t)i);
		if (!slot)
			return false;
		size_t xl = slot->len > result_len ? slot->len : result_len;
		if (xl > sizeof(result))
			xl = sizeof(result);
		for (size_t j = 0; j < xl; j++) {
			uint8_t a = (j < result_len) ? result[j] : 0;
			uint8_t b = (j < slot->len) ? slot->data[j] : 0;
			result[j] = a ^ b;
		}
		result_len = xl;
	}

	if (result_len < LTS_HEADER_SIZE) {
		dec->unrecoverable++;
		return false;
	}

	if (!lts_decode_packet(result, result_len, recovered)) {
		dec->unrecoverable++;
		return false;
	}

	dec->recovered++;
	return true;
}
