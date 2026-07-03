#include "radiod/clock_sync.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

void clock_sync_init(clock_sync_t *cs)
{
	if (cs == NULL)
		return;
	memset(cs, 0, sizeof(*cs));
}

static int64_t mono_now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000LL;
}

bool clock_sync_apply_master_time(clock_sync_t *cs, int64_t master_time_us)
{
	if (cs == NULL || master_time_us <= 0)
		return false;

	/* Cooldown on the monotonic clock so a wall-clock step can't corrupt
	 * its own rate limit. */
	int64_t now_mono = mono_now_us();
	if (cs->last_step_mono_us != 0 &&
	    (now_mono - cs->last_step_mono_us) < CLOCK_WALL_STEP_MIN_INTERVAL_US)
		return false;

	struct timeval tv;
	gettimeofday(&tv, NULL);
	int64_t local_wall_us = (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
	int64_t delta_us = master_time_us - local_wall_us;
	int64_t abs_delta = delta_us < 0 ? -delta_us : delta_us;
	if (abs_delta < CLOCK_WALL_STEP_THRESHOLD_US)
		return false;

	tv.tv_sec = (time_t)(master_time_us / 1000000LL);
	tv.tv_usec = (suseconds_t)(master_time_us % 1000000LL);
	if (settimeofday(&tv, NULL) != 0)
		return false;

	cs->last_step_mono_us = now_mono;
	cs->step_count++;

	char buf[64];
	time_t t = tv.tv_sec;
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&t));
	fprintf(stderr, "clock_sync: stepped wall clock by %+lld ms -> %s\n",
		(long long)(delta_us / 1000), buf);
	return true;
}
