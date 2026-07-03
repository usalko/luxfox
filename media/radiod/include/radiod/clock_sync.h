#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * Monotonic-only architecture (protocol v2).
 *
 * TDMA scheduling is done entirely on CLOCK_MONOTONIC and anchored to the
 * local RX time of each beacon (radio_sync_t.last_sync_rx_us). There is no
 * cross-node clock offset to estimate, so the old PTP/EMA apparatus is gone.
 *
 * The ONLY job left for clock_sync is to make the slave's system/log clock
 * (CLOCK_REALTIME) human-readable by stepping it to the master's wall time
 * carried in the beacon (master_time_us). This never feeds scheduling.
 *
 * Policy (P2): step only when the delta exceeds a threshold, and not more
 * often than a cooldown interval. Cooldown is measured on CLOCK_MONOTONIC so
 * a wall-clock step cannot corrupt its own rate limit. The software is
 * expected to tolerate occasional CLOCK_REALTIME jumps.
 */

/* Step CLOCK_REALTIME only when |master_time - local_wall| exceeds this. */
#define CLOCK_WALL_STEP_THRESHOLD_US 1000000    /* 1 second */
/* Do not step more often than this (measured on CLOCK_MONOTONIC). */
#define CLOCK_WALL_STEP_MIN_INTERVAL_US 5000000 /* 5 seconds */

typedef struct {
    int64_t  last_step_mono_us;  /* CLOCK_MONOTONIC of last settimeofday, 0 = never */
    uint32_t step_count;         /* how many times we stepped the wall clock */
} clock_sync_t;

void clock_sync_init(clock_sync_t *cs);

/*
 * Apply the master's wall time to the local CLOCK_REALTIME, subject to the
 * threshold + cooldown policy above. master_time_us <= 0 is ignored.
 * Returns true if a step was applied.
 */
bool clock_sync_apply_master_time(clock_sync_t *cs, int64_t master_time_us);
