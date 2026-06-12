#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ulama/crsf.h"
#include "ulama/ulama_frame.h"

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
	uint8_t crsf[ULAMA_CRSF_RC_FRAME_SIZE];
	uint8_t ulama[ULAMA_FRAME_HEADER_SIZE + ULAMA_CRSF_RC_FRAME_SIZE];
	ulama_frame_view_t frame_view;
	uint8_t address = 0;
	size_t ulama_len = 0;
	int rc = 0;

	ulama_crsf_channels_init(channels);
	channels[0] = ULAMA_CRSF_VALUE_MAX;
	channels[1] = ULAMA_CRSF_VALUE_MIN;
	channels[2] = 1300;
	channels[3] = ULAMA_CRSF_VALUE_MID;
	channels[4] = ULAMA_CRSF_VALUE_MAX;

	rc |= expect_true(ulama_crsf_build_rc_channels_frame(ULAMA_CRSF_ADDRESS_FLIGHT_CONTROLLER, channels, crsf) == ULAMA_CRSF_RC_FRAME_SIZE,
		"crsf build should succeed");

	frame_view.src_node = 10;
	frame_view.dst_node = 1;
	frame_view.flags = 0;
	frame_view.traffic_class = ULAMA_CLASS_CTRL;
	frame_view.seq = 7;
	frame_view.frag_idx = 0;
	frame_view.frag_total = 1;
	frame_view.ttl = ULAMA_FRAME_ONE_HOP_TTL;
	frame_view.payload = crsf;
	frame_view.payload_len = sizeof(crsf);

	rc |= expect_true(ulama_frame_pack(&frame_view, ulama, sizeof(ulama), &ulama_len), "ulama frame pack should succeed");
	rc |= expect_true(ulama_frame_unpack(ulama, ulama_len, &frame_view), "ulama frame unpack should succeed");
	rc |= expect_true(frame_view.traffic_class == ULAMA_CLASS_CTRL, "traffic class should stay CTRL");
	rc |= expect_true(frame_view.dst_node == 1, "dst node should stay intact");
	rc |= expect_true(ulama_crsf_parse_rc_channels_frame(frame_view.payload, frame_view.payload_len, &address, decoded), "crsf payload should still parse");
	rc |= expect_true(address == ULAMA_CRSF_ADDRESS_FLIGHT_CONTROLLER, "crsf address should survive bridge");
	rc |= expect_true(memcmp(channels, decoded, sizeof(channels)) == 0, "channel values should survive bridge");

	if (rc != 0) {
		return 1;
	}
	printf("test_ulama_bridge: ok\n");
	return 0;
}