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

/*
 * Step system clock if offset from master is large and stable.
 * Called after EMA update — only acts when we have enough samples
 * and the discrepancy exceeds CLOCK_SYNC_STEP_THRESHOLD_US.
 *
 * After stepping, the offset resets to near-zero (since our local
 * clock is now close to master). We allow re-stepping if the clock
 * drifts again (e.g. long uptime without NTP on master side).
 */
static void try_step_system_clock(clock_sync_t *cs)
{
	if (cs->history_count < CLOCK_SYNC_STEP_MIN_SAMPLES)
		return;

	int64_t abs_offset = cs->offset_us < 0 ? -cs->offset_us : cs->offset_us;
	if (abs_offset < CLOCK_SYNC_STEP_THRESHOLD_US)
		return;

	/* offset_us = local - master.
	 * Positive = our clock is ahead, negative = behind.
	 * Correction: new_time = now - offset_us */
	struct timeval tv;
	gettimeofday(&tv, NULL);
	int64_t now_us = (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
	int64_t corrected_us = now_us - cs->offset_us;

	tv.tv_sec = (time_t)(corrected_us / 1000000LL);
	tv.tv_usec = (suseconds_t)(corrected_us % 1000000LL);

	if (settimeofday(&tv, NULL) == 0) {
		char buf[64];
		time_t t = tv.tv_sec;
		strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&t));
		fprintf(stderr, "clock_sync: stepped system clock by %+lld ms → %s\n",
			(long long)(cs->offset_us / 1000), buf);
		cs->clock_stepped = true;
		cs->step_count++;

		/* Reset EMA — after step, offset should be ~0 */
		cs->ema_initialized = false;
		cs->history_count = 0;
		cs->history_index = 0;
		cs->offset_us = 0;
	}
}

static void history_push(clock_sync_t *cs, int64_t offset)
{
	cs->offset_history[cs->history_index] = offset;
	cs->history_index = (uint8_t)((cs->history_index + 1U) % CLOCK_SYNC_HISTORY);
	if (cs->history_count < CLOCK_SYNC_HISTORY)
		cs->history_count++;
}

static void ema_update(clock_sync_t *cs, int64_t offset)
{
	if (!cs->ema_initialized) {
		cs->ema_offset = offset;
		cs->ema_initialized = true;
	} else {
		cs->ema_offset += (offset - cs->ema_offset) >> CLOCK_SYNC_EMA_SHIFT;
	}
	cs->offset_us = cs->ema_offset;
}

int64_t clock_sync_on_sync_rx(clock_sync_t *cs,
                               int64_t t1_master, int64_t t2_local,
                               uint8_t source_node)
{
	if (cs == NULL)
		return 0;

	/* Source changed — reset EMA to avoid dragging old offset */
	if (cs->synced && source_node != cs->sync_source_node) {
		cs->ema_initialized = false;
		cs->history_count = 0;
		cs->history_index = 0;
		cs->current.t3t4_valid = false;
		cs->delay_req_pending = false;
	}
	cs->sync_source_node = source_node;

	cs->current.t1 = t1_master;
	cs->current.t2 = t2_local;

	int64_t rough_offset = t2_local - t1_master;

	int64_t offset;
	if (cs->current.t3t4_valid) {
		/* Refined: offset = ((T2-T1) - (T4-T3)) / 2 */
		int64_t fwd = t2_local - t1_master;
		int64_t rev = cs->current.t4 - cs->current.t3;
		offset = (fwd - rev) / 2;
	} else {
		offset = rough_offset;
	}

	ema_update(cs, offset);
	history_push(cs, offset);

	cs->last_update_us = t2_local;
	cs->synced = true;

	try_step_system_clock(cs);

	return rough_offset;
}

void clock_sync_prepare_delay_req(clock_sync_t *cs,
                                   int64_t t3_local,
                                   uint32_t superframe_seq)
{
	if (cs == NULL)
		return;
	cs->pending_t3 = t3_local;
	cs->pending_seq = superframe_seq;
	cs->delay_req_pending = true;
	cs->current.t3 = t3_local;
}

void clock_sync_on_delay_resp(clock_sync_t *cs,
                               int64_t t4_master)
{
	if (cs == NULL || !cs->delay_req_pending)
		return;

	cs->current.t4 = t4_master;
	cs->current.t3t4_valid = true;
	cs->delay_req_pending = false;

	int64_t fwd = cs->current.t2 - cs->current.t1;
	int64_t rev = t4_master - cs->current.t3;
	int64_t offset = (fwd - rev) / 2;

	cs->rtt_us = fwd + rev;

	ema_update(cs, offset);
	history_push(cs, offset);

	try_step_system_clock(cs);
}

int64_t clock_sync_to_master(const clock_sync_t *cs, int64_t local_us)
{
	if (cs == NULL)
		return local_us;
	return local_us - cs->offset_us;
}

int64_t clock_sync_to_local(const clock_sync_t *cs, int64_t master_us)
{
	if (cs == NULL)
		return master_us;
	return master_us + cs->offset_us;
}

bool clock_sync_is_valid(const clock_sync_t *cs, int64_t local_now_us)
{
	if (cs == NULL)
		return false;
	return cs->synced &&
	       (local_now_us - cs->last_update_us) < CLOCK_SYNC_MAX_AGE_US;
}
