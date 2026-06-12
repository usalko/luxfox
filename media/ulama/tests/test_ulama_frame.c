#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
	static const uint8_t payload[] = {0x43, 0x52, 0x53, 0x46, 0x01, 0x02, 0x03};
	static const uint8_t expected[] = {
		0xA5, 0x02, 0x11, 0x22, 0x80, 0x00, 0x34, 0x12, 0x00, 0x01, 0x01, 0x00, 0xD6, 0xE4,
		0x43, 0x52, 0x53, 0x46, 0x01, 0x02, 0x03,
	};
	ulama_frame_view_t view = {
		.src_node = 0x11,
		.dst_node = 0x22,
		.flags = ULAMA_FLAG_DUP_ALLOWED,
		.traffic_class = ULAMA_CLASS_CTRL,
		.seq = 0x1234,
		.frag_idx = 0,
		.frag_total = 1,
		.ttl = ULAMA_FRAME_ONE_HOP_TTL,
		.payload = payload,
		.payload_len = sizeof(payload),
	};
	ulama_frame_view_t unpacked;
	uint8_t packed[ULAMA_FRAME_HEADER_SIZE + sizeof(payload)] = {0};
	size_t packed_len = 0;
	int rc = 0;

	rc |= expect_true(ulama_frame_pack(&view, packed, sizeof(packed), &packed_len), "frame pack should succeed");
	rc |= expect_true(packed_len == sizeof(expected), "packed size should match expected");
	rc |= expect_true(memcmp(packed, expected, sizeof(expected)) == 0, "packed bytes should match expected vector");
	rc |= expect_true(ulama_frame_unpack(packed, packed_len, &unpacked), "frame unpack should succeed");
	rc |= expect_true(unpacked.src_node == view.src_node, "src node should roundtrip");
	rc |= expect_true(unpacked.dst_node == view.dst_node, "dst node should roundtrip");
	rc |= expect_true(unpacked.flags == view.flags, "flags should roundtrip");
	rc |= expect_true(unpacked.traffic_class == view.traffic_class, "class should roundtrip");
	rc |= expect_true(unpacked.seq == view.seq, "seq should roundtrip");
	rc |= expect_true(unpacked.frag_idx == view.frag_idx, "frag idx should roundtrip");
	rc |= expect_true(unpacked.frag_total == view.frag_total, "frag total should roundtrip");
	rc |= expect_true(unpacked.ttl == view.ttl, "ttl should roundtrip");
	rc |= expect_true(unpacked.payload_len == view.payload_len, "payload len should roundtrip");
	rc |= expect_true(memcmp(unpacked.payload, payload, sizeof(payload)) == 0, "payload bytes should roundtrip");

	if (rc != 0) {
		return 1;
	}

	printf("test_ulama_frame: ok\n");
	return 0;
}