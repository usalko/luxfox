#include "radiod/sync.h"
#include "radiod/sync_frame.h"

#include <stdio.h>
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

/*
 * Real radiod hardware never runs a master superframe in the nominal
 * superframe_period_us the sync engine computes for itself. Every cycle
 * pays a fixed floor of real wall-clock time for poll()/pcap_next_ex(),
 * the DL send, the inter-slot guards and (periodically) the bootstrap RX
 * window — independent of how few UL slots are currently assigned. On the
 * host+drone pair that shipped as build #375 (main@c6919e615), radiod's
 * own [radiod stats] line measured cycles=~396 per 5.0s, i.e. ~12.6 ms/
 * cycle in *real* time, while num_slots was still 0 (nominal period only
 * dl_duration_us+guard_us = 1800us on that build, since the min-clamp to
 * SYNC_BEACON_INTERVAL_US was missing for num_slots==0). radio_sync_-
 * update_slot_map() derived its slave stale_threshold from that same
 * (wrong, tiny) nominal period, so it purged a slave that had just
 * DELAY_REQ'd in the previous bootstrap window before the next bootstrap
 * window could arrive in real time — num_known_slaves stayed 0 forever
 * even though rx_dispatcher's rx_delay_req counter kept climbing.
 *
 * These tests advance the shared clock by max(nominal_period, this floor)
 * so they exercise that real-world mismatch instead of the idealized
 * "everything happens exactly on the nominal cadence" world the plain
 * unit tests in test_sync.c use.
 */
#define REAL_CYCLE_OVERHEAD_US 12600

static int64_t real_step_us(const radio_sync_t *master)
{
	return master->superframe_period_us > REAL_CYCLE_OVERHEAD_US
		? master->superframe_period_us : REAL_CYCLE_OVERHEAD_US;
}

/*
 * One master superframe, mirroring the exact call order of master_cycle()/
 * slave_cycle() in radiod/tools/radiod.c: update_slot_map+beacon first,
 * then the (single, already-associated or bootstrap-window) slave reacts
 * and may DELAY_REQ back. Returns the number of superframes elapsed on the
 * shared clock so callers can track real elapsed time.
 */
static void run_superframe(radio_sync_t *master, radio_sync_t *slave,
			   int64_t *t, bool drop_delay_req)
{
	radio_sync_update_slot_map(master, *t);

	sync_frame_t beacon;
	radio_sync_build_beacon(master, &beacon, *t);

	radio_sync_on_sync_rx(slave, &beacon, *t, master->own_node_id);
	radio_sync_compute_timing(slave, *t);

	bool bootstrap_active = master->bootstrap_period > 0 &&
		(master->superframe_seq % master->bootstrap_period) == 0;
	bool slave_slotted = slave->my_slot_index != 0xFF;

	if (radio_sync_should_transmit_ul(slave) &&
	    (slave_slotted || bootstrap_active)) {
		delay_req_frame_t dreq;
		if (radio_sync_build_delay_req(slave, &dreq, *t) &&
		    !drop_delay_req)
			radio_sync_on_delay_req_rx(master, &dreq, *t);
	}

	*t += real_step_us(master);
}

static void elect_master(radio_sync_t *s, int64_t *t)
{
	radio_sync_tick(s, *t);
	*t += SYNC_ELECTION_TIMEOUT_US;
	radio_sync_tick(s, *t);
}

/* ---- Test: end-to-end join, mirroring radiod.c's real call order ---- */

static void test_two_node_join_and_promote(void)
{
	radio_sync_t master, slave;
	radio_sync_init(&master, 254, 1500, 5000, 300);
	radio_sync_init(&slave, 1, 1500, 5000, 300);

	int64_t t = 0;
	elect_master(&master, &t);
	CHECK(master.role == RADIO_ROLE_MASTER);

	bool promoted = false;
	for (int i = 0; i < 3 * SYNC_BOOTSTRAP_PERIOD && !promoted; i++) {
		run_superframe(&master, &slave, &t, false);
		if (slave.my_slot_index != 0xFF)
			promoted = true;
	}

	CHECK(promoted);
	CHECK(master.num_known_slaves == 1);
	CHECK(master.num_slots == 2);
	CHECK(master.slot_map[0] == 1);
	CHECK(master.slot_map[1] == 1);
	CHECK(slave.current_master_id == 254);
	CHECK(radio_sync_should_transmit_ul(&slave));
}

/*
 * The direct regression test for the production bug: a slave has just
 * been registered via DELAY_REQ (as happens once per bootstrap window)
 * and then goes quiet for one full bootstrap-to-bootstrap gap under
 * realistic (not idealized) cycle timing. It must not be purged before
 * the next bootstrap window has a chance to arrive.
 */
static void test_bootstrap_gap_survives_realistic_cycle_overhead(void)
{
	radio_sync_t master;
	radio_sync_init(&master, 254, 1500, 5000, 300);

	int64_t t = 0;
	elect_master(&master, &t);
	CHECK(master.role == RADIO_ROLE_MASTER);

	delay_req_frame_t dreq = {
		.requester_node_id = 1,
		.target_node_id = 254,
		.superframe_seq = 0,
	};
	radio_sync_on_delay_req_rx(&master, &dreq, t);
	radio_sync_update_slot_map(&master, t);
	CHECK(master.num_known_slaves == 1);
	CHECK(master.num_slots == 2);

	/* Advance through a full bootstrap_period's worth of superframes
	 * with NO further heartbeat from the slave — the worst case for a
	 * slave whose first post-join DELAY_REQ is lost to a collision. */
	for (int i = 0; i < SYNC_BOOTSTRAP_PERIOD; i++) {
		t += real_step_us(&master);
		radio_sync_update_slot_map(&master, t);
		CHECK(master.num_known_slaves == 1);
		CHECK(master.num_slots == 2);
	}
}

/*
 * Same failure class, steady state: a slotted slave that misses exactly
 * one UL DELAY_REQ (radio collision, single dropped frame) must not be
 * evicted before its next chance to transmit one superframe later.
 */
static void test_slotted_slave_tolerates_one_dropped_delay_req(void)
{
	radio_sync_t master, slave;
	radio_sync_init(&master, 254, 1500, 5000, 300);
	radio_sync_init(&slave, 1, 1500, 5000, 300);

	int64_t t = 0;
	elect_master(&master, &t);

	bool promoted = false;
	for (int i = 0; i < 3 * SYNC_BOOTSTRAP_PERIOD && !promoted; i++) {
		run_superframe(&master, &slave, &t, false);
		if (slave.my_slot_index != 0xFF)
			promoted = true;
	}
	CHECK(promoted);

	/* Drop exactly one DELAY_REQ, then resume normal heartbeats. */
	run_superframe(&master, &slave, &t, true);
	CHECK(master.num_known_slaves == 1);
	CHECK(master.num_slots == 2);

	run_superframe(&master, &slave, &t, false);
	CHECK(master.num_known_slaves == 1);
	CHECK(master.num_slots == 2);
	CHECK(slave.my_slot_index != 0xFF);
}

/* ---- Test: long run, no flapping once promoted ---- */

static void test_long_run_no_flapping_after_promotion(void)
{
	radio_sync_t master, slave;
	radio_sync_init(&master, 254, 1500, 5000, 300);
	radio_sync_init(&slave, 1, 1500, 5000, 300);

	int64_t t = 0;
	elect_master(&master, &t);

	bool promoted = false;
	int promote_cycle = -1;
	const int total_cycles = 400; /* ~5s of real time at REAL_CYCLE_OVERHEAD_US */

	for (int i = 0; i < total_cycles; i++) {
		run_superframe(&master, &slave, &t, false);

		if (!promoted) {
			if (slave.my_slot_index != 0xFF) {
				promoted = true;
				promote_cycle = i;
			}
			continue;
		}

		/* Once promoted, this must never regress for the rest of
		 * the run: no spurious staleness purge, no slot churn. */
		CHECK(master.num_known_slaves == 1);
		CHECK(master.num_slots == 2);
		CHECK(master.slot_map[0] == 1);
		CHECK(master.slot_map[1] == 1);
		CHECK(slave.my_slot_index == 0);
		CHECK(master.role == RADIO_ROLE_MASTER);
		CHECK(slave.role == RADIO_ROLE_SLAVE);
	}

	CHECK(promoted);
	CHECK(promote_cycle >= 0 && promote_cycle < 3 * SYNC_BOOTSTRAP_PERIOD);
}

/* ---- Test: multiple slaves join across separate bootstrap windows ---- */

static void test_multi_slave_cold_start(void)
{
	radio_sync_t master;
	radio_sync_t slaves[3];
	radio_sync_init(&master, 254, 1500, 5000, 300);
	for (int i = 0; i < 3; i++)
		radio_sync_init(&slaves[i], (uint8_t)(i + 1), 1500, 5000, 300);

	int64_t t = 0;
	elect_master(&master, &t);
	CHECK(master.role == RADIO_ROLE_MASTER);

	for (int cycle = 0; cycle < 6 * SYNC_BOOTSTRAP_PERIOD; cycle++) {
		radio_sync_update_slot_map(&master, t);

		sync_frame_t beacon;
		radio_sync_build_beacon(&master, &beacon, t);

		bool bootstrap_active = master.bootstrap_period > 0 &&
			(master.superframe_seq % master.bootstrap_period) == 0;

		for (int i = 0; i < 3; i++) {
			radio_sync_on_sync_rx(&slaves[i], &beacon, t,
					       master.own_node_id);
			radio_sync_compute_timing(&slaves[i], t);

			bool slotted = slaves[i].my_slot_index != 0xFF;
			if (!radio_sync_should_transmit_ul(&slaves[i]) ||
			    !(slotted || bootstrap_active))
				continue;

			delay_req_frame_t dreq;
			if (radio_sync_build_delay_req(&slaves[i], &dreq, t))
				radio_sync_on_delay_req_rx(&master, &dreq, t);
		}

		t += real_step_us(&master);
	}

	CHECK(master.num_known_slaves == 3);
	CHECK(master.num_slots == 4);
	CHECK(master.slot_map[0] == 1);
	CHECK(master.slot_map[2] == 1);
	CHECK((master.slot_map[1] == 2 && master.slot_map[3] == 3) ||
	      (master.slot_map[1] == 3 && master.slot_map[3] == 2));
	for (int i = 0; i < 3; i++) {
		CHECK(slaves[i].current_master_id == 254);
	}
	CHECK(slaves[0].my_slot_index == 0);
	CHECK(slaves[1].my_slot_index != 0xFF);
	CHECK(slaves[2].my_slot_index != 0xFF);
	CHECK(master.slot_map[slaves[1].my_slot_index] == 2);
	CHECK(master.slot_map[slaves[2].my_slot_index] == 3);
}

int main(void)
{
	const struct { const char *name; void (*fn)(void); } tests[] = {
		{"two_node_join_and_promote", test_two_node_join_and_promote},
		{"bootstrap_gap_survives_realistic_cycle_overhead",
			test_bootstrap_gap_survives_realistic_cycle_overhead},
		{"slotted_slave_tolerates_one_dropped_delay_req",
			test_slotted_slave_tolerates_one_dropped_delay_req},
		{"long_run_no_flapping_after_promotion",
			test_long_run_no_flapping_after_promotion},
		{"multi_slave_cold_start", test_multi_slave_cold_start},
	};

	size_t n = sizeof(tests) / sizeof(tests[0]);
	for (size_t i = 0; i < n; i++) {
		int before = g_failures;
		tests[i].fn();
		fprintf(stderr, "  %s: %s\n", tests[i].name,
			g_failures == before ? "OK" : "FAILED");
	}

	fprintf(stderr, "\ntest_sync_integration: %d tests, %d failures\n",
		g_tests, g_failures);
	return g_failures != 0 ? 1 : 0;
}
