#include "radiod/sync_frame.h"

#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_tests;

static void check(int ok, const char *expr, const char *file, int line)
{
	g_tests++;
	if (ok)
		return;
	g_failures++;
	fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, expr);
}

#define CHECK(cond) check(!!(cond), #cond, __FILE__, __LINE__)

static void test_sync_pack_unpack_roundtrip(void)
{
	sync_frame_t in = {
		.master_node_id = 5,
		.sender_node_id = 3,
		.superframe_seq = 42,
		.master_time_us = 1234567890LL,
		.dl_duration_us = 2000,
		.ul_slot_us = 5000,
		.guard_us = 300,
		.num_slots = 3,
		.relay_hops = 1,
		.slot_map = {1, 2, 3, 0},
		.bootstrap_window_us = 4000,
		.bootstrap_period = 8,
	};
	uint8_t buf[SYNC_FRAME_MAX_SIZE];
	size_t len = 0;

	CHECK(sync_frame_pack(&in, buf, sizeof(buf), &len));
	CHECK(len == SYNC_FRAME_SIZE);

	sync_frame_t out;
	memset(&out, 0xFF, sizeof(out));
	CHECK(sync_frame_unpack(buf, len, &out));
	CHECK(out.master_node_id == 5);
	CHECK(out.sender_node_id == 3);
	CHECK(out.superframe_seq == 42);
	CHECK(out.master_time_us == 1234567890LL);
	CHECK(out.dl_duration_us == 2000);
	CHECK(out.ul_slot_us == 5000);
	CHECK(out.guard_us == 300);
	CHECK(out.num_slots == 3);
	CHECK(out.relay_hops == 1);
	CHECK(out.slot_map[0] == 1);
	CHECK(out.slot_map[1] == 2);
	CHECK(out.slot_map[2] == 3);
	CHECK(out.slot_map[3] == 0);
	CHECK(out.bootstrap_window_us == 4000);
	CHECK(out.bootstrap_period == 8);
}

static void test_sync_unpack_short_buffer(void)
{
	uint8_t buf[SYNC_FRAME_SIZE - 1];
	memset(buf, 0, sizeof(buf));
	buf[0] = SYNC_FRAME_MAGIC;
	buf[1] = SYNC_FRAME_VERSION;

	sync_frame_t out;
	CHECK(!sync_frame_unpack(buf, sizeof(buf), &out));
	CHECK(!sync_frame_unpack(buf, 0, &out));
	CHECK(!sync_frame_unpack(buf, 1, &out));
}

static void test_sync_unpack_bad_magic(void)
{
	uint8_t buf[SYNC_FRAME_SIZE] = {0};
	buf[0] = 0xAA;
	buf[1] = SYNC_FRAME_VERSION;

	sync_frame_t out;
	CHECK(!sync_frame_unpack(buf, sizeof(buf), &out));
}

static void test_sync_unpack_bad_version(void)
{
	uint8_t buf[SYNC_FRAME_SIZE] = {0};
	buf[0] = SYNC_FRAME_MAGIC;
	buf[1] = 0x01;

	sync_frame_t out;
	CHECK(!sync_frame_unpack(buf, sizeof(buf), &out));
}

static void test_delay_req_pack_unpack_roundtrip(void)
{
	delay_req_frame_t in = {
		.requester_node_id = 2,
		.target_node_id = 5,
		.superframe_seq = 42,
	};
	uint8_t buf[DELAY_REQ_FRAME_SIZE];
	size_t len = 0;

	CHECK(delay_req_pack(&in, buf, sizeof(buf), &len));
	CHECK(len == DELAY_REQ_FRAME_SIZE);

	delay_req_frame_t out;
	memset(&out, 0xFF, sizeof(out));
	CHECK(delay_req_unpack(buf, len, &out));
	CHECK(out.requester_node_id == 2);
	CHECK(out.target_node_id == 5);
	CHECK(out.superframe_seq == 42);
}

static void test_delay_req_unpack_short(void)
{
	uint8_t buf[DELAY_REQ_FRAME_SIZE - 1];
	memset(buf, 0, sizeof(buf));
	buf[0] = DELAY_REQ_MAGIC;
	buf[1] = DELAY_REQ_VERSION;

	delay_req_frame_t out;
	CHECK(!delay_req_unpack(buf, sizeof(buf), &out));
	CHECK(!delay_req_unpack(buf, 0, &out));
}

static void test_endianness(void)
{
	sync_frame_t in = {
		.master_node_id = 5,
		.sender_node_id = 3,
		.superframe_seq = 0x04030201U,
		.master_time_us = 0x0807060504030201LL,
		.dl_duration_us = 0x0201,
		.ul_slot_us = 0x0403,
		.guard_us = 0x0605,
		.num_slots = 2,
		.relay_hops = 1,
		.slot_map = {1, 2, 0, 0},
		.bootstrap_window_us = 0x0A09,
		.bootstrap_period = 0x0B,
	};
	uint8_t buf[SYNC_FRAME_MAX_SIZE];
	size_t len = 0;

	CHECK(sync_frame_pack(&in, buf, sizeof(buf), &len));
	CHECK(buf[0] == 0xBE);
	CHECK(buf[1] == 0x03);
	CHECK(buf[4] == 0x01);
	CHECK(buf[5] == 0x02);
	CHECK(buf[6] == 0x03);
	CHECK(buf[7] == 0x04);
	CHECK(buf[8] == 0x01);
	CHECK(buf[9] == 0x02);
	CHECK(buf[10] == 0x03);
	CHECK(buf[11] == 0x04);
	CHECK(buf[12] == 0x05);
	CHECK(buf[13] == 0x06);
	CHECK(buf[14] == 0x07);
	CHECK(buf[15] == 0x08);
	CHECK(buf[16] == 0x01);
	CHECK(buf[17] == 0x02);
	CHECK(buf[18] == 0x03);
	CHECK(buf[19] == 0x04);
	CHECK(buf[20] == 0x05);
	CHECK(buf[21] == 0x06);
	CHECK(buf[24] == 1);
	CHECK(buf[25] == 2);
	CHECK(buf[26] == 0);
	CHECK(buf[27] == 0);
	CHECK(buf[28] == 0x09);
	CHECK(buf[29] == 0x0A);
	CHECK(buf[30] == 0x0B);

	delay_req_frame_t dr = {
		.requester_node_id = 7,
		.target_node_id = 9,
		.superframe_seq = 0x0D0C0B0AU,
	};
	CHECK(delay_req_pack(&dr, buf, DELAY_REQ_FRAME_SIZE, &len));
	CHECK(buf[0] == 0xBD);
	CHECK(buf[1] == 0x02);
	CHECK(buf[4] == 0x0A);
	CHECK(buf[5] == 0x0B);
	CHECK(buf[6] == 0x0C);
	CHECK(buf[7] == 0x0D);
}

static void test_null_params(void)
{
	sync_frame_t sf = {0};
	uint8_t buf[SYNC_FRAME_MAX_SIZE];
	size_t len;

	CHECK(!sync_frame_pack(NULL, buf, sizeof(buf), &len));
	CHECK(!sync_frame_pack(&sf, NULL, sizeof(buf), &len));
	CHECK(!sync_frame_pack(&sf, buf, sizeof(buf), NULL));
	CHECK(!sync_frame_unpack(NULL, SYNC_FRAME_SIZE, &sf));
	CHECK(!sync_frame_unpack(buf, SYNC_FRAME_SIZE, NULL));

	delay_req_frame_t dr = {0};
	CHECK(!delay_req_pack(NULL, buf, sizeof(buf), &len));
	CHECK(!delay_req_pack(&dr, NULL, sizeof(buf), &len));
	CHECK(!delay_req_unpack(NULL, DELAY_REQ_FRAME_SIZE, &dr));
	CHECK(!delay_req_unpack(buf, DELAY_REQ_FRAME_SIZE, NULL));
}

static void test_sync_pack_capacity_too_small(void)
{
	sync_frame_t in = {
		.master_node_id = 1,
	};
	uint8_t buf[SYNC_FRAME_SIZE - 1];
	size_t len;
	CHECK(!sync_frame_pack(&in, buf, sizeof(buf), &len));
}

int main(void)
{
	const struct { const char *name; void (*fn)(void); } tests[] = {
		{"sync_pack_unpack_roundtrip", test_sync_pack_unpack_roundtrip},
		{"sync_unpack_short_buffer", test_sync_unpack_short_buffer},
		{"sync_unpack_bad_magic", test_sync_unpack_bad_magic},
		{"sync_unpack_bad_version", test_sync_unpack_bad_version},
		{"delay_req_pack_unpack_roundtrip", test_delay_req_pack_unpack_roundtrip},
		{"delay_req_unpack_short", test_delay_req_unpack_short},
		{"endianness", test_endianness},
		{"null_params", test_null_params},
		{"sync_pack_capacity_too_small", test_sync_pack_capacity_too_small},
	};

	size_t n = sizeof(tests) / sizeof(tests[0]);
	for (size_t i = 0; i < n; i++) {
		int before = g_failures;
		tests[i].fn();
		fprintf(stderr, "  %s: %s\n", tests[i].name,
			g_failures == before ? "OK" : "FAILED");
	}

	fprintf(stderr, "\ntest_sync_frame: %d tests, %d failures\n",
		g_tests, g_failures);
	return g_failures != 0 ? 1 : 0;
}