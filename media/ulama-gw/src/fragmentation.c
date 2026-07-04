#include "ulama_gw/fragmentation.h"

#include <string.h>

size_t frag_split(const uint8_t *payload, size_t payload_len, uint8_t frag_payloads[][ULAMA_FRAME_MAX_PAYLOAD], size_t *frag_sizes, size_t max_frags)
{
	if (!payload || payload_len == 0 || !frag_payloads || !frag_sizes)
		return 0;

	size_t n = (payload_len + ULAMA_FRAME_MAX_PAYLOAD - 1) / ULAMA_FRAME_MAX_PAYLOAD;
	if (n > max_frags || n > FRAG_MAX_FRAGMENTS)
		return 0;

	size_t offset = 0;
	for (size_t i = 0; i < n; i++) {
		size_t chunk = payload_len - offset;
		if (chunk > ULAMA_FRAME_MAX_PAYLOAD)
			chunk = ULAMA_FRAME_MAX_PAYLOAD;
		memcpy(frag_payloads[i], payload + offset, chunk);
		frag_sizes[i] = chunk;
		offset += chunk;
	}

	return n;
}

void frag_reassembly_init(frag_reassembly_ctx_t *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
}

static frag_reassembly_slot_t *find_slot(frag_reassembly_ctx_t *ctx, uint8_t src_node, uint16_t seq)
{
	for (int i = 0; i < FRAG_MAX_SLOTS; i++) {
		frag_reassembly_slot_t *s = &ctx->slots[i];
		if (s->active && s->src_node == src_node && s->seq == seq)
			return s;
	}
	return NULL;
}

static frag_reassembly_slot_t *alloc_slot(frag_reassembly_ctx_t *ctx, uint8_t src_node, uint16_t seq, uint8_t frag_total, uint64_t now_ms)
{
	for (int i = 0; i < FRAG_MAX_SLOTS; i++) {
		if (!ctx->slots[i].active) {
			frag_reassembly_slot_t *s = &ctx->slots[i];
			memset(s, 0, sizeof(*s));
			s->active = true;
			s->src_node = src_node;
			s->seq = seq;
			s->frag_total = frag_total;
			s->first_ts_ms = now_ms;
			return s;
		}
	}
	uint64_t oldest_ts = UINT64_MAX;
	int oldest_idx = 0;
	for (int i = 0; i < FRAG_MAX_SLOTS; i++) {
		if (ctx->slots[i].first_ts_ms < oldest_ts) {
			oldest_ts = ctx->slots[i].first_ts_ms;
			oldest_idx = i;
		}
	}
	frag_reassembly_slot_t *s = &ctx->slots[oldest_idx];
	memset(s, 0, sizeof(*s));
	s->active = true;
	s->src_node = src_node;
	s->seq = seq;
	s->frag_total = frag_total;
	s->first_ts_ms = now_ms;
	return s;
}

bool frag_reassembly_insert(frag_reassembly_ctx_t *ctx, const ulama_frame_view_t *frame, uint64_t now_ms)
{
	if (!ctx || !frame)
		return false;
	if (!(frame->flags & ULAMA_FLAG_FRAGMENT))
		return false;
	if (frame->frag_idx >= frame->frag_total || frame->frag_total > FRAG_MAX_FRAGMENTS)
		return false;

	frag_reassembly_slot_t *slot = find_slot(ctx, frame->src_node, frame->seq);
	if (!slot)
		slot = alloc_slot(ctx, frame->src_node, frame->seq, frame->frag_total, now_ms);

	if (slot->received_mask & (1u << frame->frag_idx))
		return false;

	size_t copy_len = frame->payload_len;
	if (copy_len > ULAMA_FRAME_MAX_PAYLOAD)
		copy_len = ULAMA_FRAME_MAX_PAYLOAD;
	memcpy(slot->payload_bufs[frame->frag_idx], frame->payload, copy_len);
	slot->fragments[frame->frag_idx] = *frame;
	slot->fragments[frame->frag_idx].payload = slot->payload_bufs[frame->frag_idx];
	slot->fragments[frame->frag_idx].payload_len = copy_len;
	slot->received_mask |= (1u << frame->frag_idx);

	uint32_t expected_mask = (frame->frag_total == 32)
				 ? 0xFFFFFFFFu
				 : (1u << frame->frag_total) - 1u;
	return (slot->received_mask & expected_mask) == expected_mask;
}

bool frag_reassembly_complete(frag_reassembly_ctx_t *ctx, uint8_t src_node, uint16_t seq, uint8_t *out, size_t out_capacity, size_t *out_len)
{
	frag_reassembly_slot_t *slot = find_slot(ctx, src_node, seq);
	if (!slot)
		return false;

	uint32_t expected_mask = (slot->frag_total == 32)
				 ? 0xFFFFFFFFu
				 : (1u << slot->frag_total) - 1u;
	if ((slot->received_mask & expected_mask) != expected_mask)
		return false;

	size_t total = 0;
	for (uint8_t i = 0; i < slot->frag_total; i++)
		total += slot->fragments[i].payload_len;

	if (total > out_capacity)
		return false;

	size_t offset = 0;
	for (uint8_t i = 0; i < slot->frag_total; i++) {
		memcpy(out + offset, slot->fragments[i].payload, slot->fragments[i].payload_len);
		offset += slot->fragments[i].payload_len;
	}

	*out_len = total;
	slot->active = false;
	return true;
}

void frag_reassembly_expire(frag_reassembly_ctx_t *ctx, uint64_t now_ms)
{
	for (int i = 0; i < FRAG_MAX_SLOTS; i++) {
		frag_reassembly_slot_t *s = &ctx->slots[i];
		if (s->active && (now_ms - s->first_ts_ms) > FRAG_TIMEOUT_MS)
			s->active = false;
	}
}

frag_reassembly_slot_t *frag_reassembly_find_slot(frag_reassembly_ctx_t *ctx, uint8_t src_node, uint16_t seq)
{
	return find_slot(ctx, src_node, seq);
}

void frag_reassembly_flush_stale_video(frag_reassembly_ctx_t *ctx,
				       uint8_t src_node, uint16_t keyframe_seq)
{
	for (int i = 0; i < FRAG_MAX_SLOTS; i++) {
		frag_reassembly_slot_t *s = &ctx->slots[i];
		if (!s->active || s->src_node != src_node)
			continue;
		/* Flush all incomplete frames whose seq < keyframe_seq (sequence space wrap-safe) */
		int16_t delta = (int16_t)(keyframe_seq - s->seq);
		if (delta > 0)
			s->active = false;
	}
}
