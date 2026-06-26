#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ulama_gw/lts_decoder.h"

#define LTS_FEC_DEC_MAX_GROUP 8
#define LTS_FEC_HEADER_SIZE 4
#define LTS_FLAG_FEC (1 << 3)

#define LTS_FEC_MAX_XOR (LTS_MAX_PAYLOAD - LTS_FEC_HEADER_SIZE)
#define LTS_FEC_RAW_RING_SIZE 64

typedef struct {
	uint8_t payload[LTS_FEC_MAX_XOR];
	size_t payload_len;
	uint8_t flags;
	uint16_t seq;
	bool valid;
} lts_fec_raw_slot_t;

typedef struct {
	lts_fec_raw_slot_t ring[LTS_FEC_RAW_RING_SIZE];
	uint32_t recovered;
	uint32_t unrecoverable;
} lts_fec_decoder_t;

void lts_fec_decoder_init(lts_fec_decoder_t *dec);

void lts_fec_decoder_add_data(lts_fec_decoder_t *dec, uint16_t seq,
			      uint8_t flags, const uint8_t *lts_payload,
			      size_t lts_payload_len);

bool lts_fec_decoder_add_parity(lts_fec_decoder_t *dec, uint8_t stream_id,
				const uint8_t *payload, size_t payload_len,
				lts_packet_t *recovered);
