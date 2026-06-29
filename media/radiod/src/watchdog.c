#include "radiod/watchdog.h"

#include <stdio.h>

void radio_watchdog_init(radio_watchdog_t *wd, int64_t now_us)
{
	if (wd == NULL)
		return;
	wd->last_ctrl_rx_us = now_us;
	wd->last_emergency_us = 0;
	wd->state = RADIO_LINK_OK;
	wd->emergency_count = 0;
	wd->lost_count = 0;
}

void radio_watchdog_feed(radio_watchdog_t *wd, int64_t now_us)
{
	if (wd == NULL)
		return;

	wd->last_ctrl_rx_us = now_us;

	if (wd->state != RADIO_LINK_OK) {
		fprintf(stderr, "radiod: CTRL link restored (was %s for %u cycles)\n",
			wd->state == RADIO_LINK_LOST ? "LOST" : "DEGRADED",
			wd->lost_count);
		wd->state = RADIO_LINK_OK;
		wd->lost_count = 0;
	}
}

radio_link_state_t radio_watchdog_tick(radio_watchdog_t *wd,
				       int64_t now_us,
				       bool *need_emergency)
{
	int64_t elapsed;

	if (wd == NULL)
		return RADIO_LINK_OK;

	if (need_emergency != NULL)
		*need_emergency = false;

	elapsed = now_us - wd->last_ctrl_rx_us;

	if (elapsed < RADIO_WATCHDOG_TIMEOUT_US) {
		if (wd->state != RADIO_LINK_OK) {
			wd->state = RADIO_LINK_OK;
			wd->lost_count = 0;
		}
		return RADIO_LINK_OK;
	}

	/* CTRL timeout — link lost */
	if (wd->state != RADIO_LINK_LOST) {
		wd->state = RADIO_LINK_LOST;
		wd->lost_count = 0;
		fprintf(stderr,
			"radiod: WARNING: CTRL link LOST — "
			"no CTRL frames for %lld ms. "
			"VIDEO TX suppressed, entering RX-only mode.\n",
			(long long)(elapsed / 1000));
	}
	wd->lost_count++;

	/* Send emergency beacon at fixed interval */
	if (need_emergency != NULL) {
		int64_t since_emergency = now_us - wd->last_emergency_us;
		if (since_emergency >= RADIO_EMERGENCY_INTERVAL_US) {
			*need_emergency = true;
			wd->last_emergency_us = now_us;
			wd->emergency_count++;
		}
	}

	return RADIO_LINK_LOST;
}

bool radio_watchdog_video_blocked(const radio_watchdog_t *wd)
{
	if (wd == NULL)
		return false;
	return wd->state == RADIO_LINK_LOST;
}

void radio_watchdog_feed_sync(radio_watchdog_t *wd, int64_t now_us)
{
	radio_watchdog_feed(wd, now_us);
}
