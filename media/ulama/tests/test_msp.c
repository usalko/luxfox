#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ulama/msp.h"

static int failures = 0;

static void expect_true(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		failures++;
	}
}

static void test_v1_checksum(void)
{
	uint8_t data[] = {0x00, 0x6C};
	uint8_t crc = msp_v1_checksum(data, 2);
	expect_true(crc == 0x6C, "V1 checksum of [0x00, 0x6C] should be 0x6C");
}

static void test_v1_request_build(void)
{
	uint8_t buf[16];
	size_t n = msp_v1_build_request(MSP_ATTITUDE, buf, sizeof(buf));
	expect_true(n == MSP_V1_OVERHEAD, "V1 request should be 6 bytes");
	expect_true(buf[0] == '$', "preamble[0]");
	expect_true(buf[1] == 'M', "preamble[1]");
	expect_true(buf[2] == '<', "direction");
	expect_true(buf[3] == 0, "payload_len = 0");
	expect_true(buf[4] == MSP_ATTITUDE, "code = MSP_ATTITUDE");
}

static void test_v1_response_roundtrip(void)
{
	uint8_t payload[] = {0x10, 0x27, 0xF0, 0xD8, 0x2C, 0x01};
	uint8_t buf[32];
	size_t n = msp_v1_build_response(MSP_ATTITUDE, payload, sizeof(payload), buf, sizeof(buf));
	expect_true(n == MSP_V1_OVERHEAD + sizeof(payload), "V1 response size");

	msp_message_t msg;
	int consumed = msp_parse(buf, n, &msg);
	expect_true(consumed == (int)n, "parse should consume all bytes");
	expect_true(msg.version == MSP_VERSION_1, "version should be V1");
	expect_true(msg.direction == MSP_DIR_RESPONSE, "direction should be response");
	expect_true(msg.code == MSP_ATTITUDE, "code should be MSP_ATTITUDE");
	expect_true(msg.payload_len == sizeof(payload), "payload_len roundtrip");
	expect_true(memcmp(msg.payload, payload, sizeof(payload)) == 0, "payload roundtrip");
}

static void test_v2_crc8(void)
{
	uint8_t data[] = {0x00, 0x6C, 0x00, 0x06, 0x00, 0x10, 0x27, 0xF0, 0xD8, 0x2C, 0x01};
	uint8_t crc = msp_v2_crc8_dvb_s2(data, sizeof(data));
	expect_true(crc != 0, "V2 CRC should be non-zero for non-zero data");
}

static void test_v2_request_build(void)
{
	uint8_t buf[16];
	size_t n = msp_v2_build_request(MSP_ATTITUDE, buf, sizeof(buf));
	expect_true(n == MSP_V2_OVERHEAD, "V2 request should be 9 bytes");
	expect_true(buf[0] == '$', "preamble[0]");
	expect_true(buf[1] == 'X', "preamble[1]");
	expect_true(buf[2] == '<', "direction");
	expect_true(buf[3] == 0, "flag");
	expect_true(buf[4] == (MSP_ATTITUDE & 0xFF), "code_lo");
	expect_true(buf[5] == (MSP_ATTITUDE >> 8), "code_hi");
	expect_true(buf[6] == 0, "len_lo");
	expect_true(buf[7] == 0, "len_hi");
}

static void test_v2_response_roundtrip(void)
{
	uint8_t payload[] = {0x10, 0x27, 0xF0, 0xD8, 0x2C, 0x01};
	uint8_t buf[32];
	size_t n = msp_v2_build_response(MSP_ATTITUDE, payload, sizeof(payload), buf, sizeof(buf));
	expect_true(n == MSP_V2_OVERHEAD + sizeof(payload), "V2 response size");

	msp_message_t msg;
	int consumed = msp_parse(buf, n, &msg);
	expect_true(consumed == (int)n, "V2 parse should consume all bytes");
	expect_true(msg.version == MSP_VERSION_2, "version should be V2");
	expect_true(msg.direction == MSP_DIR_RESPONSE, "direction should be response");
	expect_true(msg.code == MSP_ATTITUDE, "code should be MSP_ATTITUDE");
	expect_true(msg.payload_len == sizeof(payload), "V2 payload_len roundtrip");
	expect_true(memcmp(msg.payload, payload, sizeof(payload)) == 0, "V2 payload roundtrip");
}

static void test_v1_stream_parser(void)
{
	uint8_t payload[] = {0xAA, 0xBB};
	uint8_t buf[16];
	size_t n = msp_v1_build_response(MSP_ANALOG, payload, sizeof(payload), buf, sizeof(buf));

	msp_parser_t parser;
	msp_parser_init(&parser);
	msp_message_t out;
	bool complete = false;

	for (size_t i = 0; i < n; i++) {
		complete = msp_parser_feed(&parser, buf[i], &out);
		if (i < n - 1)
			expect_true(!complete, "V1 stream: should not complete before last byte");
	}

	expect_true(complete, "V1 stream: should complete on last byte");
	expect_true(out.version == MSP_VERSION_1, "V1 stream: version");
	expect_true(out.code == MSP_ANALOG, "V1 stream: code");
	expect_true(out.payload_len == 2, "V1 stream: payload_len");
	expect_true(out.payload[0] == 0xAA && out.payload[1] == 0xBB, "V1 stream: payload");
}

static void test_v2_stream_parser(void)
{
	uint8_t payload[] = {0x11, 0x22, 0x33};
	uint8_t buf[32];
	size_t n = msp_v2_build_response(MSP_BATTERY_STATE, payload, sizeof(payload), buf, sizeof(buf));

	msp_parser_t parser;
	msp_parser_init(&parser);
	msp_message_t out;
	bool complete = false;

	for (size_t i = 0; i < n; i++) {
		complete = msp_parser_feed(&parser, buf[i], &out);
		if (i < n - 1)
			expect_true(!complete, "V2 stream: should not complete before last byte");
	}

	expect_true(complete, "V2 stream: should complete on last byte");
	expect_true(out.version == MSP_VERSION_2, "V2 stream: version");
	expect_true(out.code == MSP_BATTERY_STATE, "V2 stream: code");
	expect_true(out.payload_len == 3, "V2 stream: payload_len");
	expect_true(out.payload[0] == 0x11, "V2 stream: payload[0]");
}

static void test_v1_no_payload_stream(void)
{
	uint8_t buf[16];
	size_t n = msp_v1_build_request(MSP_STATUS, buf, sizeof(buf));

	msp_parser_t parser;
	msp_parser_init(&parser);
	msp_message_t out;
	bool complete = false;

	for (size_t i = 0; i < n; i++)
		complete = msp_parser_feed(&parser, buf[i], &out);

	expect_true(complete, "V1 no-payload: should complete");
	expect_true(out.code == MSP_STATUS, "V1 no-payload: code");
	expect_true(out.payload_len == 0, "V1 no-payload: payload_len");
}

static void test_v2_no_payload_stream(void)
{
	uint8_t buf[16];
	size_t n = msp_v2_build_request(MSP_RAW_GPS, buf, sizeof(buf));

	msp_parser_t parser;
	msp_parser_init(&parser);
	msp_message_t out;
	bool complete = false;

	for (size_t i = 0; i < n; i++)
		complete = msp_parser_feed(&parser, buf[i], &out);

	expect_true(complete, "V2 no-payload: should complete");
	expect_true(out.code == MSP_RAW_GPS, "V2 no-payload: code");
	expect_true(out.payload_len == 0, "V2 no-payload: payload_len");
}

int main(void)
{
	test_v1_checksum();
	test_v1_request_build();
	test_v1_response_roundtrip();
	test_v2_crc8();
	test_v2_request_build();
	test_v2_response_roundtrip();
	test_v1_stream_parser();
	test_v2_stream_parser();
	test_v1_no_payload_stream();
	test_v2_no_payload_stream();

	if (failures > 0) {
		fprintf(stderr, "test_msp: %d failures\n", failures);
		return 1;
	}
	printf("test_msp: ok\n");
	return 0;
}
