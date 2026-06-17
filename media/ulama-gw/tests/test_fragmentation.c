#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ulama_gw/fragmentation.h"

static int failures = 0;

static void expect_true(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		failures++;
	}
}

static void test_no_fragmentation_needed(void)
{
	uint8_t payload[100];
	memset(payload, 0xAA, sizeof(payload));

	uint8_t frag_payloads[FRAG_MAX_FRAGMENTS][ULAMA_FRAME_MAX_PAYLOAD];
	size_t frag_sizes[FRAG_MAX_FRAGMENTS];

	size_t n = frag_split(payload, sizeof(payload), frag_payloads, frag_sizes, FRAG_MAX_FRAGMENTS);
	expect_true(n == 1, "100B payload should produce 1 fragment");
	expect_true(frag_sizes[0] == 100, "single fragment should be 100B");
	expect_true(memcmp(frag_payloads[0], payload, 100) == 0, "fragment content should match");
}

static void test_exact_split(void)
{
	uint8_t payload[ULAMA_FRAME_MAX_PAYLOAD * 3];
	for (size_t i = 0; i < sizeof(payload); i++)
		payload[i] = (uint8_t)(i & 0xFF);

	uint8_t frag_payloads[FRAG_MAX_FRAGMENTS][ULAMA_FRAME_MAX_PAYLOAD];
	size_t frag_sizes[FRAG_MAX_FRAGMENTS];

	size_t n = frag_split(payload, sizeof(payload), frag_payloads, frag_sizes, FRAG_MAX_FRAGMENTS);
	expect_true(n == 3, "660B payload should produce 3 fragments");
	for (size_t i = 0; i < n; i++)
		expect_true(frag_sizes[i] == ULAMA_FRAME_MAX_PAYLOAD, "each fragment should be 220B");
}

static void test_1400_bytes(void)
{
	uint8_t payload[1400];
	for (size_t i = 0; i < sizeof(payload); i++)
		payload[i] = (uint8_t)(i & 0xFF);

	uint8_t frag_payloads[FRAG_MAX_FRAGMENTS][ULAMA_FRAME_MAX_PAYLOAD];
	size_t frag_sizes[FRAG_MAX_FRAGMENTS];

	size_t n = frag_split(payload, sizeof(payload), frag_payloads, frag_sizes, FRAG_MAX_FRAGMENTS);
	expect_true(n == 7, "1400B payload should produce 7 fragments");

	size_t total = 0;
	for (size_t i = 0; i < n; i++)
		total += frag_sizes[i];
	expect_true(total == 1400, "total fragment sizes should equal 1400");
}

static void test_reassembly(void)
{
	frag_reassembly_ctx_t ctx;
	frag_reassembly_init(&ctx);

	uint8_t original[500];
	for (size_t i = 0; i < sizeof(original); i++)
		original[i] = (uint8_t)(i & 0xFF);

	uint8_t frag_payloads[FRAG_MAX_FRAGMENTS][ULAMA_FRAME_MAX_PAYLOAD];
	size_t frag_sizes[FRAG_MAX_FRAGMENTS];
	size_t n = frag_split(original, sizeof(original), frag_payloads, frag_sizes, FRAG_MAX_FRAGMENTS);
	expect_true(n == 3, "500B should split into 3 fragments");

	for (size_t i = 0; i < n; i++) {
		ulama_frame_view_t fv = {
			.src_node = 5,
			.dst_node = 1,
			.flags = ULAMA_FLAG_FRAGMENT | ((i == n - 1) ? ULAMA_FLAG_LAST_FRAGMENT : 0),
			.traffic_class = ULAMA_CLASS_VIDEO,
			.seq = 100,
			.frag_idx = (uint8_t)i,
			.frag_total = (uint8_t)n,
			.ttl = ULAMA_FRAME_DEFAULT_TTL,
			.payload = frag_payloads[i],
			.payload_len = frag_sizes[i],
		};

		bool complete = frag_reassembly_insert(&ctx, &fv, 1000);
		if (i < n - 1)
			expect_true(!complete, "should not be complete before last fragment");
		else
			expect_true(complete, "should be complete after last fragment");
	}

	uint8_t reassembled[1500];
	size_t reassembled_len = 0;
	expect_true(frag_reassembly_complete(&ctx, 5, 100, reassembled, sizeof(reassembled), &reassembled_len),
		    "reassembly_complete should succeed");
	expect_true(reassembled_len == sizeof(original), "reassembled size should match original");
	expect_true(memcmp(reassembled, original, sizeof(original)) == 0, "reassembled content should match original");
}

int main(void)
{
	test_no_fragmentation_needed();
	test_exact_split();
	test_1400_bytes();
	test_reassembly();

	if (failures > 0) {
		fprintf(stderr, "test_fragmentation: %d failures\n", failures);
		return 1;
	}
	printf("test_fragmentation: ok\n");
	return 0;
}
