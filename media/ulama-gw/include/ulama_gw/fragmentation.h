#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ulama/ulama_frame.h"

#define FRAG_MAX_FRAGMENTS 29
#define FRAG_MAX_SLOTS 32
#define FRAG_TIMEOUT_MS 800
#define FRAG_MAX_REASSEMBLED 65536   /* = VIDEO_RING_SLOT_MAX, one whole H.265 frame */

typedef struct {
	ulama_frame_view_t fragments[FRAG_MAX_FRAGMENTS];
	uint8_t payload_bufs[FRAG_MAX_FRAGMENTS][ULAMA_FRAME_MAX_PAYLOAD];
	uint32_t received_mask;    /* bits 0..28 for up to 29 fragments */
	/* TX-side slot-phase decile (0-9) per received fragment, or 0xFF=N/A;
	 * see ULAMA_TX_PHASE_NA in ulama/transport.h. Only meaningful where
	 * received_mask has the corresponding bit set. Lets the gateway
	 * correlate loss with position in the sender's TX window. */
	uint8_t tx_phase[FRAG_MAX_FRAGMENTS];
	uint8_t src_node;
	uint16_t seq;
	uint8_t frag_total;
	uint64_t first_ts_ms;
	bool active;
} frag_reassembly_slot_t;

typedef struct {
	frag_reassembly_slot_t slots[FRAG_MAX_SLOTS];
} frag_reassembly_ctx_t;

size_t frag_split(const uint8_t *payload, size_t payload_len, uint8_t frag_payloads[][ULAMA_FRAME_MAX_PAYLOAD], size_t *frag_sizes, size_t max_frags);

void frag_reassembly_init(frag_reassembly_ctx_t *ctx);
bool frag_reassembly_insert(frag_reassembly_ctx_t *ctx, const ulama_frame_view_t *frame, uint8_t tx_phase, uint64_t now_ms);
bool frag_reassembly_complete(frag_reassembly_ctx_t *ctx, uint8_t src_node, uint16_t seq, uint8_t *out, size_t out_capacity, size_t *out_len);
void frag_reassembly_expire(frag_reassembly_ctx_t *ctx, uint64_t now_ms);

/* Find reassembly slot for given source and sequence for debugging */
frag_reassembly_slot_t *frag_reassembly_find_slot(frag_reassembly_ctx_t *ctx, uint8_t src_node, uint16_t seq);

/*
 * Expire all incomplete reassembly slots for src_node that are older than
 * the given seq. Call this when a KEYFRAME from src_node is fully assembled,
 * to discard stale incomplete P-frames and free slots immediately.
 */
void frag_reassembly_flush_stale_video(frag_reassembly_ctx_t *ctx,
				       uint8_t src_node, uint16_t keyframe_seq);
