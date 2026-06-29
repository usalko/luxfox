#include "radiod/sync.h"

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

/* ---- Helpers ---- */

static sync_frame_t make_sync(uint8_t master_id, uint32_t seq,
                               int64_t origin_us)
{
	sync_frame_t f;
	memset(&f, 0, sizeof(f));
	f.master_node_id = master_id;
	f.sender_node_id = master_id;
	f.superframe_seq = seq;
	f.origin_time_us = origin_us;
	f.dl_duration_us = 2000;
	f.ul_slot_us = 2000;
	f.guard_us = 300;
	f.num_slots = 0;
	f.relay_hops = 0;
	return f;
}

/* ---- Tests ---- */

static void test_init_candidate(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 5, 2000, 2000, 300);
	CHECK(s.role == RADIO_ROLE_CANDIDATE);
	CHECK(s.state == SYNC_STATE_SEARCHING);
	CHECK(s.own_node_id == 5);
	CHECK(s.my_slot_index == 0xFF);
	CHECK(s.current_master_id == 0);
	CHECK(s.dl_duration_us == 2000);
	CHECK(s.ul_slot_us == 2000);
	CHECK(s.guard_us == 300);
}

static void test_election_timeout_become_master(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 5, 2000, 2000, 300);

	/* First tick sets deadline */
	radio_role_t r = radio_sync_tick(&s, 1000000);
	CHECK(r == RADIO_ROLE_CANDIDATE);

	/* Still candidate before timeout */
	r = radio_sync_tick(&s, 1000000 + SYNC_ELECTION_TIMEOUT_US - 1);
	CHECK(r == RADIO_ROLE_CANDIDATE);

	/* Timeout → MASTER */
	r = radio_sync_tick(&s, 1000000 + SYNC_ELECTION_TIMEOUT_US);
	CHECK(r == RADIO_ROLE_MASTER);
	CHECK(s.state == SYNC_STATE_MASTER_TX);
	CHECK(s.current_master_id == 5);
	CHECK(s.elections_won == 1);
	CHECK(s.role_changes == 1);
}

static void test_higher_sync_become_slave(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 5, 2000, 2000, 300);

	sync_frame_t f = make_sync(10, 1, 1000);
	bool relay = radio_sync_on_sync_rx(&s, &f, 1500, 10);

	CHECK(relay);
	CHECK(s.role == RADIO_ROLE_SLAVE);
	CHECK(s.state == SYNC_STATE_SYNCED);
	CHECK(s.current_master_id == 10);
	CHECK(s.sync_rx_count == 1);
	CHECK(s.role_changes == 1);
}

static void test_lower_sync_ignored(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 5, 2000, 2000, 300);

	sync_frame_t f = make_sync(3, 1, 1000);
	bool relay = radio_sync_on_sync_rx(&s, &f, 1500, 3);

	CHECK(!relay);
	CHECK(s.role == RADIO_ROLE_CANDIDATE);
	CHECK(s.sync_rx_count == 0);
}

static void test_equal_sync_ignored(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 5, 2000, 2000, 300);

	sync_frame_t f = make_sync(5, 1, 1000);
	bool relay = radio_sync_on_sync_rx(&s, &f, 1500, 5);

	CHECK(!relay);
	CHECK(s.role == RADIO_ROLE_CANDIDATE);
}

static void test_master_yields_to_higher(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 5, 2000, 2000, 300);

	/* Become master first */
	radio_sync_tick(&s, 0);
	radio_sync_tick(&s, SYNC_ELECTION_TIMEOUT_US);
	CHECK(s.role == RADIO_ROLE_MASTER);

	/* Higher node appears */
	sync_frame_t f = make_sync(10, 1, 1000000);
	bool relay = radio_sync_on_sync_rx(&s, &f, 1000500, 10);

	CHECK(relay);
	CHECK(s.role == RADIO_ROLE_SLAVE);
	CHECK(s.elections_lost == 1);
	CHECK(s.elections_won == 1);
}

static void test_slave_lost_threshold(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 1, 2000, 2000, 300);

	/* Become slave */
	sync_frame_t f = make_sync(5, 1, 0);
	f.num_slots = 1;
	f.slot_map[0] = 1;
	radio_sync_on_sync_rx(&s, &f, 500, 5);
	CHECK(s.role == RADIO_ROLE_SLAVE);

	/* Expected interval: max(2000 + 1*(2000+300) + 300, 12000) = 12000 */
	int64_t expected = 2000 + 1 * (2000 + 300) + 300;
	if (expected < SYNC_BEACON_INTERVAL_US)
		expected = SYNC_BEACON_INTERVAL_US;
	int64_t miss_interval = expected * SYNC_MISS_THRESHOLD + 1;

	int64_t now = 500;
	for (int i = 0; i < SYNC_LOST_THRESHOLD; i++) {
		now += miss_interval;
		radio_sync_tick(&s, now);
	}

	CHECK(s.role == RADIO_ROLE_CANDIDATE);
	CHECK(s.state == SYNC_STATE_SEARCHING);
}

static void test_slot_map_assignment(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 5, 2000, 2000, 300);

	/* Become master */
	radio_sync_tick(&s, 0);
	radio_sync_tick(&s, SYNC_ELECTION_TIMEOUT_US);

	/* Register 3 slaves via DELAY_REQ */
	int64_t now = SYNC_ELECTION_TIMEOUT_US + 1000;
	for (uint8_t id = 1; id <= 3; id++) {
		delay_req_frame_t dreq = {
			.requester_node_id = id,
			.target_node_id = 5,
			.t3_us = now - 500,
			.superframe_seq = 1,
		};
		radio_sync_on_delay_req_rx(&s, &dreq, now);
		now += 100;
	}

	radio_sync_update_slot_map(&s, now);

	CHECK(s.num_slots == 3);
	CHECK(s.slot_map[0] == 1);
	CHECK(s.slot_map[1] == 2);
	CHECK(s.slot_map[2] == 3);
	CHECK(s.slot_map[3] == 0);
}

static void test_compute_timing_slot0(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 1, 2000, 2000, 300);

	sync_frame_t f = make_sync(5, 1, 0);
	f.num_slots = 3;
	f.slot_map[0] = 1;
	f.slot_map[1] = 2;
	f.slot_map[2] = 3;
	radio_sync_on_sync_rx(&s, &f, 10000, 5);
	radio_sync_compute_timing(&s, 10000);

	CHECK(s.dl_start_us == 10000);
	CHECK(s.dl_end_us == 12000);
	/* UL base = 12000 + 300 = 12300 */
	CHECK(s.my_ul_start_us == 12300);
	CHECK(s.my_ul_end_us == 14300);
	CHECK(s.my_slot_index == 0);
}

static void test_compute_timing_slot2(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 3, 2000, 2000, 300);

	sync_frame_t f = make_sync(5, 1, 0);
	f.num_slots = 3;
	f.slot_map[0] = 1;
	f.slot_map[1] = 2;
	f.slot_map[2] = 3;
	radio_sync_on_sync_rx(&s, &f, 10000, 5);
	radio_sync_compute_timing(&s, 10000);

	/* UL base = 12300 */
	/* Slot 2 start = 12300 + 2*(2000+300) = 12300 + 4600 = 16900 */
	CHECK(s.my_ul_start_us == 16900);
	CHECK(s.my_ul_end_us == 18900);
	CHECK(s.my_slot_index == 2);
}

static void test_compute_timing_no_slot(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 4, 2000, 2000, 300);

	sync_frame_t f = make_sync(5, 1, 0);
	f.num_slots = 3;
	f.slot_map[0] = 1;
	f.slot_map[1] = 2;
	f.slot_map[2] = 3;
	radio_sync_on_sync_rx(&s, &f, 10000, 5);
	radio_sync_compute_timing(&s, 10000);

	CHECK(s.my_slot_index == 0xFF);
}

static void test_dedup_rejects_duplicate_sync(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 1, 2000, 2000, 300);

	sync_frame_t f = make_sync(5, 42, 0);
	CHECK(radio_sync_on_sync_rx(&s, &f, 500, 5));
	/* Same master+seq → rejected */
	CHECK(!radio_sync_on_sync_rx(&s, &f, 600, 5));
	CHECK(s.sync_rx_count == 1);
}

static void test_dedup_passes_new_seq(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 1, 2000, 2000, 300);

	sync_frame_t f1 = make_sync(5, 1, 0);
	sync_frame_t f2 = make_sync(5, 2, 1000);
	CHECK(radio_sync_on_sync_rx(&s, &f1, 500, 5));
	CHECK(radio_sync_on_sync_rx(&s, &f2, 1500, 5));
	CHECK(s.sync_rx_count == 2);
}

static void test_delay_req_generation(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 1, 2000, 2000, 300);

	/* Not slave → no DELAY_REQ */
	delay_req_frame_t dreq;
	CHECK(!radio_sync_build_delay_req(&s, &dreq, 0));

	/* Become slave */
	sync_frame_t f = make_sync(5, 1, 0);
	radio_sync_on_sync_rx(&s, &f, 500, 5);

	CHECK(radio_sync_build_delay_req(&s, &dreq, 1000));
	CHECK(dreq.requester_node_id == 1);
	CHECK(dreq.target_node_id == 5);
	CHECK(dreq.t3_us == 1000);
	CHECK(s.delay_req_tx_count == 1);

	/* Second call blocked — pending */
	CHECK(!radio_sync_build_delay_req(&s, &dreq, 2000));
}

static void test_delay_resp_in_beacon(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 5, 2000, 2000, 300);

	/* Become master */
	radio_sync_tick(&s, 0);
	radio_sync_tick(&s, SYNC_ELECTION_TIMEOUT_US);

	/* Receive DELAY_REQ from node 1 */
	delay_req_frame_t dreq = {
		.requester_node_id = 1,
		.target_node_id = 5,
		.t3_us = 100000,
		.superframe_seq = 1,
	};
	radio_sync_on_delay_req_rx(&s, &dreq, 100500);

	/* Build beacon */
	sync_frame_t beacon;
	radio_sync_build_beacon(&s, &beacon, 101000);

	CHECK(beacon.num_delay_resp == 1);
	CHECK(beacon.delay_resp[0].node_id == 1);
	CHECK(beacon.delay_resp[0].t4_us == 100500);

	/* Second beacon: no pending resp */
	sync_frame_t beacon2;
	radio_sync_build_beacon(&s, &beacon2, 113000);
	CHECK(beacon2.num_delay_resp == 0);
}

static void test_relay_prepare(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 3, 2000, 2000, 300);

	/* Not slave → relay fails */
	sync_frame_t rx = make_sync(5, 1, 0);
	sync_frame_t relay;
	CHECK(!radio_sync_prepare_relay(&s, &rx, &relay, 1000));

	/* Become slave */
	radio_sync_on_sync_rx(&s, &rx, 500, 5);

	/* Now relay works */
	CHECK(radio_sync_prepare_relay(&s, &rx, &relay, 1000));
	CHECK(relay.sender_node_id == 3);
	CHECK(relay.master_node_id == 5);
	CHECK(relay.relay_hops == 1);
	CHECK(relay.num_delay_resp == 0);
	CHECK(s.sync_relay_count == 1);
}

static void test_role_change_counter(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 3, 2000, 2000, 300);
	CHECK(s.role_changes == 0);

	/* CANDIDATE → SLAVE */
	sync_frame_t f = make_sync(5, 1, 0);
	radio_sync_on_sync_rx(&s, &f, 500, 5);
	CHECK(s.role_changes == 1);

	/* SLAVE → CANDIDATE (via lost threshold) */
	int64_t expected = 2000 + 300;
	if (expected < SYNC_BEACON_INTERVAL_US)
		expected = SYNC_BEACON_INTERVAL_US;
	int64_t miss_time = expected * SYNC_MISS_THRESHOLD + 1;
	int64_t now = 500;
	for (int i = 0; i < SYNC_LOST_THRESHOLD; i++) {
		now += miss_time;
		radio_sync_tick(&s, now);
	}
	CHECK(s.role_changes == 2);
}

static void test_cold_start_5_nodes(void)
{
	radio_sync_t nodes[5];
	for (int i = 0; i < 5; i++)
		radio_sync_init(&nodes[i], (uint8_t)(i + 1), 2000, 2000, 300);

	/* All start candidate, tick to trigger elections */
	for (int i = 0; i < 5; i++)
		radio_sync_tick(&nodes[i], 0);

	/* Election timeout → all become MASTER */
	for (int i = 0; i < 5; i++)
		radio_sync_tick(&nodes[i], SYNC_ELECTION_TIMEOUT_US);

	/* Now they all think they're MASTER */
	for (int i = 0; i < 5; i++)
		CHECK(nodes[i].role == RADIO_ROLE_MASTER);

	/* Each master sends SYNC; everyone receives node 5's SYNC */
	sync_frame_t sync5 = make_sync(5, 1, SYNC_ELECTION_TIMEOUT_US + 100);

	for (int i = 0; i < 4; i++) {
		radio_sync_on_sync_rx(&nodes[i], &sync5,
		                       SYNC_ELECTION_TIMEOUT_US + 600,
		                       5);
	}

	/* Nodes 1-4 → SLAVE, node 5 stays MASTER */
	for (int i = 0; i < 4; i++)
		CHECK(nodes[i].role == RADIO_ROLE_SLAVE);
	CHECK(nodes[4].role == RADIO_ROLE_MASTER);
	CHECK(nodes[4].current_master_id == 5);

	/* Verify lower SYNC frames from nodes 1-4 are ignored by node 5 */
	for (int i = 0; i < 4; i++) {
		sync_frame_t f = make_sync((uint8_t)(i + 1), 1,
		                            SYNC_ELECTION_TIMEOUT_US + 100);
		CHECK(!radio_sync_on_sync_rx(&nodes[4], &f,
		                              SYNC_ELECTION_TIMEOUT_US + 600,
		                              (uint8_t)(i + 1)));
	}
	CHECK(nodes[4].role == RADIO_ROLE_MASTER);
}

static void test_master_loss_and_reelection(void)
{
	radio_sync_t node1, node4, node5;
	radio_sync_init(&node1, 1, 2000, 2000, 300);
	radio_sync_init(&node4, 4, 2000, 2000, 300);
	radio_sync_init(&node5, 5, 2000, 2000, 300);

	/* node5 becomes master, others become slave */
	radio_sync_tick(&node5, 0);
	radio_sync_tick(&node5, SYNC_ELECTION_TIMEOUT_US);
	CHECK(node5.role == RADIO_ROLE_MASTER);

	int64_t now = SYNC_ELECTION_TIMEOUT_US + 1000;
	sync_frame_t f5 = make_sync(5, 1, now);
	f5.num_slots = 2;
	f5.slot_map[0] = 1;
	f5.slot_map[1] = 4;
	radio_sync_on_sync_rx(&node1, &f5, now + 500, 5);
	radio_sync_on_sync_rx(&node4, &f5, now + 500, 5);
	CHECK(node1.role == RADIO_ROLE_SLAVE);
	CHECK(node4.role == RADIO_ROLE_SLAVE);

	/* node5 disappears: slaves accumulate misses */
	int64_t expected = 2000 + 2 * (2000 + 300) + 300;
	if (expected < SYNC_BEACON_INTERVAL_US)
		expected = SYNC_BEACON_INTERVAL_US;
	int64_t miss_time = expected * SYNC_MISS_THRESHOLD + 1;
	int64_t t = now + 500;
	for (int i = 0; i < SYNC_LOST_THRESHOLD; i++) {
		t += miss_time;
		radio_sync_tick(&node1, t);
		radio_sync_tick(&node4, t);
	}
	CHECK(node1.role == RADIO_ROLE_CANDIDATE);
	CHECK(node4.role == RADIO_ROLE_CANDIDATE);

	/* Election: node4 wins (higher id) */
	radio_sync_tick(&node1, t);
	radio_sync_tick(&node4, t);
	t += SYNC_ELECTION_TIMEOUT_US;
	radio_sync_tick(&node1, t);
	radio_sync_tick(&node4, t);
	CHECK(node4.role == RADIO_ROLE_MASTER);
	CHECK(node1.role == RADIO_ROLE_MASTER);

	/* node1 hears node4's SYNC → becomes SLAVE */
	sync_frame_t f4 = make_sync(4, 1, t + 100);
	radio_sync_on_sync_rx(&node1, &f4, t + 600, 4);
	CHECK(node1.role == RADIO_ROLE_SLAVE);
	CHECK(node1.current_master_id == 4);

	/* node5 returns, sends SYNC(5) */
	radio_sync_init(&node5, 5, 2000, 2000, 300);
	radio_sync_tick(&node5, t + 1000);
	radio_sync_tick(&node5, t + 1000 + SYNC_ELECTION_TIMEOUT_US);
	CHECK(node5.role == RADIO_ROLE_MASTER);

	sync_frame_t f5b = make_sync(5, 2, t + 2000000);
	radio_sync_on_sync_rx(&node4, &f5b, t + 2000500, 5);
	radio_sync_on_sync_rx(&node1, &f5b, t + 2000500, 5);

	CHECK(node4.role == RADIO_ROLE_SLAVE);
	CHECK(node1.role == RADIO_ROLE_SLAVE);
	CHECK(node1.current_master_id == 5);
	CHECK(node4.current_master_id == 5);
}

static void test_queries(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 3, 2000, 2000, 300);

	CHECK(radio_sync_get_role(&s) == RADIO_ROLE_CANDIDATE);
	CHECK(!radio_sync_is_synced(&s));
	CHECK(radio_sync_get_offset(&s) == 0);

	sync_frame_t f = make_sync(5, 1, 0);
	radio_sync_on_sync_rx(&s, &f, 500, 5);

	CHECK(radio_sync_get_role(&s) == RADIO_ROLE_SLAVE);
	CHECK(radio_sync_is_synced(&s));

	/* NULL safety */
	CHECK(radio_sync_get_role(NULL) == RADIO_ROLE_CANDIDATE);
	CHECK(!radio_sync_is_synced(NULL));
	CHECK(radio_sync_get_offset(NULL) == 0);
}

int main(void)
{
	const struct { const char *name; void (*fn)(void); } tests[] = {
		{"init_candidate",              test_init_candidate},
		{"election_timeout_master",     test_election_timeout_become_master},
		{"higher_sync_slave",           test_higher_sync_become_slave},
		{"lower_sync_ignored",          test_lower_sync_ignored},
		{"equal_sync_ignored",          test_equal_sync_ignored},
		{"master_yields_to_higher",     test_master_yields_to_higher},
		{"slave_lost_threshold",        test_slave_lost_threshold},
		{"slot_map_assignment",         test_slot_map_assignment},
		{"compute_timing_slot0",        test_compute_timing_slot0},
		{"compute_timing_slot2",        test_compute_timing_slot2},
		{"compute_timing_no_slot",      test_compute_timing_no_slot},
		{"dedup_rejects_duplicate",     test_dedup_rejects_duplicate_sync},
		{"dedup_passes_new_seq",        test_dedup_passes_new_seq},
		{"delay_req_generation",        test_delay_req_generation},
		{"delay_resp_in_beacon",        test_delay_resp_in_beacon},
		{"relay_prepare",               test_relay_prepare},
		{"role_change_counter",         test_role_change_counter},
		{"cold_start_5_nodes",          test_cold_start_5_nodes},
		{"master_loss_reelection",      test_master_loss_and_reelection},
		{"queries",                     test_queries},
	};

	size_t n = sizeof(tests) / sizeof(tests[0]);
	for (size_t i = 0; i < n; i++) {
		int before = g_failures;
		tests[i].fn();
		fprintf(stderr, "  %s: %s\n", tests[i].name,
			g_failures == before ? "OK" : "FAILED");
	}

	fprintf(stderr, "\ntest_sync: %d tests, %d failures\n",
		g_tests, g_failures);
	return g_failures != 0 ? 1 : 0;
}
