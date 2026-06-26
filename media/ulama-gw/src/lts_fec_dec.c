#include "ulama_gw/lts_fec_dec.h"

#include <string.h>

void lts_fec_decoder_init(lts_fec_decoder_t *dec)
{
	memset(dec, 0, sizeof(*dec));
}

void lts_fec_decoder_add_data(lts_fec_decoder_t *dec, uint16_t seq,
			      uint8_t flags, const uint8_t *lts_payload,
			      size_t lts_payload_len)
{
	if (!dec || !lts_payload || lts_payload_len == 0)
		return;

	int idx = seq % LTS_FEC_RAW_RING_SIZE;
	lts_fec_raw_slot_t *slot = &dec->ring[idx];
	size_t copy = lts_payload_len;
	if (copy > sizeof(slot->payload))
		copy = sizeof(slot->payload);
	memcpy(slot->payload, lts_payload, copy);
	slot->payload_len = copy;
	slot->flags = flags;
	slot->seq = seq;
	slot->valid = true;
}

static const lts_fec_raw_slot_t *lookup(const lts_fec_decoder_t *dec, uint16_t seq)
{
	int idx = seq % LTS_FEC_RAW_RING_SIZE;
	const lts_fec_raw_slot_t *slot = &dec->ring[idx];
	if (slot->valid && slot->seq == seq)
		return slot;
	return NULL;
}

bool lts_fec_decoder_add_parity(lts_fec_decoder_t *dec, uint8_t stream_id,
				const uint8_t *payload, size_t payload_len,
				lts_packet_t *recovered)
{
	if (!dec || !payload || payload_len < LTS_FEC_HEADER_SIZE || !recovered)
		return false;

	uint16_t start_seq = ((uint16_t)payload[0] << 8) | payload[1];
	uint8_t count = payload[2];
	uint8_t flags_xor = payload[3];
	if (count < 2 || count > LTS_FEC_DEC_MAX_GROUP)
		return false;

	const uint8_t *xor_data = payload + LTS_FEC_HEADER_SIZE;
	size_t xor_len = payload_len - LTS_FEC_HEADER_SIZE;

	int missing_idx = -1;
	int missing_count = 0;
	for (int i = 0; i < count; i++) {
		if (!lookup(dec, start_seq + (uint16_t)i)) {
			missing_count++;
			missing_idx = i;
		}
	}

	if (missing_count != 1) {
		if (missing_count > 1)
			dec->unrecoverable++;
		return false;
	}

	uint8_t result_payload[LTS_FEC_MAX_XOR];
	size_t result_len = xor_len;
	if (result_len > sizeof(result_payload))
		result_len = sizeof(result_payload);
	memcpy(result_payload, xor_data, result_len);

	uint8_t recovered_flags = flags_xor;

	for (int i = 0; i < count; i++) {
		if (i == missing_idx)
			continue;
		const lts_fec_raw_slot_t *slot = lookup(dec, start_seq + (uint16_t)i);
		if (!slot)
			return false;
		size_t xl = slot->payload_len > result_len ? slot->payload_len : result_len;
		if (xl > sizeof(result_payload))
			xl = sizeof(result_payload);
		for (size_t j = 0; j < xl; j++) {
			uint8_t a = (j < result_len) ? result_payload[j] : 0;
			uint8_t b = (j < slot->payload_len) ? slot->payload[j] : 0;
			result_payload[j] = a ^ b;
		}
		result_len = xl;
		recovered_flags ^= slot->flags;
	}

	recovered->stream_id = stream_id;
	recovered->pkt_seq = start_seq + (uint16_t)missing_idx;
	recovered->flags = recovered_flags;
	recovered->payload = result_payload;
	recovered->payload_len = result_len;

	dec->recovered++;
	return true;
}
