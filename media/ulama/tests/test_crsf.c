#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ulama/crsf.h"

static int expect_true(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}
	return 0;
}

int main(void)
{
	uint16_t channels[ULAMA_CRSF_NUM_CHANNELS];
	uint16_t decoded[ULAMA_CRSF_NUM_CHANNELS];
	uint8_t frame[ULAMA_CRSF_RC_FRAME_SIZE];
	uint8_t address = 0;
	int rc = 0;

	ulama_crsf_channels_init(channels);
	channels[0] = 1811;
	channels[1] = 172;
	channels[2] = 1200;
	channels[3] = 992;
	channels[4] = 1811;
	channels[5] = 300;

	rc |= expect_true(ulama_crsf_build_rc_channels_frame(ULAMA_CRSF_ADDRESS_FLIGHT_CONTROLLER, channels, frame) == ULAMA_CRSF_RC_FRAME_SIZE,
		"crsf frame size should be 26 bytes");
	rc |= expect_true(frame[0] == ULAMA_CRSF_ADDRESS_FLIGHT_CONTROLLER, "address should be flight controller");
	rc |= expect_true(frame[1] == ULAMA_CRSF_LEN_RC_CHANNELS, "length should match rc channels payload");
	rc |= expect_true(frame[2] == ULAMA_CRSF_FRAME_TYPE_RC_CHANNELS_PACKED, "frame type should be rc channels packed");
	rc |= expect_true(ulama_crsf_parse_rc_channels_frame(frame, sizeof(frame), &address, decoded), "crsf parse should succeed");
	rc |= expect_true(address == ULAMA_CRSF_ADDRESS_FLIGHT_CONTROLLER, "decoded address should match");
	rc |= expect_true(memcmp(channels, decoded, sizeof(channels)) == 0, "decoded channels should roundtrip");

	if (rc != 0) {
		return 1;
	}
	printf("test_crsf: ok\n");
	return 0;
}