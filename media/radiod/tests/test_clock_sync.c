#include "radiod/clock_sync.h"

#include <stdio.h>
#include <stdlib.h>
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

static void test_initial_state(void)
{
	clock_sync_t cs;
	clock_sync_init(&cs);
	CHECK(!cs.synced);
	CHECK(!cs.ema_initialized);
	CHECK(cs.offset_us == 0);
	CHECK(cs.rtt_us == 0);
	CHECK(!cs.delay_req_pending);
	CHECK(!clock_sync_is_valid(&cs, 1000000));
}

static void test_initial_rough_offset(void)
{
	clock_sync_t cs;
	clock_sync_init(&cs);

	/* T1=1000 (master), T2=1500 (local) → rough_offset=500 */
	int64_t rough = clock_sync_on_sync_rx(&cs, 1000, 1500, 5);
	CHECK(rough == 500);
	CHECK(cs.synced);
	CHECK(cs.offset_us == 500);
}

static void test_refined_offset_symmetric(void)
{
	clock_sync_t cs;
	clock_sync_init(&cs);

	/* Symmetric delay: 500µs each way, clocks synchronized */
	clock_sync_on_sync_rx(&cs, 0, 500, 5);

	clock_sync_prepare_delay_req(&cs, 1000, 1);
	clock_sync_on_delay_resp(&cs, 1500);

	/* offset = ((500-0) - (1500-1000)) / 2 = (500-500)/2 = 0 */
	/* rtt = (500-0) + (1500-1000) = 1000 */
	CHECK(cs.rtt_us == 1000);

	/* After EMA converges: offset should be near 0 */
	/* First sample was rough=500, second refined=0 */
	/* EMA: 500 + (0-500)/8 = 500 - 62 = 438 ... need more samples */

	/* Feed more symmetric samples to converge */
	for (int i = 0; i < 50; i++) {
		int64_t base = (int64_t)(i + 2) * 2000;
		clock_sync_on_sync_rx(&cs, base, base + 500, 5);
		clock_sync_prepare_delay_req(&cs, base + 1000, (uint32_t)(i + 2));
		clock_sync_on_delay_resp(&cs, base + 1500);
	}

	/* After many samples, offset should be very close to 0 */
	CHECK(cs.offset_us >= -5 && cs.offset_us <= 5);
	CHECK(cs.rtt_us == 1000);
}

static void test_refined_offset_asymmetric(void)
{
	clock_sync_t cs;
	clock_sync_init(&cs);

	/* Asymmetric: forward=300µs, reverse=700µs
	 * T1=0, T2=300, T3=1000, T4=1700
	 * offset = ((300-0) - (1700-1000))/2 = (300-700)/2 = -200
	 * rtt = 300 + 700 = 1000
	 */
	clock_sync_on_sync_rx(&cs, 0, 300, 5);
	clock_sync_prepare_delay_req(&cs, 1000, 1);
	clock_sync_on_delay_resp(&cs, 1700);

	/* Converge with repeated samples */
	for (int i = 0; i < 50; i++) {
		int64_t base = (int64_t)(i + 1) * 2000;
		clock_sync_on_sync_rx(&cs, base, base + 300, 5);
		clock_sync_prepare_delay_req(&cs, base + 1000, (uint32_t)(i + 2));
		clock_sync_on_delay_resp(&cs, base + 1700);
	}

	CHECK(cs.offset_us >= -205 && cs.offset_us <= -195);
	CHECK(cs.rtt_us == 1000);
}

static void test_ema_converges(void)
{
	clock_sync_t cs;
	clock_sync_init(&cs);

	/* Series of samples with stable offset=1000 */
	for (int i = 0; i < 30; i++) {
		int64_t t1 = (int64_t)i * 12000;
		int64_t t2 = t1 + 1500;
		clock_sync_on_sync_rx(&cs, t1, t2, 5);
		clock_sync_prepare_delay_req(&cs, t2 + 500, (uint32_t)i);
		clock_sync_on_delay_resp(&cs, t1 + 2000);
	}

	/* offset = ((1500) - (2000 - (t1+1500+500)))/2 ... let me recalc:
	 * T1 = i*12000
	 * T2 = i*12000 + 1500
	 * T3 = i*12000 + 2000
	 * T4 = i*12000 + 2000
	 * offset = ((T2-T1) - (T4-T3)) / 2 = (1500 - 0) / 2 = 750
	 * rtt = 1500 + 0 = 1500
	 */
	CHECK(cs.offset_us >= 745 && cs.offset_us <= 755);
}

static void test_to_master_to_local_roundtrip(void)
{
	clock_sync_t cs;
	clock_sync_init(&cs);

	/* Set up a known offset */
	for (int i = 0; i < 30; i++) {
		int64_t base = (int64_t)i * 10000;
		clock_sync_on_sync_rx(&cs, base, base + 500, 5);
		clock_sync_prepare_delay_req(&cs, base + 1000, (uint32_t)i);
		clock_sync_on_delay_resp(&cs, base + 1500);
	}

	/* Roundtrip: to_local(to_master(x)) should be close to x */
	int64_t local_time = 999999;
	int64_t master_time = clock_sync_to_master(&cs, local_time);
	int64_t back = clock_sync_to_local(&cs, master_time);
	CHECK(back == local_time);

	/* And vice-versa */
	int64_t mt = 500000;
	int64_t lt = clock_sync_to_local(&cs, mt);
	int64_t mt2 = clock_sync_to_master(&cs, lt);
	CHECK(mt2 == mt);
}

static void test_is_valid_expiry(void)
{
	clock_sync_t cs;
	clock_sync_init(&cs);

	clock_sync_on_sync_rx(&cs, 0, 1000, 5);
	CHECK(cs.synced);

	/* Still valid at +4.9 sec */
	CHECK(clock_sync_is_valid(&cs, 1000 + 4900000));
	/* Expired at +5.0 sec */
	CHECK(!clock_sync_is_valid(&cs, 1000 + 5000000));
	/* Well past */
	CHECK(!clock_sync_is_valid(&cs, 1000 + 10000000));
}

static void test_source_change_resets_ema(void)
{
	clock_sync_t cs;
	clock_sync_init(&cs);

	/* Sync to node 5 with offset ~500 */
	for (int i = 0; i < 20; i++)
		clock_sync_on_sync_rx(&cs, (int64_t)i * 1000, (int64_t)i * 1000 + 500, 5);

	CHECK(cs.offset_us >= 495 && cs.offset_us <= 505);

	/* Switch to node 3 with offset ~1000 — EMA should reset */
	clock_sync_on_sync_rx(&cs, 100000, 101000, 3);
	CHECK(cs.sync_source_node == 3);
	/* After reset, offset should jump to 1000 (not drag from 500) */
	CHECK(cs.offset_us == 1000);
}

static void test_delay_req_not_pending_blocks_resp(void)
{
	clock_sync_t cs;
	clock_sync_init(&cs);

	clock_sync_on_sync_rx(&cs, 0, 500, 5);

	/* No prepare_delay_req called — on_delay_resp should be no-op */
	int64_t offset_before = cs.offset_us;
	clock_sync_on_delay_resp(&cs, 1500);
	CHECK(cs.offset_us == offset_before);
	CHECK(cs.rtt_us == 0);
}

static void test_null_safety(void)
{
	clock_sync_init(NULL);
	CHECK(clock_sync_on_sync_rx(NULL, 0, 0, 0) == 0);
	clock_sync_prepare_delay_req(NULL, 0, 0);
	clock_sync_on_delay_resp(NULL, 0);
	CHECK(clock_sync_to_master(NULL, 42) == 42);
	CHECK(clock_sync_to_local(NULL, 42) == 42);
	CHECK(!clock_sync_is_valid(NULL, 0));
}

int main(void)
{
	const struct { const char *name; void (*fn)(void); } tests[] = {
		{"initial_state",                 test_initial_state},
		{"initial_rough_offset",          test_initial_rough_offset},
		{"refined_offset_symmetric",      test_refined_offset_symmetric},
		{"refined_offset_asymmetric",     test_refined_offset_asymmetric},
		{"ema_converges",                 test_ema_converges},
		{"to_master_to_local_roundtrip",  test_to_master_to_local_roundtrip},
		{"is_valid_expiry",               test_is_valid_expiry},
		{"source_change_resets_ema",      test_source_change_resets_ema},
		{"delay_req_not_pending",         test_delay_req_not_pending_blocks_resp},
		{"null_safety",                   test_null_safety},
	};

	size_t n = sizeof(tests) / sizeof(tests[0]);
	for (size_t i = 0; i < n; i++) {
		int before = g_failures;
		tests[i].fn();
		fprintf(stderr, "  %s: %s\n", tests[i].name,
			g_failures == before ? "OK" : "FAILED");
	}

	fprintf(stderr, "\ntest_clock_sync: %d tests, %d failures\n",
		g_tests, g_failures);
	return g_failures != 0 ? 1 : 0;
}
