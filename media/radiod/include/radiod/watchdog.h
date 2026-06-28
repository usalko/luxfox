#pragma once

#include <stdbool.h>
#include <stdint.h>

/* -------------------------------------------------------------------
 * Control link watchdog — SAFETY CRITICAL.
 *
 * If no CTRL frames received for >2 seconds:
 *   1. Stop all VIDEO TX
 *   2. Switch radio to RX-only mode
 *   3. Send EMERGENCY beacon every 100ms
 *   4. Log warning
 *
 * This is a failsafe: if video floods the channel, radiod
 * automatically frees it to restore control link.
 * ------------------------------------------------------------------- */

#define RADIO_WATCHDOG_TIMEOUT_US   2000000  /* 2 seconds */
#define RADIO_EMERGENCY_INTERVAL_US 100000   /* 100 ms    */

typedef enum {
	RADIO_LINK_OK = 0,
	RADIO_LINK_DEGRADED,
	RADIO_LINK_LOST,
} radio_link_state_t;

typedef struct {
	int64_t           last_ctrl_rx_us;
	int64_t           last_emergency_us;
	radio_link_state_t state;
	uint32_t          emergency_count;
	uint32_t          lost_count;
} radio_watchdog_t;

void radio_watchdog_init(radio_watchdog_t *wd, int64_t now_us);

/* Call when a CTRL frame is received. Resets the watchdog timer. */
void radio_watchdog_feed(radio_watchdog_t *wd, int64_t now_us);

/*
 * Tick the watchdog. Returns current link state.
 * When state == RADIO_LINK_LOST:
 *   - Caller must suppress VIDEO/TELEM TX
 *   - *need_emergency is set to true if an emergency beacon should
 *     be sent this tick
 */
radio_link_state_t radio_watchdog_tick(radio_watchdog_t *wd,
				       int64_t now_us,
				       bool *need_emergency);

/* Returns true if VIDEO TX should be suppressed. */
bool radio_watchdog_video_blocked(const radio_watchdog_t *wd);
