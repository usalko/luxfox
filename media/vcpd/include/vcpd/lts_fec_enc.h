#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vcpd/lts_encoder.h"

#define LTS_FEC_MAX_GROUP 8
#define LTS_FEC_HEADER_SIZE 3
#define LTS_ENC_FLAG_FEC (1 << 3)

typedef struct {
	uint8_t xor_buf[LTS_ENC_HEADER_SIZE + LTS_ENC_MAX_PAYLOAD];
	size_t max_len;
	uint16_t group_start_seq;
	int count;
	int group_size;
} lts_fec_encoder_t;

void lts_fec_encoder_init(lts_fec_encoder_t *fec, int group_size);

bool lts_fec_encoder_add(lts_fec_encoder_t *fec, const lts_encoded_packet_t *pkt,
			 lts_encoder_t *enc, lts_encoded_packet_t *fec_out);
