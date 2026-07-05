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
	expect_true(n == 3, "3*MAX_PAYLOAD should produce 3 fragments");
	for (size_t i = 0; i < n; i++)
		expect_true(frag_sizes[i] == ULAMA_FRAME_MAX_PAYLOAD, "each fragment should be MAX_PAYLOAD");
}

static void test_large_payload(void)
{
	size_t payload_size = ULAMA_FRAME_MAX_PAYLOAD * 2 + 100;
	uint8_t payload[ULAMA_FRAME_MAX_PAYLOAD * 3];
	for (size_t i = 0; i < payload_size; i++)
		payload[i] = (uint8_t)(i & 0xFF);

	uint8_t frag_payloads[FRAG_MAX_FRAGMENTS][ULAMA_FRAME_MAX_PAYLOAD];
	size_t frag_sizes[FRAG_MAX_FRAGMENTS];

	size_t n = frag_split(payload, payload_size, frag_payloads, frag_sizes, FRAG_MAX_FRAGMENTS);
	expect_true(n == 3, "payload > 2*MAX should produce 3 fragments");

	size_t total = 0;
	for (size_t i = 0; i < n; i++)
		total += frag_sizes[i];
	expect_true(total == payload_size, "total fragment sizes should equal payload");
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
	size_t expected_frags = (sizeof(original) + ULAMA_FRAME_MAX_PAYLOAD - 1) / ULAMA_FRAME_MAX_PAYLOAD;
	expect_true(n == expected_frags, "500B should split correctly");

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

		bool complete = frag_reassembly_insert(&ctx, &fv, 0xFF, 1000);
		if (i < n - 1)
			expect_true(!complete, "should not be complete before last fragment");
		else
			expect_true(complete, "should be complete after last fragment");
	}

	uint8_t reassembled[FRAG_MAX_REASSEMBLED];
	size_t reassembled_len = 0;
	expect_true(frag_reassembly_complete(&ctx, 5, 100, reassembled, sizeof(reassembled), &reassembled_len),
		    "reassembly_complete should succeed");
	expect_true(reassembled_len == sizeof(original), "reassembled size should match original");
	expect_true(memcmp(reassembled, original, sizeof(original)) == 0, "reassembled content should match original");
}

/* 64KB frame → 29 fragments of ULAMA_FRAME_MAX_PAYLOAD (last one short).
 * Exercises FRAG_MAX_FRAGMENTS=29 and the uint32_t received_mask (needs bit 28). */
static void test_29_fragments_64kb(void)
{
	static uint8_t big_frame[65536];
	for (size_t i = 0; i < sizeof(big_frame); i++)
		big_frame[i] = (uint8_t)(i & 0xFF);

	uint8_t frag_payloads[FRAG_MAX_FRAGMENTS][ULAMA_FRAME_MAX_PAYLOAD];
	size_t frag_sizes[FRAG_MAX_FRAGMENTS];
	size_t n = frag_split(big_frame, sizeof(big_frame), frag_payloads, frag_sizes, FRAG_MAX_FRAGMENTS);
	size_t expected_frags = (sizeof(big_frame) + ULAMA_FRAME_MAX_PAYLOAD - 1) / ULAMA_FRAME_MAX_PAYLOAD;
	expect_true(expected_frags == 29, "65536B at MAX_PAYLOAD=2285 should need 29 fragments");
	expect_true(n == expected_frags, "64KB frame should split into 29 fragments");

	frag_reassembly_ctx_t ctx;
	frag_reassembly_init(&ctx);

	for (size_t i = 0; i < n; i++) {
		ulama_frame_view_t fv = {
			.src_node = 5,
			.dst_node = 1,
			.flags = ULAMA_FLAG_FRAGMENT | ((i == n - 1) ? ULAMA_FLAG_LAST_FRAGMENT : 0),
			.traffic_class = ULAMA_CLASS_VIDEO,
			.seq = 200,
			.frag_idx = (uint8_t)i,
			.frag_total = (uint8_t)n,
			.ttl = ULAMA_FRAME_DEFAULT_TTL,
			.payload = frag_payloads[i],
			.payload_len = frag_sizes[i],
		};

		bool complete = frag_reassembly_insert(&ctx, &fv, 0xFF, 1000);
		/* Fragment 28 (the last, idx >= 8) must be tracked correctly —
		 * this is exactly what the uint8_t->uint32_t received_mask fix covers. */
		if (i < n - 1)
			expect_true(!complete, "should not be complete before fragment 28");
		else
			expect_true(complete, "should be complete after all 29 fragments (incl. idx>=8)");
	}

	static uint8_t reassembled[FRAG_MAX_REASSEMBLED];
	size_t reassembled_len = 0;
	expect_true(frag_reassembly_complete(&ctx, 5, 200, reassembled, sizeof(reassembled), &reassembled_len),
		    "64KB reassembly_complete should succeed");
	expect_true(reassembled_len == sizeof(big_frame), "reassembled size should match 65536");
	expect_true(memcmp(reassembled, big_frame, sizeof(big_frame)) == 0,
		    "reassembled 64KB content should match original");
}

/* A keyframe arriving for a later seq should flush an older, still-incomplete
 * P-frame slot from the same sender, freeing it immediately instead of
 * waiting for FRAG_TIMEOUT_MS. */
static void test_flush_stale_video(void)
{
	frag_reassembly_ctx_t ctx;
	frag_reassembly_init(&ctx);

	uint8_t payload[100];
	memset(payload, 0x42, sizeof(payload));

	/* Insert fragment 0/2 of an older P-frame (seq=10) — left incomplete. */
	ulama_frame_view_t stale = {
		.src_node = 7,
		.dst_node = 1,
		.flags = ULAMA_FLAG_FRAGMENT,
		.traffic_class = ULAMA_CLASS_VIDEO,
		.seq = 10,
		.frag_idx = 0,
		.frag_total = 2,
		.ttl = ULAMA_FRAME_DEFAULT_TTL,
		.payload = payload,
		.payload_len = sizeof(payload),
	};
	bool complete = frag_reassembly_insert(&ctx, &stale, 0xFF, 1000);
	expect_true(!complete, "stale P-frame slot should be incomplete (1/2 fragments)");

	/* A newer keyframe (seq=20) completes — flush the stale slot. */
	frag_reassembly_flush_stale_video(&ctx, 7, 20);

	/* Re-inserting fragment 0 for seq=10 should now allocate a fresh slot
	 * (the old one was deactivated), not be rejected as a duplicate. */
	complete = frag_reassembly_insert(&ctx, &stale, 0xFF, 2000);
	expect_true(!complete, "flushed slot should accept seq=10 fragment 0 again as fresh");
}

int main(void)
{
	test_no_fragmentation_needed();
	test_exact_split();
	test_large_payload();
	test_reassembly();
	test_29_fragments_64kb();
	test_flush_stale_video();

	if (failures > 0) {
		fprintf(stderr, "test_fragmentation: %d failures\n", failures);
		return 1;
	}
	printf("test_fragmentation: ok\n");
	return 0;
}
