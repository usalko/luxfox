#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ulama_gw/lts_decoder.h"

static int failures = 0;

static void expect_true(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		failures++;
	}
}

static void test_packet_codec(void)
{
	uint8_t wire[] = {0x01, 0x00, 0x2A, 0x03, 0xDE, 0xAD};
	lts_packet_t pkt;
	expect_true(lts_decode_packet(wire, sizeof(wire), &pkt), "decode should succeed");
	expect_true(pkt.stream_id == 1, "stream_id should be 1");
	expect_true(pkt.pkt_seq == 42, "pkt_seq should be 42");
	expect_true(pkt.flags == 0x03, "flags should be 0x03");
	expect_true(pkt.payload_len == 2, "payload should be 2 bytes");
	expect_true(pkt.payload[0] == 0xDE && pkt.payload[1] == 0xAD, "payload content");
}

static void test_nack_codec(void)
{
	lts_nack_t nack = {.stream_id = 1, .start_seq = 100, .bitmask = 0x0005};
	uint8_t buf[16];
	size_t n = lts_encode_nack(&nack, buf, sizeof(buf));
	expect_true(n == LTS_NACK_SIZE, "NACK encode size should be 7");
	expect_true(buf[0] == LTS_NACK_MAGIC0, "NACK magic0");
	expect_true(buf[1] == LTS_NACK_MAGIC1, "NACK magic1");
	expect_true(lts_is_nack(buf, n), "is_nack should detect NACK");

	lts_nack_t decoded;
	expect_true(lts_decode_nack(buf, n, &decoded), "NACK decode should succeed");
	expect_true(decoded.stream_id == 1, "NACK stream_id roundtrip");
	expect_true(decoded.start_seq == 100, "NACK start_seq roundtrip");
	expect_true(decoded.bitmask == 0x0005, "NACK bitmask roundtrip");
}

static void test_reorder_in_order(void)
{
	lts_decoder_t dec;
	lts_decoder_init(&dec, 16, 80);

	uint8_t data_a[] = {0xAA};
	uint8_t data_b[] = {0xBB};
	uint8_t data_c[] = {0xCC};

	lts_packet_t pkts[3] = {
		{.stream_id = 0, .pkt_seq = 0, .flags = 0, .payload = data_a, .payload_len = 1},
		{.stream_id = 0, .pkt_seq = 1, .flags = 0, .payload = data_b, .payload_len = 1},
		{.stream_id = 0, .pkt_seq = 2, .flags = 0, .payload = data_c, .payload_len = 1},
	};

	for (int i = 0; i < 3; i++)
		lts_decoder_insert(&dec, &pkts[i], 1000);

	lts_packet_t out[8];
	size_t n = lts_decoder_emit(&dec, out, 8, 1000);
	expect_true(n == 3, "in-order should emit 3 packets");
	expect_true(out[0].pkt_seq == 0, "first emit seq=0");
	expect_true(out[1].pkt_seq == 1, "second emit seq=1");
	expect_true(out[2].pkt_seq == 2, "third emit seq=2");
}

static void test_reorder_out_of_order(void)
{
	lts_decoder_t dec;
	lts_decoder_init(&dec, 16, 80);

	uint8_t data[] = {0xFF};
	lts_packet_t pkt0 = {.stream_id = 0, .pkt_seq = 0, .flags = 0, .payload = data, .payload_len = 1};
	lts_packet_t pkt1 = {.stream_id = 0, .pkt_seq = 1, .flags = 0, .payload = data, .payload_len = 1};
	lts_packet_t pkt2 = {.stream_id = 0, .pkt_seq = 2, .flags = 0, .payload = data, .payload_len = 1};

	lts_decoder_insert(&dec, &pkt0, 1000);
	lts_decoder_insert(&dec, &pkt2, 1000);

	lts_packet_t out[8];
	size_t n = lts_decoder_emit(&dec, out, 8, 1000);
	expect_true(n == 1, "should emit seq=0 only (seq=1 missing)");
	expect_true(out[0].pkt_seq == 0, "emitted should be seq=0");

	lts_decoder_insert(&dec, &pkt1, 1001);
	n = lts_decoder_emit(&dec, out, 8, 1001);
	expect_true(n == 2, "after seq=1 arrives, should emit 2 packets");
	expect_true(out[0].pkt_seq == 1, "first emit seq=1");
	expect_true(out[1].pkt_seq == 2, "second emit seq=2");
}

static void test_dedup(void)
{
	lts_decoder_t dec;
	lts_decoder_init(&dec, 16, 80);

	uint8_t data[] = {0x11};
	lts_packet_t pkt = {.stream_id = 0, .pkt_seq = 0, .flags = 0, .payload = data, .payload_len = 1};

	bool dup1 = lts_decoder_insert(&dec, &pkt, 1000);
	expect_true(!dup1, "first insert should not be duplicate");
	bool dup2 = lts_decoder_insert(&dec, &pkt, 1001);
	expect_true(dup2, "second insert should be duplicate");
}

static void test_gap_detection(void)
{
	lts_decoder_t dec;
	lts_decoder_init(&dec, 16, 80);

	uint8_t data[] = {0x00};
	lts_packet_t pkt0 = {.stream_id = 0, .pkt_seq = 10, .flags = 0, .payload = data, .payload_len = 1};
	lts_packet_t pkt3 = {.stream_id = 0, .pkt_seq = 13, .flags = 0, .payload = data, .payload_len = 1};

	lts_decoder_insert(&dec, &pkt0, 1000);
	lts_decoder_insert(&dec, &pkt3, 1001);

	lts_packet_t out[8];
	lts_decoder_emit(&dec, out, 8, 1001);

	uint16_t gaps[16];
	size_t ngaps = lts_decoder_detect_gaps(&dec, gaps, 16);
	expect_true(ngaps == 2, "should detect 2 gaps (seq 11, 12)");
	if (ngaps >= 2) {
		expect_true(gaps[0] == 11, "first gap should be seq=11");
		expect_true(gaps[1] == 12, "second gap should be seq=12");
	}
}

static void test_deadline_skip(void)
{
	lts_decoder_t dec;
	lts_decoder_init(&dec, 16, 80);

	uint8_t data[] = {0x00};
	lts_packet_t pkt0 = {.stream_id = 0, .pkt_seq = 0, .flags = 0, .payload = data, .payload_len = 1};
	lts_packet_t pkt2 = {.stream_id = 0, .pkt_seq = 2, .flags = 0, .payload = data, .payload_len = 1};

	lts_decoder_insert(&dec, &pkt0, 1000);
	lts_decoder_insert(&dec, &pkt2, 1000);

	lts_packet_t out[8];
	size_t n = lts_decoder_emit(&dec, out, 8, 1000);
	expect_true(n == 1, "should emit seq=0, block on missing seq=1");

	n = lts_decoder_emit(&dec, out, 8, 1100);
	expect_true(n == 1, "after deadline, should skip seq=1 and emit seq=2");
	expect_true(out[0].pkt_seq == 2, "emitted should be seq=2");
}

static void test_nack_bitmask_from_gaps(void)
{
	lts_decoder_t dec;
	lts_decoder_init(&dec, 32, 80);

	uint8_t data[] = {0x00};
	lts_packet_t pkt0 = {.stream_id = 0, .pkt_seq = 10, .flags = 0, .payload = data, .payload_len = 1};
	lts_packet_t pkt5 = {.stream_id = 0, .pkt_seq = 15, .flags = 0, .payload = data, .payload_len = 1};

	lts_decoder_insert(&dec, &pkt0, 1000);
	lts_decoder_insert(&dec, &pkt5, 1001);

	lts_packet_t out[8];
	lts_decoder_emit(&dec, out, 8, 1001);

	uint16_t gaps[16];
	size_t ngaps = lts_decoder_detect_gaps(&dec, gaps, 16);
	expect_true(ngaps == 4, "should detect 4 gaps (seq 11,12,13,14)");

	if (ngaps > 0) {
		lts_nack_t nack;
		nack.stream_id = 0;
		nack.start_seq = gaps[0];
		nack.bitmask = 0;
		for (size_t i = 0; i < ngaps; i++) {
			uint16_t offset = gaps[i] - gaps[0];
			if (offset < 16)
				nack.bitmask |= (uint16_t)(1 << offset);
		}

		expect_true(nack.start_seq == 11, "NACK start_seq should be 11");
		expect_true(nack.bitmask == 0x000F, "NACK bitmask should cover 4 consecutive gaps (0b1111)");

		uint8_t wire[16];
		size_t n = lts_encode_nack(&nack, wire, sizeof(wire));
		expect_true(n == LTS_NACK_SIZE, "NACK encode should produce 7 bytes");

		lts_nack_t decoded;
		expect_true(lts_decode_nack(wire, n, &decoded), "NACK roundtrip decode");
		expect_true(decoded.start_seq == 11, "NACK roundtrip start_seq");
		expect_true(decoded.bitmask == 0x000F, "NACK roundtrip bitmask");
	}
}

static void test_retx_restores_nal(void)
{
	lts_decoder_t dec;
	lts_decoder_init(&dec, 32, 80);

	uint8_t data_a[] = {0xAA};
	uint8_t data_b[] = {0xBB};
	uint8_t data_c[] = {0xCC};

	lts_packet_t pkt0 = {.stream_id = 0, .pkt_seq = 0, .flags = 0, .payload = data_a, .payload_len = 1};
	lts_packet_t pkt2 = {.stream_id = 0, .pkt_seq = 2, .flags = LTS_FLAG_LAST_OF_FRAME, .payload = data_c, .payload_len = 1};

	lts_decoder_insert(&dec, &pkt0, 1000);
	lts_decoder_insert(&dec, &pkt2, 1001);

	lts_packet_t out[8];
	size_t n = lts_decoder_emit(&dec, out, 8, 1001);
	expect_true(n == 1, "before retx: should emit seq=0 only");

	uint16_t gaps[16];
	size_t ngaps = lts_decoder_detect_gaps(&dec, gaps, 16);
	expect_true(ngaps == 1, "should detect 1 gap (seq=1)");
	expect_true(gaps[0] == 1, "gap should be seq=1");

	lts_packet_t pkt1 = {.stream_id = 0, .pkt_seq = 1, .flags = 0, .payload = data_b, .payload_len = 1};
	lts_decoder_insert(&dec, &pkt1, 1010);

	n = lts_decoder_emit(&dec, out, 8, 1010);
	expect_true(n == 2, "after retx: should emit seq=1 and seq=2");
	expect_true(out[0].pkt_seq == 1, "first emitted should be seq=1");
	expect_true(out[1].pkt_seq == 2, "second emitted should be seq=2");
	expect_true(out[1].flags & LTS_FLAG_LAST_OF_FRAME, "seq=2 should have LAST_OF_FRAME");
}

int main(void)
{
	test_packet_codec();
	test_nack_codec();
	test_reorder_in_order();
	test_reorder_out_of_order();
	test_dedup();
	test_gap_detection();
	test_deadline_skip();
	test_nack_bitmask_from_gaps();
	test_retx_restores_nal();

	if (failures > 0) {
		fprintf(stderr, "test_lts_decoder: %d failures\n", failures);
		return 1;
	}
	printf("test_lts_decoder: ok\n");
	return 0;
}
