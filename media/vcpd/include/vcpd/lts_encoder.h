#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LTS_ENC_HEADER_SIZE 4
#define LTS_ENC_MAX_PAYLOAD 2281

#define LTS_ENC_FLAG_LAST_OF_FRAME (1 << 0)
#define LTS_ENC_FLAG_KEYFRAME      (1 << 1)
#define LTS_ENC_FLAG_RETX          (1 << 2)

#define LTS_ENC_NACK_MAGIC0 0x4C
#define LTS_ENC_NACK_MAGIC1 0x4E
#define LTS_ENC_NACK_SIZE   7

#define LTS_RETX_SLOTS 512

typedef struct {
	uint8_t stream_id;
	uint16_t next_seq;
	size_t max_payload;
} lts_encoder_t;

typedef struct {
	uint8_t data[LTS_ENC_HEADER_SIZE + LTS_ENC_MAX_PAYLOAD];
	size_t len;
	uint16_t pkt_seq;
} lts_encoded_packet_t;

typedef struct {
	uint8_t stream_id;
	uint16_t start_seq;
	uint16_t bitmask;
} lts_enc_nack_t;

typedef struct {
	lts_encoded_packet_t packets[LTS_RETX_SLOTS];
	bool valid[LTS_RETX_SLOTS];
} lts_retx_buf_t;

void lts_encoder_init(lts_encoder_t *enc, uint8_t stream_id);

size_t lts_encoder_encode(lts_encoder_t *enc, const uint8_t *payload, size_t payload_len,
			  uint8_t flags, lts_encoded_packet_t *out, size_t max_out);

size_t lts_encode_single(uint8_t stream_id, uint16_t pkt_seq, uint8_t flags,
			 const uint8_t *payload, size_t payload_len,
			 uint8_t *out, size_t out_capacity);

bool lts_enc_is_nack(const uint8_t *data, size_t len);
bool lts_enc_decode_nack(const uint8_t *data, size_t len, lts_enc_nack_t *out);

void lts_retx_buf_init(lts_retx_buf_t *buf);
void lts_retx_buf_store(lts_retx_buf_t *buf, const lts_encoded_packet_t *pkt);
const lts_encoded_packet_t *lts_retx_buf_find(const lts_retx_buf_t *buf, uint16_t seq);
