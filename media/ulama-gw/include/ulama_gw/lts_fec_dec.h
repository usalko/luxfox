#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ulama_gw/lts_decoder.h"

#define LTS_FEC_DEC_MAX_GROUP 8
#define LTS_FEC_DEC_SLOTS 4
#define LTS_FEC_HEADER_SIZE 3
#define LTS_FLAG_FEC (1 << 3)

typedef struct {
	uint16_t group_start_seq;
	uint8_t group_count;
	uint8_t received_mask;
	uint8_t data[LTS_FEC_DEC_MAX_GROUP][LTS_HEADER_SIZE + LTS_MAX_PAYLOAD];
	size_t data_len[LTS_FEC_DEC_MAX_GROUP];
	uint8_t parity[LTS_HEADER_SIZE + LTS_MAX_PAYLOAD];
	size_t parity_len;
	bool has_parity;
	bool active;
} lts_fec_group_t;

typedef struct {
	lts_fec_group_t groups[LTS_FEC_DEC_SLOTS];
	uint32_t recovered;
	uint32_t unrecoverable;
} lts_fec_decoder_t;

void lts_fec_decoder_init(lts_fec_decoder_t *dec);

void lts_fec_decoder_add_data(lts_fec_decoder_t *dec, const lts_packet_t *pkt,
			      const uint8_t *raw, size_t raw_len);

bool lts_fec_decoder_add_parity(lts_fec_decoder_t *dec, const uint8_t *payload,
				size_t payload_len, lts_packet_t *recovered);
