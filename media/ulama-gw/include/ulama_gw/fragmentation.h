#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ulama/ulama_frame.h"

#define FRAG_MAX_FRAGMENTS 16
#define FRAG_MAX_SLOTS 8
#define FRAG_TIMEOUT_MS 200
#define FRAG_MAX_REASSEMBLED (ULAMA_FRAME_MAX_PAYLOAD * FRAG_MAX_FRAGMENTS)

typedef struct {
	ulama_frame_view_t fragments[FRAG_MAX_FRAGMENTS];
	uint8_t payload_bufs[FRAG_MAX_FRAGMENTS][ULAMA_FRAME_MAX_PAYLOAD];
	uint8_t received_mask;
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
bool frag_reassembly_insert(frag_reassembly_ctx_t *ctx, const ulama_frame_view_t *frame, uint64_t now_ms);
bool frag_reassembly_complete(frag_reassembly_ctx_t *ctx, uint8_t src_node, uint16_t seq, uint8_t *out, size_t out_capacity, size_t *out_len);
void frag_reassembly_expire(frag_reassembly_ctx_t *ctx, uint64_t now_ms);
