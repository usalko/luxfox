#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ulama_gw/cascade_frame.h"

static int failures = 0;

static void expect_true(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		failures++;
	}
}

static void test_golden_vector(void)
{
	/* From cascade/docs/PROTOCOL.md:
	 * version=1, src=42, dst=7, class=TELEMETRY, payload="sim:seq=1"
	 * Hex: 01 00 2A 00 07 01 73 69 6D 3A 73 65 71 3D 31
	 */
	static const uint8_t expected[] = {
		0x01, 0x00, 0x2A, 0x00, 0x07, 0x01,
		0x73, 0x69, 0x6D, 0x3A, 0x73, 0x65, 0x71, 0x3D, 0x31,
	};
	const char *payload_str = "sim:seq=1";
	cascade_frame_view_t view = {
		.version = CASCADE_FRAME_VERSION,
		.src = 42,
		.dst = 7,
		.traffic_class = CASCADE_CLASS_TELEMETRY,
		.payload = (const uint8_t *)payload_str,
		.payload_len = 9,
	};

	uint8_t packed[64];
	size_t packed_len = 0;

	expect_true(cascade_frame_pack(&view, packed, sizeof(packed), &packed_len),
		    "pack should succeed");
	expect_true(packed_len == sizeof(expected),
		    "packed size should match golden vector");
	expect_true(memcmp(packed, expected, sizeof(expected)) == 0,
		    "packed bytes should match golden vector");

	cascade_frame_view_t unpacked;
	expect_true(cascade_frame_unpack(packed, packed_len, &unpacked),
		    "unpack should succeed");
	expect_true(unpacked.version == 1, "version should be 1");
	expect_true(unpacked.src == 42, "src should be 42");
	expect_true(unpacked.dst == 7, "dst should be 7");
	expect_true(unpacked.traffic_class == CASCADE_CLASS_TELEMETRY,
		    "class should be TELEMETRY");
	expect_true(unpacked.payload_len == 9, "payload len should be 9");
	expect_true(memcmp(unpacked.payload, payload_str, 9) == 0,
		    "payload should match");
}

static void test_roundtrip(void)
{
	static const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
	cascade_frame_view_t view = {
		.version = CASCADE_FRAME_VERSION,
		.src = 1000,
		.dst = 2000,
		.traffic_class = CASCADE_CLASS_VIDEO,
		.payload = payload,
		.payload_len = sizeof(payload),
	};

	uint8_t packed[64];
	size_t packed_len = 0;

	expect_true(cascade_frame_pack(&view, packed, sizeof(packed), &packed_len),
		    "roundtrip pack should succeed");

	cascade_frame_view_t unpacked;
	expect_true(cascade_frame_unpack(packed, packed_len, &unpacked),
		    "roundtrip unpack should succeed");
	expect_true(unpacked.version == view.version, "version roundtrip");
	expect_true(unpacked.src == view.src, "src roundtrip");
	expect_true(unpacked.dst == view.dst, "dst roundtrip");
	expect_true(unpacked.traffic_class == view.traffic_class, "class roundtrip");
	expect_true(unpacked.payload_len == view.payload_len, "payload_len roundtrip");
	expect_true(memcmp(unpacked.payload, payload, sizeof(payload)) == 0,
		    "payload roundtrip");
}

static void test_header_only(void)
{
	cascade_frame_view_t view = {
		.version = CASCADE_FRAME_VERSION,
		.src = 1,
		.dst = 2,
		.traffic_class = CASCADE_CLASS_CONTROL,
		.payload = NULL,
		.payload_len = 0,
	};

	uint8_t packed[8];
	size_t packed_len = 0;

	expect_true(cascade_frame_pack(&view, packed, sizeof(packed), &packed_len),
		    "header-only pack should succeed");
	expect_true(packed_len == CASCADE_FRAME_HEADER_SIZE,
		    "header-only size should be 6");

	cascade_frame_view_t unpacked;
	expect_true(cascade_frame_unpack(packed, packed_len, &unpacked),
		    "header-only unpack should succeed");
	expect_true(unpacked.payload_len == 0, "header-only payload should be empty");
}

static void test_too_short(void)
{
	uint8_t short_buf[5] = {1, 0, 1, 0, 2};
	cascade_frame_view_t out;
	expect_true(!cascade_frame_unpack(short_buf, sizeof(short_buf), &out),
		    "should reject frame shorter than header");
}

static void test_version_zero_normalize(void)
{
	cascade_frame_view_t view = {
		.version = 0,
		.src = 1,
		.dst = 1,
		.traffic_class = CASCADE_CLASS_CONTROL,
		.payload = NULL,
		.payload_len = 0,
	};
	uint8_t packed[8];
	size_t packed_len = 0;

	expect_true(cascade_frame_pack(&view, packed, sizeof(packed), &packed_len),
		    "version=0 pack should succeed");
	expect_true(packed[0] == CASCADE_FRAME_VERSION,
		    "version=0 should be normalized to 1");
}

int main(void)
{
	test_golden_vector();
	test_roundtrip();
	test_header_only();
	test_too_short();
	test_version_zero_normalize();

	if (failures > 0) {
		fprintf(stderr, "test_cascade_frame: %d failures\n", failures);
		return 1;
	}
	printf("test_cascade_frame: ok\n");
	return 0;
}
