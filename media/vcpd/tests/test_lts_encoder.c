#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vcpd/lts_encoder.h"

static int failures = 0;

static void expect_true(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		failures++;
	}
}

static void test_single_encode(void)
{
	uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
	uint8_t out[32];
	size_t n = lts_encode_single(1, 42, LTS_ENC_FLAG_KEYFRAME, payload, sizeof(payload), out, sizeof(out));
	expect_true(n == LTS_ENC_HEADER_SIZE + sizeof(payload), "encode size should be 4+4=8");
	expect_true(out[0] == 1, "stream_id should be 1");
	expect_true(out[1] == 0 && out[2] == 42, "pkt_seq should be 42 (BE)");
	expect_true(out[3] == LTS_ENC_FLAG_KEYFRAME, "flags should be KEYFRAME");
	expect_true(out[4] == 0xDE && out[5] == 0xAD, "payload content");
}

static void test_encoder_sequence(void)
{
	lts_encoder_t enc;
	lts_encoder_init(&enc, 0);

	uint8_t payload[100];
	memset(payload, 0xAA, sizeof(payload));

	lts_encoded_packet_t pkts[4];
	size_t n = lts_encoder_encode(&enc, payload, sizeof(payload), 0, pkts, 4);
	expect_true(n == 1, "100B payload should produce 1 LTS packet");
	expect_true(pkts[0].pkt_seq == 0, "first packet seq should be 0");
	expect_true(pkts[0].len == LTS_ENC_HEADER_SIZE + sizeof(payload), "packet size");

	n = lts_encoder_encode(&enc, payload, sizeof(payload), 0, pkts, 4);
	expect_true(pkts[0].pkt_seq == 1, "second packet seq should be 1");
}

static void test_encoder_fragmentation(void)
{
	lts_encoder_t enc;
	lts_encoder_init(&enc, 0);

	uint8_t payload[LTS_ENC_MAX_PAYLOAD * 2 + 50];
	memset(payload, 0xBB, sizeof(payload));

	lts_encoded_packet_t pkts[8];
	size_t n = lts_encoder_encode(&enc, payload, sizeof(payload), 0, pkts, 8);
	expect_true(n == 3, "payload exceeding 2*max should produce 3 LTS packets");

	size_t total_payload = 0;
	for (size_t i = 0; i < n; i++)
		total_payload += (pkts[i].len - LTS_ENC_HEADER_SIZE);
	expect_true(total_payload == sizeof(payload), "total payload should match input");

	expect_true(pkts[n - 1].data[3] & LTS_ENC_FLAG_LAST_OF_FRAME,
		    "last packet should have LAST_OF_FRAME flag");
}

static void test_roundtrip_with_decoder(void)
{
	uint8_t payload[] = {0x47, 0x00, 0x11, 0x10};
	uint8_t wire[32];
	size_t n = lts_encode_single(1, 100, LTS_ENC_FLAG_LAST_OF_FRAME, payload, sizeof(payload), wire, sizeof(wire));
	expect_true(n == 8, "encoded size should be 8");

	expect_true(wire[0] == 1, "roundtrip stream_id");
	uint16_t seq = ((uint16_t)wire[1] << 8) | wire[2];
	expect_true(seq == 100, "roundtrip pkt_seq");
	expect_true(wire[3] == LTS_ENC_FLAG_LAST_OF_FRAME, "roundtrip flags");
	expect_true(memcmp(wire + 4, payload, sizeof(payload)) == 0, "roundtrip payload");
}

int main(void)
{
	test_single_encode();
	test_encoder_sequence();
	test_encoder_fragmentation();
	test_roundtrip_with_decoder();

	if (failures > 0) {
		fprintf(stderr, "test_lts_encoder: %d failures\n", failures);
		return 1;
	}
	printf("test_lts_encoder: ok\n");
	return 0;
}
