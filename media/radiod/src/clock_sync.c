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
 * Live radio scheduling uses the same process-local timebase for
 * superframe deadlines and sleeps. Stepping CLOCK_REALTIME here can move
 * that timebase backwards/forwards mid-flight and corrupt TDMA timing,
 * even though the sync engine already tracks master/local offset in
 * clock_sync_to_master()/clock_sync_to_local(). Keep the offset estimate,
 * but never mutate the host system clock from inside radiod.
 */
static void try_step_system_clock(clock_sync_t *cs)
{
	(void)cs;
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
