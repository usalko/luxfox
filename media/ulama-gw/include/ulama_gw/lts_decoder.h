#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LTS_HEADER_SIZE 4
#define LTS_MAX_PAYLOAD 2281

#define LTS_FLAG_LAST_OF_FRAME  (1 << 0)
#define LTS_FLAG_KEYFRAME       (1 << 1)
#define LTS_FLAG_RETX           (1 << 2)
#define LTS_FLAG_FIRST_OF_FRAME (1 << 4)

#define LTS_NACK_MAGIC0 0x4C
#define LTS_NACK_MAGIC1 0x4E
#define LTS_NACK_SIZE   7

#define LTS_REORDER_WINDOW 128
#define LTS_EMIT_DEADLINE_MS 200

typedef struct {
	uint8_t stream_id;
	uint16_t pkt_seq;
	uint8_t flags;
	const uint8_t *payload;
	size_t payload_len;
} lts_packet_t;

typedef struct {
	uint8_t stream_id;
	uint16_t start_seq;
	uint16_t bitmask;
} lts_nack_t;

typedef struct {
	uint8_t data[LTS_MAX_PAYLOAD];
	size_t len;
	uint16_t pkt_seq;
	uint8_t flags;
	bool occupied;
	uint64_t recv_ts_ms;
	uint64_t deadline_ms;
} lts_reorder_slot_t;

typedef struct {
	lts_reorder_slot_t slots[LTS_REORDER_WINDOW];
	uint16_t next_emit;
	uint16_t last_received;
	bool first_packet;
	int window_size;
	uint64_t emit_deadline_ms;
} lts_decoder_t;

bool lts_decode_packet(const uint8_t *data, size_t len, lts_packet_t *out);
size_t lts_encode_nack(const lts_nack_t *nack, uint8_t *out, size_t out_capacity);
bool lts_decode_nack(const uint8_t *data, size_t len, lts_nack_t *out);
bool lts_is_nack(const uint8_t *data, size_t len);

void lts_decoder_init(lts_decoder_t *dec, int window_size, uint64_t emit_deadline_ms);
bool lts_decoder_insert(lts_decoder_t *dec, const lts_packet_t *pkt, uint64_t now_ms);
size_t lts_decoder_emit(lts_decoder_t *dec, lts_packet_t *out, size_t max_out, uint64_t now_ms);
size_t lts_decoder_detect_gaps(lts_decoder_t *dec, uint16_t *gaps, size_t max_gaps);

static inline bool lts_seq_lt(uint16_t a, uint16_t b) { return (int16_t)(a - b) < 0; }
static inline bool lts_seq_le(uint16_t a, uint16_t b) { return (int16_t)(a - b) <= 0; }
