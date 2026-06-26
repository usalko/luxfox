#include "ulama_gw/lts_fec_dec.h"

#include <string.h>

void lts_fec_decoder_init(lts_fec_decoder_t *dec)
{
	memset(dec, 0, sizeof(*dec));
}

static lts_fec_group_t *find_or_alloc_group(lts_fec_decoder_t *dec, uint16_t start_seq, uint8_t count)
{
	for (int i = 0; i < LTS_FEC_DEC_SLOTS; i++) {
		if (dec->groups[i].active &&
		    dec->groups[i].group_start_seq == start_seq &&
		    dec->groups[i].group_count == count)
			return &dec->groups[i];
	}
	for (int i = 0; i < LTS_FEC_DEC_SLOTS; i++) {
		if (!dec->groups[i].active) {
			memset(&dec->groups[i], 0, sizeof(dec->groups[i]));
			dec->groups[i].active = true;
			dec->groups[i].group_start_seq = start_seq;
			dec->groups[i].group_count = count;
			return &dec->groups[i];
		}
	}
	/* Evict oldest slot */
	lts_fec_group_t *oldest = &dec->groups[0];
	memset(oldest, 0, sizeof(*oldest));
	oldest->active = true;
	oldest->group_start_seq = start_seq;
	oldest->group_count = count;
	return oldest;
}

void lts_fec_decoder_add_data(lts_fec_decoder_t *dec, const lts_packet_t *pkt,
			      const uint8_t *raw, size_t raw_len)
{
	if (!dec || !pkt || !raw || raw_len == 0)
		return;

	for (int i = 0; i < LTS_FEC_DEC_SLOTS; i++) {
		lts_fec_group_t *g = &dec->groups[i];
		if (!g->active)
			continue;
		uint16_t offset = (uint16_t)(pkt->pkt_seq - g->group_start_seq);
		if (offset < g->group_count) {
			if (!(g->received_mask & (1 << offset))) {
				g->received_mask |= (1 << offset);
				size_t copy = raw_len;
				if (copy > sizeof(g->data[0]))
					copy = sizeof(g->data[0]);
				memcpy(g->data[offset], raw, copy);
				g->data_len[offset] = copy;
			}
		}
	}
}

static int popcount8(uint8_t v)
{
	int c = 0;
	while (v) { c += v & 1; v >>= 1; }
	return c;
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

	lts_fec_group_t *g = find_or_alloc_group(dec, start_seq, count);
	g->has_parity = true;
	size_t copy = xor_len;
	if (copy > sizeof(g->parity))
		copy = sizeof(g->parity);
	memcpy(g->parity, xor_data, copy);
	g->parity_len = copy;

	int received = popcount8(g->received_mask & ((1 << count) - 1));
	int missing = count - received;

	if (missing != 1) {
		if (missing == 0)
			g->active = false;
		else
			dec->unrecoverable++;
		return false;
	}

	int missing_idx = -1;
	for (int i = 0; i < count; i++) {
		if (!(g->received_mask & (1 << i))) {
			missing_idx = i;
			break;
		}
	}
	if (missing_idx < 0)
		return false;

	/* XOR all received packets with parity to recover missing */
	uint8_t result[LTS_HEADER_SIZE + LTS_MAX_PAYLOAD];
	size_t result_len = g->parity_len;
	memcpy(result, g->parity, result_len);

	for (int i = 0; i < count; i++) {
		if (i == missing_idx)
			continue;
		size_t xl = g->data_len[i] > result_len ? g->data_len[i] : result_len;
		for (size_t j = 0; j < xl; j++) {
			uint8_t a = (j < result_len) ? result[j] : 0;
			uint8_t b = (j < g->data_len[i]) ? g->data[i][j] : 0;
			result[j] = a ^ b;
		}
		if (xl > result_len)
			result_len = xl;
	}

	if (result_len < LTS_HEADER_SIZE) {
		dec->unrecoverable++;
		g->active = false;
		return false;
	}

	if (!lts_decode_packet(result, result_len, recovered)) {
		dec->unrecoverable++;
		g->active = false;
		return false;
	}

	dec->recovered++;
	g->active = false;
	return true;
}
