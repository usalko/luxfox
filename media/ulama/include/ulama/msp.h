#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MSP v1 (Multiwii Serial Protocol) */
#define MSP_V1_PREAMBLE_0    '$'
#define MSP_V1_PREAMBLE_1    'M'
#define MSP_V1_DIRECTION_IN  '<'
#define MSP_V1_DIRECTION_OUT '>'
#define MSP_V1_DIRECTION_ERR '!'
#define MSP_V1_HEADER_SIZE   5
#define MSP_V1_OVERHEAD      6

/* MSP v2 */
#define MSP_V2_PREAMBLE_0    '$'
#define MSP_V2_PREAMBLE_1    'X'
#define MSP_V2_HEADER_SIZE   8
#define MSP_V2_OVERHEAD      9

/* Common message codes */
#define MSP_STATUS         101
#define MSP_RAW_IMU        102
#define MSP_RAW_GPS        106
#define MSP_ATTITUDE       108
#define MSP_ALTITUDE       109
#define MSP_ANALOG         110
#define MSP_BATTERY_STATE  130

#define MSP_MAX_PAYLOAD    256

typedef enum {
	MSP_VERSION_1 = 1,
	MSP_VERSION_2 = 2,
} msp_version_t;

typedef enum {
	MSP_DIR_REQUEST  = '<',
	MSP_DIR_RESPONSE = '>',
	MSP_DIR_ERROR    = '!',
} msp_direction_t;

typedef struct {
	msp_version_t version;
	msp_direction_t direction;
	uint8_t  flag;
	uint16_t code;
	uint8_t  payload[MSP_MAX_PAYLOAD];
	uint16_t payload_len;
} msp_message_t;

/* CRC */
uint8_t msp_v1_checksum(const uint8_t *data, size_t len);
uint8_t msp_v2_crc8_dvb_s2(const uint8_t *data, size_t len);

/* Build request (for polling FC) */
size_t msp_v1_build_request(uint16_t code, uint8_t *out, size_t out_capacity);
size_t msp_v2_build_request(uint16_t code, uint8_t *out, size_t out_capacity);

/* Build response (for testing) */
size_t msp_v1_build_response(uint16_t code, const uint8_t *payload, size_t payload_len,
			     uint8_t *out, size_t out_capacity);
size_t msp_v2_build_response(uint16_t code, const uint8_t *payload, size_t payload_len,
			     uint8_t *out, size_t out_capacity);

/* Parse (returns consumed bytes, 0 if incomplete, -1 if invalid) */
int msp_parse(const uint8_t *buf, size_t buf_len, msp_message_t *out);

/* Stream parser for byte-at-a-time reception from UART */
typedef enum {
	MSP_PARSE_IDLE,
	MSP_PARSE_PREAMBLE_1,
	/* V1: $M <dir> <len> <code> [payload] <xor_crc> */
	MSP_PARSE_V1_DIR,
	MSP_PARSE_V1_LEN,
	MSP_PARSE_V1_CODE,
	MSP_PARSE_V1_PAYLOAD,
	MSP_PARSE_V1_CRC,
	/* V2: $X <dir> <flag> <code_lo> <code_hi> <len_lo> <len_hi> [payload] <crc8> */
	MSP_PARSE_V2_DIR,
	MSP_PARSE_V2_FLAG,
	MSP_PARSE_V2_CODE_LO,
	MSP_PARSE_V2_CODE_HI,
	MSP_PARSE_V2_LEN_LO,
	MSP_PARSE_V2_LEN_HI,
	MSP_PARSE_V2_PAYLOAD,
	MSP_PARSE_V2_CRC,
} msp_parse_state_t;

typedef struct {
	msp_parse_state_t state;
	msp_message_t msg;
	uint16_t payload_idx;
} msp_parser_t;

void msp_parser_init(msp_parser_t *parser);
bool msp_parser_feed(msp_parser_t *parser, uint8_t byte, msp_message_t *out);
