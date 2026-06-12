#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ULAMA_CRSF_ADDRESS_FLIGHT_CONTROLLER 0xC8
#define ULAMA_CRSF_FRAME_TYPE_RC_CHANNELS_PACKED 0x16
#define ULAMA_CRSF_NUM_CHANNELS 16
#define ULAMA_CRSF_RC_PAYLOAD_SIZE 22
#define ULAMA_CRSF_RC_FRAME_SIZE 26
#define ULAMA_CRSF_LEN_RC_CHANNELS 24

#define ULAMA_CRSF_VALUE_MIN 172U
#define ULAMA_CRSF_VALUE_MID 992U
#define ULAMA_CRSF_VALUE_MAX 1811U

void ulama_crsf_channels_init(uint16_t channels[ULAMA_CRSF_NUM_CHANNELS]);
uint16_t ulama_crsf_clamp(uint16_t value);
uint16_t ulama_crsf_axis_to_bipolar(int value, bool invert);
uint16_t ulama_crsf_axis_to_throttle(int value, bool invert);
uint8_t ulama_crsf_crc8_dvb_s2(const uint8_t *data, size_t len);
size_t ulama_crsf_build_rc_channels_frame(uint8_t address,
	const uint16_t channels[ULAMA_CRSF_NUM_CHANNELS],
	uint8_t out[ULAMA_CRSF_RC_FRAME_SIZE]);
bool ulama_crsf_parse_rc_channels_frame(const uint8_t *frame,
	size_t len,
	uint8_t *out_address,
	uint16_t channels[ULAMA_CRSF_NUM_CHANNELS]);