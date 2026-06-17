#include "ulama/msp.h"

#include <string.h>

uint8_t msp_v1_checksum(const uint8_t *data, size_t len)
{
	uint8_t crc = 0;
	for (size_t i = 0; i < len; i++)
		crc ^= data[i];
	return crc;
}

uint8_t msp_v2_crc8_dvb_s2(const uint8_t *data, size_t len)
{
	uint8_t crc = 0;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int j = 0; j < 8; j++)
			crc = (crc & 0x80) ? ((crc << 1) ^ 0xD5) : (crc << 1);
	}
	return crc;
}

size_t msp_v1_build_request(uint16_t code, uint8_t *out, size_t out_capacity)
{
	if (out_capacity < MSP_V1_OVERHEAD || code > 255)
		return 0;

	out[0] = '$';
	out[1] = 'M';
	out[2] = '<';
	out[3] = 0;
	out[4] = (uint8_t)code;
	out[5] = msp_v1_checksum(out + 3, 2);

	return MSP_V1_OVERHEAD;
}

size_t msp_v2_build_request(uint16_t code, uint8_t *out, size_t out_capacity)
{
	if (out_capacity < MSP_V2_OVERHEAD)
		return 0;

	out[0] = '$';
	out[1] = 'X';
	out[2] = '<';
	out[3] = 0;
	out[4] = (uint8_t)(code & 0xFF);
	out[5] = (uint8_t)(code >> 8);
	out[6] = 0;
	out[7] = 0;
	out[8] = msp_v2_crc8_dvb_s2(out + 3, 5);

	return MSP_V2_OVERHEAD;
}

size_t msp_v1_build_response(uint16_t code, const uint8_t *payload, size_t payload_len,
			     uint8_t *out, size_t out_capacity)
{
	if (code > 255 || payload_len > 255)
		return 0;
	size_t total = MSP_V1_OVERHEAD + payload_len;
	if (out_capacity < total)
		return 0;

	out[0] = '$';
	out[1] = 'M';
	out[2] = '>';
	out[3] = (uint8_t)payload_len;
	out[4] = (uint8_t)code;
	if (payload_len > 0 && payload)
		memcpy(out + 5, payload, payload_len);

	out[total - 1] = msp_v1_checksum(out + 3, 2 + payload_len);
	return total;
}

size_t msp_v2_build_response(uint16_t code, const uint8_t *payload, size_t payload_len,
			     uint8_t *out, size_t out_capacity)
{
	if (payload_len > MSP_MAX_PAYLOAD)
		return 0;
	size_t total = MSP_V2_OVERHEAD + payload_len;
	if (out_capacity < total)
		return 0;

	out[0] = '$';
	out[1] = 'X';
	out[2] = '>';
	out[3] = 0;
	out[4] = (uint8_t)(code & 0xFF);
	out[5] = (uint8_t)(code >> 8);
	out[6] = (uint8_t)(payload_len & 0xFF);
	out[7] = (uint8_t)(payload_len >> 8);
	if (payload_len > 0 && payload)
		memcpy(out + 8, payload, payload_len);

	out[total - 1] = msp_v2_crc8_dvb_s2(out + 3, 5 + payload_len);
	return total;
}

int msp_parse(const uint8_t *buf, size_t buf_len, msp_message_t *out)
{
	if (!buf || !out || buf_len < 3)
		return 0;

	if (buf[0] != '$')
		return -1;

	if (buf[1] == 'M') {
		if (buf_len < MSP_V1_OVERHEAD)
			return 0;
		uint8_t dir = buf[2];
		uint8_t payload_len = buf[3];
		size_t total = MSP_V1_OVERHEAD + payload_len;
		if (buf_len < total)
			return 0;

		uint8_t expected_crc = msp_v1_checksum(buf + 3, 2 + payload_len);
		if (buf[total - 1] != expected_crc)
			return -1;

		out->version = MSP_VERSION_1;
		out->direction = (msp_direction_t)dir;
		out->flag = 0;
		out->code = buf[4];
		out->payload_len = payload_len;
		if (payload_len > 0)
			memcpy(out->payload, buf + 5, payload_len);

		return (int)total;
	}

	if (buf[1] == 'X') {
		if (buf_len < MSP_V2_OVERHEAD)
			return 0;
		uint8_t dir = buf[2];
		uint16_t payload_len = ((uint16_t)buf[7] << 8) | buf[6];
		if (payload_len > MSP_MAX_PAYLOAD)
			return -1;
		size_t total = MSP_V2_OVERHEAD + payload_len;
		if (buf_len < total)
			return 0;

		uint8_t expected_crc = msp_v2_crc8_dvb_s2(buf + 3, 5 + payload_len);
		if (buf[total - 1] != expected_crc)
			return -1;

		out->version = MSP_VERSION_2;
		out->direction = (msp_direction_t)dir;
		out->flag = buf[3];
		out->code = ((uint16_t)buf[5] << 8) | buf[4];
		out->payload_len = payload_len;
		if (payload_len > 0)
			memcpy(out->payload, buf + 8, payload_len);

		return (int)total;
	}

	return -1;
}

void msp_parser_init(msp_parser_t *parser)
{
	memset(parser, 0, sizeof(*parser));
	parser->state = MSP_PARSE_IDLE;
}

bool msp_parser_feed(msp_parser_t *parser, uint8_t byte, msp_message_t *out)
{
	switch (parser->state) {
	case MSP_PARSE_IDLE:
		if (byte == '$')
			parser->state = MSP_PARSE_PREAMBLE_1;
		return false;

	case MSP_PARSE_PREAMBLE_1:
		if (byte == 'M') {
			parser->msg.version = MSP_VERSION_1;
			parser->state = MSP_PARSE_V1_DIR;
		} else if (byte == 'X') {
			parser->msg.version = MSP_VERSION_2;
			parser->state = MSP_PARSE_V2_DIR;
		} else {
			parser->state = MSP_PARSE_IDLE;
		}
		return false;

	case MSP_PARSE_V1_DIR:
		parser->msg.direction = (msp_direction_t)byte;
		parser->state = MSP_PARSE_V1_LEN;
		return false;

	case MSP_PARSE_V1_LEN:
		parser->msg.payload_len = byte;
		parser->payload_idx = 0;
		parser->state = MSP_PARSE_V1_CODE;
		return false;

	case MSP_PARSE_V1_CODE:
		parser->msg.code = byte;
		parser->state = (parser->msg.payload_len > 0) ? MSP_PARSE_V1_PAYLOAD : MSP_PARSE_V1_CRC;
		return false;

	case MSP_PARSE_V1_PAYLOAD:
		if (parser->payload_idx < MSP_MAX_PAYLOAD)
			parser->msg.payload[parser->payload_idx] = byte;
		parser->payload_idx++;
		if (parser->payload_idx >= parser->msg.payload_len)
			parser->state = MSP_PARSE_V1_CRC;
		return false;

	case MSP_PARSE_V1_CRC: {
		uint8_t crc_data[2 + MSP_MAX_PAYLOAD];
		crc_data[0] = (uint8_t)parser->msg.payload_len;
		crc_data[1] = (uint8_t)parser->msg.code;
		if (parser->msg.payload_len > 0)
			memcpy(crc_data + 2, parser->msg.payload, parser->msg.payload_len);
		uint8_t expected = msp_v1_checksum(crc_data, 2 + parser->msg.payload_len);
		parser->state = MSP_PARSE_IDLE;
		if (byte == expected) {
			parser->msg.flag = 0;
			*out = parser->msg;
			return true;
		}
		return false;
	}

	/* V2: $X <dir> <flag> <code_lo> <code_hi> <len_lo> <len_hi> [payload] <crc8> */
	case MSP_PARSE_V2_DIR:
		parser->msg.direction = (msp_direction_t)byte;
		parser->state = MSP_PARSE_V2_FLAG;
		return false;

	case MSP_PARSE_V2_FLAG:
		parser->msg.flag = byte;
		parser->state = MSP_PARSE_V2_CODE_LO;
		return false;

	case MSP_PARSE_V2_CODE_LO:
		parser->msg.code = byte;
		parser->state = MSP_PARSE_V2_CODE_HI;
		return false;

	case MSP_PARSE_V2_CODE_HI:
		parser->msg.code |= ((uint16_t)byte << 8);
		parser->state = MSP_PARSE_V2_LEN_LO;
		return false;

	case MSP_PARSE_V2_LEN_LO:
		parser->msg.payload_len = byte;
		parser->state = MSP_PARSE_V2_LEN_HI;
		return false;

	case MSP_PARSE_V2_LEN_HI:
		parser->msg.payload_len |= ((uint16_t)byte << 8);
		parser->payload_idx = 0;
		if (parser->msg.payload_len > MSP_MAX_PAYLOAD) {
			parser->state = MSP_PARSE_IDLE;
			return false;
		}
		parser->state = (parser->msg.payload_len > 0) ? MSP_PARSE_V2_PAYLOAD : MSP_PARSE_V2_CRC;
		return false;

	case MSP_PARSE_V2_PAYLOAD:
		if (parser->payload_idx < MSP_MAX_PAYLOAD)
			parser->msg.payload[parser->payload_idx] = byte;
		parser->payload_idx++;
		if (parser->payload_idx >= parser->msg.payload_len)
			parser->state = MSP_PARSE_V2_CRC;
		return false;

	case MSP_PARSE_V2_CRC: {
		uint8_t crc_buf[5 + MSP_MAX_PAYLOAD];
		crc_buf[0] = parser->msg.flag;
		crc_buf[1] = (uint8_t)(parser->msg.code & 0xFF);
		crc_buf[2] = (uint8_t)(parser->msg.code >> 8);
		crc_buf[3] = (uint8_t)(parser->msg.payload_len & 0xFF);
		crc_buf[4] = (uint8_t)(parser->msg.payload_len >> 8);
		if (parser->msg.payload_len > 0)
			memcpy(crc_buf + 5, parser->msg.payload, parser->msg.payload_len);
		uint8_t expected = msp_v2_crc8_dvb_s2(crc_buf, 5 + parser->msg.payload_len);
		parser->state = MSP_PARSE_IDLE;
		if (byte == expected) {
			*out = parser->msg;
			return true;
		}
		return false;
	}

	default:
		parser->state = MSP_PARSE_IDLE;
		return false;
	}
}
