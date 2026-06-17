#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LTS_ENC_HEADER_SIZE 4
#define LTS_ENC_MAX_PAYLOAD 236

#define LTS_ENC_FLAG_LAST_OF_FRAME (1 << 0)
#define LTS_ENC_FLAG_KEYFRAME      (1 << 1)
#define LTS_ENC_FLAG_RETX          (1 << 2)

typedef struct {
	uint8_t stream_id;
	uint16_t next_seq;
} lts_encoder_t;

typedef struct {
	uint8_t data[LTS_ENC_HEADER_SIZE + LTS_ENC_MAX_PAYLOAD];
	size_t len;
	uint16_t pkt_seq;
} lts_encoded_packet_t;

void lts_encoder_init(lts_encoder_t *enc, uint8_t stream_id);

size_t lts_encoder_encode(lts_encoder_t *enc, const uint8_t *payload, size_t payload_len,
			  uint8_t flags, lts_encoded_packet_t *out, size_t max_out);

size_t lts_encode_single(uint8_t stream_id, uint16_t pkt_seq, uint8_t flags,
			 const uint8_t *payload, size_t payload_len,
			 uint8_t *out, size_t out_capacity);
