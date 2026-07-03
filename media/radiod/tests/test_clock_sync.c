#include "radiod/clock_sync.h"

#include <stdio.h>

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
	CHECK(cs.last_step_mono_us == 0);
	CHECK(cs.step_count == 0);
}

static void test_ignore_invalid_master_time(void)
{
	clock_sync_t cs;
	clock_sync_init(&cs);
	CHECK(!clock_sync_apply_master_time(&cs, 0));
	CHECK(!clock_sync_apply_master_time(&cs, -1));
	CHECK(cs.step_count == 0);
}

static void test_null_safety(void)
{
	clock_sync_init(NULL);
	CHECK(!clock_sync_apply_master_time(NULL, 123));
}

int main(void)
{
	const struct { const char *name; void (*fn)(void); } tests[] = {
		{"initial_state", test_initial_state},
		{"ignore_invalid_master_time", test_ignore_invalid_master_time},
		{"null_safety", test_null_safety},
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