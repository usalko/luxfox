#include "ulama/crsf.h"

#include <string.h>

static void pack_11bit_channels(const uint16_t channels[ULAMA_CRSF_NUM_CHANNELS], uint8_t out[ULAMA_CRSF_RC_PAYLOAD_SIZE])
{
	unsigned int bit_index = 0;

	memset(out, 0, ULAMA_CRSF_RC_PAYLOAD_SIZE);
	for (size_t channel = 0; channel < ULAMA_CRSF_NUM_CHANNELS; channel++) {
		uint16_t value = ulama_crsf_clamp(channels[channel]);
		for (unsigned int bit = 0; bit < 11U; bit++, bit_index++) {
			if ((value & (1U << bit)) != 0U) {
				out[bit_index / 8U] |= (uint8_t)(1U << (bit_index % 8U));
			}
		}
	}
}

static void unpack_11bit_channels(const uint8_t in[ULAMA_CRSF_RC_PAYLOAD_SIZE], uint16_t channels[ULAMA_CRSF_NUM_CHANNELS])
{
	unsigned int bit_index = 0;

	for (size_t channel = 0; channel < ULAMA_CRSF_NUM_CHANNELS; channel++) {
		uint16_t value = 0;
		for (unsigned int bit = 0; bit < 11U; bit++, bit_index++) {
			if ((in[bit_index / 8U] & (uint8_t)(1U << (bit_index % 8U))) != 0U) {
				value |= (uint16_t)(1U << bit);
			}
		}
		channels[channel] = value;
	}
}

void ulama_crsf_channels_init(uint16_t channels[ULAMA_CRSF_NUM_CHANNELS])
{
	if (channels == NULL) {
		return;
	}
	for (size_t index = 0; index < ULAMA_CRSF_NUM_CHANNELS; index++) {
		channels[index] = ULAMA_CRSF_VALUE_MID;
	}
	channels[2] = ULAMA_CRSF_VALUE_MIN;
	for (size_t index = 4; index < ULAMA_CRSF_NUM_CHANNELS; index++) {
		channels[index] = ULAMA_CRSF_VALUE_MIN;
	}
}

uint16_t ulama_crsf_clamp(uint16_t value)
{
	if (value < ULAMA_CRSF_VALUE_MIN) {
		return ULAMA_CRSF_VALUE_MIN;
	}
	if (value > ULAMA_CRSF_VALUE_MAX) {
		return ULAMA_CRSF_VALUE_MAX;
	}
	return value;
}

uint16_t ulama_crsf_axis_to_bipolar(int value, bool invert)
{
	long scaled;

	if (invert) {
		value = -value;
	}
	if (value < -32767) {
		value = -32767;
	}
	if (value > 32767) {
		value = 32767;
	}
	scaled = (long)ULAMA_CRSF_VALUE_MID + ((long)value * (long)(ULAMA_CRSF_VALUE_MAX - ULAMA_CRSF_VALUE_MID)) / 32767L;
	if (scaled < (long)ULAMA_CRSF_VALUE_MIN) {
		scaled = (long)ULAMA_CRSF_VALUE_MIN;
	}
	if (scaled > (long)ULAMA_CRSF_VALUE_MAX) {
		scaled = (long)ULAMA_CRSF_VALUE_MAX;
	}
	return (uint16_t)scaled;
}

uint16_t ulama_crsf_axis_to_throttle(int value, bool invert)
{
	long scaled;

	if (invert) {
		value = -value;
	}
	if (value < -32767) {
		value = -32767;
	}
	if (value > 32767) {
		value = 32767;
	}
	scaled = (long)ULAMA_CRSF_VALUE_MIN + ((long)(value + 32767) * (long)(ULAMA_CRSF_VALUE_MAX - ULAMA_CRSF_VALUE_MIN)) / 65534L;
	if (scaled < (long)ULAMA_CRSF_VALUE_MIN) {
		scaled = (long)ULAMA_CRSF_VALUE_MIN;
	}
	if (scaled > (long)ULAMA_CRSF_VALUE_MAX) {
		scaled = (long)ULAMA_CRSF_VALUE_MAX;
	}
	return (uint16_t)scaled;
}

uint8_t ulama_crsf_crc8_dvb_s2(const uint8_t *data, size_t len)
{
	uint8_t crc = 0;

	for (size_t index = 0; index < len; index++) {
		crc ^= data[index];
		for (unsigned int bit = 0; bit < 8U; bit++) {
			crc = (crc & 0x80U) != 0U ? (uint8_t)((crc << 1U) ^ 0xD5U) : (uint8_t)(crc << 1U);
		}
	}

	return crc;
}

size_t ulama_crsf_build_rc_channels_frame(uint8_t address,
	const uint16_t channels[ULAMA_CRSF_NUM_CHANNELS],
	uint8_t out[ULAMA_CRSF_RC_FRAME_SIZE])
{
	if (channels == NULL || out == NULL) {
		return 0;
	}

	out[0] = address;
	out[1] = ULAMA_CRSF_LEN_RC_CHANNELS;
	out[2] = ULAMA_CRSF_FRAME_TYPE_RC_CHANNELS_PACKED;
	pack_11bit_channels(channels, &out[3]);
	out[ULAMA_CRSF_RC_FRAME_SIZE - 1U] = ulama_crsf_crc8_dvb_s2(&out[2], 1U + ULAMA_CRSF_RC_PAYLOAD_SIZE);
	return ULAMA_CRSF_RC_FRAME_SIZE;
}

bool ulama_crsf_parse_rc_channels_frame(const uint8_t *frame,
	size_t len,
	uint8_t *out_address,
	uint16_t channels[ULAMA_CRSF_NUM_CHANNELS])
{
	uint8_t crc;

	if (frame == NULL || channels == NULL || len < ULAMA_CRSF_RC_FRAME_SIZE) {
		return false;
	}
	if (frame[1] != ULAMA_CRSF_LEN_RC_CHANNELS) {
		return false;
	}
	if (frame[2] != ULAMA_CRSF_FRAME_TYPE_RC_CHANNELS_PACKED) {
		return false;
	}
	crc = ulama_crsf_crc8_dvb_s2(&frame[2], 1U + ULAMA_CRSF_RC_PAYLOAD_SIZE);
	if (crc != frame[ULAMA_CRSF_RC_FRAME_SIZE - 1U]) {
		return false;
	}
	if (out_address != NULL) {
		*out_address = frame[0];
	}
	unpack_11bit_channels(&frame[3], channels);
	return true;
}

/* ---- CRSF telemetry stream parser ---- */

static bool crsf_is_telem_address(uint8_t b)
{
	return b == ULAMA_CRSF_ADDRESS_RADIO_TRANSMITTER ||
	       b == ULAMA_CRSF_ADDRESS_RECEIVER ||
	       b == ULAMA_CRSF_ADDRESS_FLIGHT_CONTROLLER;
}

void crsf_telem_parser_init(crsf_telem_parser_t *p)
{
	if (p == NULL) return;
	p->state = CRSF_TP_IDLE;
	p->pos = 0;
	p->frame_len = 0;
}

bool crsf_telem_parser_feed(crsf_telem_parser_t *p, uint8_t byte,
			    uint8_t *out_buf, uint8_t *out_len)
{
	if (p == NULL) return false;

	switch (p->state) {
	case CRSF_TP_IDLE:
		if (crsf_is_telem_address(byte)) {
			p->buf[0] = byte;
			p->pos = 1;
			p->state = CRSF_TP_LEN;
		}
		return false;

	case CRSF_TP_LEN:
		if (byte < 2 || byte > ULAMA_CRSF_MAX_FRAME_SIZE - 2) {
			p->state = CRSF_TP_IDLE;
			return false;
		}
		p->buf[1] = byte;
		p->frame_len = byte;
		p->pos = 2;
		p->state = CRSF_TP_DATA;
		return false;

	case CRSF_TP_DATA: {
		p->buf[p->pos++] = byte;
		uint8_t total = 2 + p->frame_len; /* addr + len + (type+payload+crc) */
		if (p->pos < total)
			return false;

		/* Frame complete — verify CRC (over type+payload, excluding addr/len/crc) */
		uint8_t crc = ulama_crsf_crc8_dvb_s2(&p->buf[2], p->frame_len - 1);
		p->state = CRSF_TP_IDLE;

		if (crc != p->buf[total - 1])
			return false;

		/* Skip RC frames — those are handled separately */
		if (p->buf[2] == ULAMA_CRSF_FRAME_TYPE_RC_CHANNELS_PACKED)
			return false;

		if (out_buf != NULL)
			__builtin_memcpy(out_buf, p->buf, total);
		if (out_len != NULL)
			*out_len = total;
		return true;
	}

	default:
		p->state = CRSF_TP_IDLE;
		return false;
	}
}