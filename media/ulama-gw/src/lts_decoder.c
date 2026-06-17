#include "ulama_gw/lts_decoder.h"

#include <string.h>

bool lts_decode_packet(const uint8_t *data, size_t len, lts_packet_t *out)
{
	if (!data || !out || len < LTS_HEADER_SIZE)
		return false;

	out->stream_id = data[0];
	out->pkt_seq = ((uint16_t)data[1] << 8) | data[2];
	out->flags = data[3];
	out->payload = (len > LTS_HEADER_SIZE) ? (data + LTS_HEADER_SIZE) : NULL;
	out->payload_len = len - LTS_HEADER_SIZE;

	return true;
}

size_t lts_encode_nack(const lts_nack_t *nack, uint8_t *out, size_t out_capacity)
{
	if (!nack || !out || out_capacity < LTS_NACK_SIZE)
		return 0;

	out[0] = LTS_NACK_MAGIC0;
	out[1] = LTS_NACK_MAGIC1;
	out[2] = nack->stream_id;
	out[3] = (uint8_t)(nack->start_seq >> 8);
	out[4] = (uint8_t)(nack->start_seq & 0xFF);
	out[5] = (uint8_t)(nack->bitmask >> 8);
	out[6] = (uint8_t)(nack->bitmask & 0xFF);

	return LTS_NACK_SIZE;
}

bool lts_decode_nack(const uint8_t *data, size_t len, lts_nack_t *out)
{
	if (!data || !out || len < LTS_NACK_SIZE)
		return false;
	if (data[0] != LTS_NACK_MAGIC0 || data[1] != LTS_NACK_MAGIC1)
		return false;

	out->stream_id = data[2];
	out->start_seq = ((uint16_t)data[3] << 8) | data[4];
	out->bitmask = ((uint16_t)data[5] << 8) | data[6];

	return true;
}

bool lts_is_nack(const uint8_t *data, size_t len)
{
	return len >= 2 && data[0] == LTS_NACK_MAGIC0 && data[1] == LTS_NACK_MAGIC1;
}

void lts_decoder_init(lts_decoder_t *dec, int window_size, uint64_t emit_deadline_ms)
{
	memset(dec, 0, sizeof(*dec));
	dec->window_size = (window_size > 0 && window_size <= LTS_REORDER_WINDOW) ? window_size : LTS_REORDER_WINDOW;
	dec->emit_deadline_ms = (emit_deadline_ms > 0) ? emit_deadline_ms : LTS_EMIT_DEADLINE_MS;
	dec->first_packet = true;
}

static int slot_index(const lts_decoder_t *dec, uint16_t seq)
{
	return seq % dec->window_size;
}

bool lts_decoder_insert(lts_decoder_t *dec, const lts_packet_t *pkt, uint64_t now_ms)
{
	if (!dec || !pkt)
		return false;

	int idx = slot_index(dec, pkt->pkt_seq);
	lts_reorder_slot_t *slot = &dec->slots[idx];

	if (slot->occupied && slot->pkt_seq == pkt->pkt_seq)
		return true;

	if (dec->first_packet) {
		dec->next_emit = pkt->pkt_seq;
		dec->first_packet = false;
	}

	size_t copy_len = pkt->payload_len;
	if (copy_len > LTS_MAX_PAYLOAD)
		copy_len = LTS_MAX_PAYLOAD;

	slot->pkt_seq = pkt->pkt_seq;
	slot->flags = pkt->flags;
	slot->len = copy_len;
	if (copy_len > 0 && pkt->payload)
		memcpy(slot->data, pkt->payload, copy_len);
	slot->occupied = true;
	slot->recv_ts_ms = now_ms;
	slot->deadline_ms = now_ms + dec->emit_deadline_ms;

	dec->last_received = pkt->pkt_seq;

	return false;
}

size_t lts_decoder_emit(lts_decoder_t *dec, lts_packet_t *out, size_t max_out, uint64_t now_ms)
{
	if (!dec || !out || max_out == 0)
		return 0;

	size_t count = 0;

	while (count < max_out) {
		int idx = slot_index(dec, dec->next_emit);
		lts_reorder_slot_t *slot = &dec->slots[idx];

		if (slot->occupied && slot->pkt_seq == dec->next_emit) {
			out[count].stream_id = 0;
			out[count].pkt_seq = slot->pkt_seq;
			out[count].flags = slot->flags;
			out[count].payload = slot->data;
			out[count].payload_len = slot->len;
			slot->occupied = false;
			dec->next_emit++;
			count++;
			continue;
		}

		if (!lts_seq_lt(dec->next_emit, dec->last_received))
			break;

		bool deadline_expired = false;
		if (slot->occupied && slot->pkt_seq != dec->next_emit) {
			deadline_expired = (now_ms >= slot->deadline_ms);
		} else {
			for (uint16_t s = dec->next_emit; lts_seq_le(s, dec->last_received); s++) {
				int si = slot_index(dec, s);
				lts_reorder_slot_t *check = &dec->slots[si];
				if (check->occupied && check->pkt_seq == s) {
					deadline_expired = (now_ms >= check->deadline_ms);
					break;
				}
			}
		}

		if (deadline_expired) {
			dec->next_emit++;
			continue;
		}

		break;
	}

	return count;
}

size_t lts_decoder_detect_gaps(lts_decoder_t *dec, uint16_t *gaps, size_t max_gaps)
{
	if (!dec || !gaps || max_gaps == 0)
		return 0;

	size_t count = 0;
	for (uint16_t seq = dec->next_emit; lts_seq_le(seq, dec->last_received) && count < max_gaps; seq++) {
		int idx = slot_index(dec, seq);
		lts_reorder_slot_t *slot = &dec->slots[idx];
		if (!slot->occupied || slot->pkt_seq != seq)
			gaps[count++] = seq;
	}

	return count;
}
