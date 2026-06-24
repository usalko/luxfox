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

static void test_nack_decode(void)
{
	uint8_t nack_wire[] = {0x4C, 0x4E, 0x01, 0x00, 0x64, 0x00, 0x05};
	expect_true(lts_enc_is_nack(nack_wire, sizeof(nack_wire)), "is_nack should detect NACK");

	lts_enc_nack_t nack;
	expect_true(lts_enc_decode_nack(nack_wire, sizeof(nack_wire), &nack), "decode should succeed");
	expect_true(nack.stream_id == 1, "NACK stream_id");
	expect_true(nack.start_seq == 100, "NACK start_seq");
	expect_true(nack.bitmask == 0x0005, "NACK bitmask");

	uint8_t not_nack[] = {0x00, 0x01, 0x02};
	expect_true(!lts_enc_is_nack(not_nack, sizeof(not_nack)), "non-NACK should not match");
	expect_true(!lts_enc_decode_nack(not_nack, sizeof(not_nack), &nack), "non-NACK decode should fail");
}

static void test_retx_buf_store_find(void)
{
	lts_retx_buf_t buf;
	lts_retx_buf_init(&buf);

	lts_encoder_t enc;
	lts_encoder_init(&enc, 0);

	uint8_t payload[100];
	memset(payload, 0xAA, sizeof(payload));

	lts_encoded_packet_t pkts[4];
	size_t n = lts_encoder_encode(&enc, payload, sizeof(payload), 0, pkts, 4);
	expect_true(n == 1, "retx: should produce 1 packet");

	lts_retx_buf_store(&buf, &pkts[0]);

	const lts_encoded_packet_t *found = lts_retx_buf_find(&buf, 0);
	expect_true(found != NULL, "retx: should find stored packet");
	expect_true(found->pkt_seq == 0, "retx: found packet seq should be 0");
	expect_true(found->len == pkts[0].len, "retx: found packet len should match");

	const lts_encoded_packet_t *missing = lts_retx_buf_find(&buf, 99);
	expect_true(missing == NULL, "retx: unstored seq should return NULL");
}

static void test_retx_buf_overwrite(void)
{
	lts_retx_buf_t buf;
	lts_retx_buf_init(&buf);

	lts_encoded_packet_t pkt1 = {.pkt_seq = 5, .len = 10};
	memset(pkt1.data, 0x11, pkt1.len);
	lts_retx_buf_store(&buf, &pkt1);

	lts_encoded_packet_t pkt2 = {.pkt_seq = 5 + LTS_RETX_SLOTS, .len = 20};
	memset(pkt2.data, 0x22, pkt2.len);
	lts_retx_buf_store(&buf, &pkt2);

	const lts_encoded_packet_t *found = lts_retx_buf_find(&buf, 5);
	expect_true(found == NULL, "retx: overwritten seq should not be found by old seq");

	found = lts_retx_buf_find(&buf, 5 + LTS_RETX_SLOTS);
	expect_true(found != NULL, "retx: new seq should be found");
	expect_true(found->len == 20, "retx: new packet len should match");
}

int main(void)
{
	test_single_encode();
	test_encoder_sequence();
	test_encoder_fragmentation();
	test_roundtrip_with_decoder();
	test_nack_decode();
	test_retx_buf_store_find();
	test_retx_buf_overwrite();

	if (failures > 0) {
		fprintf(stderr, "test_lts_encoder: %d failures\n", failures);
		return 1;
	}
	printf("test_lts_encoder: ok\n");
	return 0;
}
