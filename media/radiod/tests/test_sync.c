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

static sync_frame_t make_sync(uint8_t master_id, uint32_t seq, int64_t master_time_us)
{
	sync_frame_t f;
	memset(&f, 0, sizeof(f));
	f.master_node_id = master_id;
	f.sender_node_id = master_id;
	f.superframe_seq = seq;
	f.master_time_us = master_time_us;
	f.dl_duration_us = 2000;
	f.ul_slot_us = 2000;
	f.guard_us = 300;
	f.num_slots = 0;
	f.relay_hops = 0;
	f.bootstrap_window_us = SYNC_BOOTSTRAP_WINDOW_US;
	f.bootstrap_period = SYNC_BOOTSTRAP_PERIOD;
	return f;
}

static int64_t expected_period(uint16_t dl, uint8_t num_slots, uint16_t ul, uint16_t guard)
{
	int64_t p = (int64_t)dl + (int64_t)num_slots * ((int64_t)ul + guard) + guard;
	return p < SYNC_BEACON_INTERVAL_US ? SYNC_BEACON_INTERVAL_US : p;
}

static void test_init_candidate(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 5, 2000, 2000, 300);
	CHECK(s.role == RADIO_ROLE_CANDIDATE);
	CHECK(s.state == SYNC_STATE_SEARCHING);
	CHECK(s.own_node_id == 5);
	CHECK(s.my_slot_index == 0xFF);
	CHECK(s.current_master_id == 0);
	CHECK(s.superframe_period_us == expected_period(2000, 0, 2000, 300));
}

static void test_election_timeout_become_master(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 5, 2000, 2000, 300);
	CHECK(radio_sync_tick(&s, 1000000) == RADIO_ROLE_CANDIDATE);
	CHECK(radio_sync_tick(&s, 1000000 + SYNC_ELECTION_TIMEOUT_US - 1) == RADIO_ROLE_CANDIDATE);
	CHECK(radio_sync_tick(&s, 1000000 + SYNC_ELECTION_TIMEOUT_US) == RADIO_ROLE_MASTER);
	CHECK(s.state == SYNC_STATE_MASTER_TX);
	CHECK(s.current_master_id == 5);
	CHECK(s.elections_won == 1);
	CHECK(s.role_changes == 1);
}

static void test_higher_sync_become_slave(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 5, 2000, 2000, 300);
	sync_frame_t f = make_sync(10, 1, 0);
	bool relay = radio_sync_on_sync_rx(&s, &f, 1500, 10);
	CHECK(relay);
	CHECK(s.role == RADIO_ROLE_SLAVE);
	CHECK(s.state == SYNC_STATE_SYNCED);
	CHECK(s.current_master_id == 10);
	CHECK(s.last_sync_rx_us == 1500);
	CHECK(s.predicted_anchor_us == 1500);
	CHECK(s.sync_rx_count == 1);
	CHECK(s.role_changes == 1);
}

static void test_lower_sync_ignored(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 5, 2000, 2000, 300);
	sync_frame_t f = make_sync(3, 1, 0);
	CHECK(!radio_sync_on_sync_rx(&s, &f, 1500, 3));
	CHECK(s.role == RADIO_ROLE_CANDIDATE);
	CHECK(s.sync_rx_count == 0);
}

static void test_equal_sync_ignored(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 5, 2000, 2000, 300);
	sync_frame_t f = make_sync(5, 1, 0);
	CHECK(!radio_sync_on_sync_rx(&s, &f, 1500, 5));
	CHECK(s.role == RADIO_ROLE_CANDIDATE);
}

static void test_master_yields_to_higher(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 5, 2000, 2000, 300);
	radio_sync_tick(&s, 0);
	radio_sync_tick(&s, SYNC_ELECTION_TIMEOUT_US);
	CHECK(s.role == RADIO_ROLE_MASTER);
	sync_frame_t f = make_sync(10, 1, 0);
	CHECK(radio_sync_on_sync_rx(&s, &f, 1000500, 10));
	CHECK(s.role == RADIO_ROLE_SLAVE);
	CHECK(s.elections_lost == 1);
	CHECK(s.elections_won == 1);
}

static void test_slot_map_assignment(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 5, 2000, 2000, 300);
	radio_sync_tick(&s, 0);
	radio_sync_tick(&s, SYNC_ELECTION_TIMEOUT_US);
	int64_t now = SYNC_ELECTION_TIMEOUT_US + 1000;
	for (uint8_t id = 1; id <= 3; id++) {
		delay_req_frame_t dreq = {
			.requester_node_id = id,
			.target_node_id = 5,
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
	CHECK(s.superframe_period_us == expected_period(2000, 3, 2000, 300));
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
	CHECK(s.my_ul_start_us == 12300);
	CHECK(s.my_ul_end_us == 14300);
	CHECK(s.my_slot_index == 0);
	CHECK(s.next_superframe_us == 10000 + expected_period(2000, 3, 2000, 300));
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

static void test_adaptive_period_tracks_observed_interval(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 1, 2000, 2000, 300);
	sync_frame_t f1 = make_sync(5, 10, 0);
	sync_frame_t f2 = make_sync(5, 11, 0);
	radio_sync_on_sync_rx(&s, &f1, 100000, 5);
	CHECK(!s.period_correction_valid);
	radio_sync_on_sync_rx(&s, &f2, 112300, 5); /* +300 us vs nominal 12000 */
	CHECK(s.period_correction_valid);
	CHECK(s.period_correction_us > 0);
	CHECK(s.superframe_period_us > expected_period(2000, 0, 2000, 300));
}

static void test_bootstrap_window_extends_period(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 1, 2000, 2000, 300);
	sync_frame_t f = make_sync(5, SYNC_BOOTSTRAP_PERIOD, 0);
	f.num_slots = 1;
	f.slot_map[0] = 1;
	radio_sync_on_sync_rx(&s, &f, 10000, 5);
	radio_sync_compute_timing(&s, 10000);
	CHECK(s.superframe_period_us ==
		expected_period(2000, 1, 2000, 300) + SYNC_BOOTSTRAP_WINDOW_US);
	CHECK(s.next_superframe_us == 10000 + s.superframe_period_us);

	radio_sync_on_beacon_timeout(&s, s.next_superframe_us + 1);
	CHECK(s.predicted_anchor_us ==
		10000 + expected_period(2000, 1, 2000, 300) + SYNC_BOOTSTRAP_WINDOW_US);
	CHECK(s.superframe_seq == SYNC_BOOTSTRAP_PERIOD + 1);
	CHECK(s.superframe_period_us == expected_period(2000, 1, 2000, 300));
}

static void test_dedup_rejects_duplicate_sync(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 1, 2000, 2000, 300);
	sync_frame_t f = make_sync(5, 42, 0);
	CHECK(radio_sync_on_sync_rx(&s, &f, 500, 5));
	CHECK(!radio_sync_on_sync_rx(&s, &f, 600, 5));
	CHECK(s.sync_rx_count == 1);
}

static void test_dedup_passes_new_seq(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 1, 2000, 2000, 300);
	sync_frame_t f1 = make_sync(5, 1, 0);
	sync_frame_t f2 = make_sync(5, 2, 0);
	CHECK(radio_sync_on_sync_rx(&s, &f1, 500, 5));
	CHECK(radio_sync_on_sync_rx(&s, &f2, 1500, 5));
	CHECK(s.sync_rx_count == 2);
}

static void test_delay_req_generation(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 1, 2000, 2000, 300);
	delay_req_frame_t dreq;
	CHECK(!radio_sync_build_delay_req(&s, &dreq, 0));
	sync_frame_t f = make_sync(5, 1, 0);
	radio_sync_on_sync_rx(&s, &f, 500, 5);
	CHECK(radio_sync_build_delay_req(&s, &dreq, 1000));
	CHECK(dreq.requester_node_id == 1);
	CHECK(dreq.target_node_id == 5);
	CHECK(dreq.superframe_seq == 1);
	CHECK(s.delay_req_tx_count == 1);
	CHECK(radio_sync_build_delay_req(&s, &dreq, 2000));
	CHECK(s.delay_req_tx_count == 2);
}

static void test_beacon_contains_slot_map_and_no_delay_resp(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 5, 2000, 2000, 300);
	radio_sync_tick(&s, 0);
	radio_sync_tick(&s, SYNC_ELECTION_TIMEOUT_US);
	delay_req_frame_t dreq = {
		.requester_node_id = 1,
		.target_node_id = 5,
		.superframe_seq = 1,
	};
	radio_sync_on_delay_req_rx(&s, &dreq, 100500);
	radio_sync_update_slot_map(&s, 100600);
	sync_frame_t beacon;
	radio_sync_build_beacon(&s, &beacon, 101000);
	CHECK(beacon.num_slots == 1);
	CHECK(beacon.slot_map[0] == 1);
	CHECK(beacon.relay_hops == 0);
	CHECK(beacon.master_node_id == 5);
	CHECK(beacon.sender_node_id == 5);
	CHECK(beacon.bootstrap_window_us == SYNC_BOOTSTRAP_WINDOW_US);
	CHECK(beacon.bootstrap_period == SYNC_BOOTSTRAP_PERIOD);
}

static void test_bootstrap_join_promotes_slave_to_slotted(void)
{
	radio_sync_t master, slave;
	radio_sync_init(&master, 254, 1500, 5000, 300);
	radio_sync_init(&slave, 1, 1500, 5000, 300);

	radio_sync_tick(&master, 0);
	radio_sync_tick(&master, SYNC_ELECTION_TIMEOUT_US);
	CHECK(master.role == RADIO_ROLE_MASTER);

	/* Force the next beacon to be a bootstrap-active superframe. */
	master.superframe_seq = SYNC_BOOTSTRAP_PERIOD - 1;

	sync_frame_t beacon1;
	radio_sync_build_beacon(&master, &beacon1, 1000);
	CHECK(beacon1.superframe_seq == SYNC_BOOTSTRAP_PERIOD);
	CHECK(beacon1.num_slots == 0);
	CHECK(beacon1.bootstrap_window_us == SYNC_BOOTSTRAP_WINDOW_US);

	CHECK(radio_sync_on_sync_rx(&slave, &beacon1, 5000, 254));
	CHECK(slave.role == RADIO_ROLE_SLAVE);
	CHECK(slave.my_slot_index == 0xFF);
	radio_sync_compute_timing(&slave, 5000);

	delay_req_frame_t dreq;
	CHECK(radio_sync_build_delay_req(&slave, &dreq,
		slave.next_superframe_us - slave.bootstrap_window_us + 500));
	CHECK(dreq.requester_node_id == 1);
	CHECK(dreq.target_node_id == 254);

	radio_sync_on_delay_req_rx(&master, &dreq,
		beacon1.superframe_seq * 1000 + 100);
	radio_sync_update_slot_map(&master, beacon1.superframe_seq * 1000 + 200);
	CHECK(master.num_slots == 1);
	CHECK(master.slot_map[0] == 1);

	sync_frame_t beacon2;
	radio_sync_build_beacon(&master, &beacon2, 2000);
	CHECK(beacon2.superframe_seq == SYNC_BOOTSTRAP_PERIOD + 1);
	CHECK(beacon2.num_slots == 1);
	CHECK(beacon2.slot_map[0] == 1);

	CHECK(radio_sync_on_sync_rx(&slave, &beacon2, 17000, 254));
	CHECK(slave.my_slot_index == 0);
	radio_sync_compute_timing(&slave, 17000);
	CHECK(slave.my_ul_start_us > 0);
	CHECK(radio_sync_should_transmit_ul(&slave));
}

static void test_relay_prepare(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 3, 2000, 2000, 300);
	sync_frame_t rx = make_sync(5, 1, 0);
	sync_frame_t relay;
	CHECK(!radio_sync_prepare_relay(&s, &rx, &relay, 1000));
	radio_sync_on_sync_rx(&s, &rx, 500, 5);
	CHECK(radio_sync_prepare_relay(&s, &rx, &relay, 1000));
	CHECK(relay.sender_node_id == 3);
	CHECK(relay.master_node_id == 5);
	CHECK(relay.relay_hops == 1);
	CHECK(s.sync_relay_count == 1);
}

static void test_master_ul_packet_refreshes_known_slave(void)
{
	radio_sync_t master;
	radio_sync_init(&master, 254, 1500, 5000, 300);
	radio_sync_tick(&master, 0);
	radio_sync_tick(&master, SYNC_ELECTION_TIMEOUT_US);
	CHECK(master.role == RADIO_ROLE_MASTER);

	delay_req_frame_t dreq = {
		.requester_node_id = 1,
		.target_node_id = 254,
		.superframe_seq = 1,
	};
	radio_sync_on_delay_req_rx(&master, &dreq, 100000);
	radio_sync_update_slot_map(&master, 100100);
	CHECK(master.num_known_slaves == 1);
	CHECK(master.num_slots == 1);

	/* Without a fresh DELAY_REQ the node would age out around 112ms.
	 * A normal ULAMA packet must refresh liveness too. */
	radio_sync_on_ul_packet_rx(&master, 1, 180000);
	radio_sync_update_slot_map(&master, 230000);
	CHECK(master.num_known_slaves == 1);
	CHECK(master.num_slots == 1);
	CHECK(master.slot_map[0] == 1);
}

static void test_master_ul_packet_does_not_add_unknown_slave(void)
{
	radio_sync_t master;
	radio_sync_init(&master, 254, 1500, 5000, 300);
	radio_sync_tick(&master, 0);
	radio_sync_tick(&master, SYNC_ELECTION_TIMEOUT_US);
	radio_sync_on_ul_packet_rx(&master, 1, 100000);
	CHECK(master.num_known_slaves == 0);
	CHECK(master.num_slots == 0);
}

static void test_holdover_tx_then_rx_only_then_candidate(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 1, 2000, 2000, 300);
	sync_frame_t f = make_sync(5, 1, 0);
	f.num_slots = 1;
	f.slot_map[0] = 1;
	f.bootstrap_window_us = 0;
	f.bootstrap_period = 0;
	radio_sync_on_sync_rx(&s, &f, 500, 5);
	int64_t period = expected_period(2000, 1, 2000, 300);
	for (int i = 1; i <= SYNC_HOLDOVER_TX_MAX; i++) {
		radio_sync_on_beacon_timeout(&s, 500 + (int64_t)i * period);
		CHECK(s.role == RADIO_ROLE_SLAVE);
		CHECK(s.state == SYNC_STATE_HOLDOVER_TX);
		CHECK(radio_sync_should_transmit_ul(&s));
		CHECK(s.predicted_anchor_us == 500 + (int64_t)i * period);
	}
	radio_sync_on_beacon_timeout(&s, 500 + (int64_t)(SYNC_HOLDOVER_TX_MAX + 1) * period);
	CHECK(s.state == SYNC_STATE_HOLDOVER_RX_ONLY);
	CHECK(!radio_sync_should_transmit_ul(&s));
	for (int i = SYNC_HOLDOVER_TX_MAX + 2; i < SYNC_LOST_THRESHOLD; i++)
		radio_sync_on_beacon_timeout(&s, 500 + (int64_t)i * period);
	CHECK(s.role == RADIO_ROLE_SLAVE);
	radio_sync_on_beacon_timeout(&s, 500 + (int64_t)SYNC_LOST_THRESHOLD * period);
	CHECK(s.role == RADIO_ROLE_CANDIDATE);
	CHECK(s.state == SYNC_STATE_SEARCHING);
}

static void test_resync_exits_holdover(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 1, 2000, 2000, 300);
	sync_frame_t f = make_sync(5, 1, 0);
	f.num_slots = 1;
	f.slot_map[0] = 1;
	radio_sync_on_sync_rx(&s, &f, 1000, 5);
	radio_sync_on_beacon_timeout(&s, 5000);
	CHECK(s.state == SYNC_STATE_HOLDOVER_TX);
	sync_frame_t f2 = make_sync(5, 2, 0);
	radio_sync_on_sync_rx(&s, &f2, 3300, 5);
	CHECK(s.state == SYNC_STATE_SYNCED);
	CHECK(s.missed_beacons == 0);
	CHECK(s.predicted_anchor_us == 3300);
	CHECK(radio_sync_should_transmit_ul(&s));
}

static void test_role_change_counter(void)
{
	radio_sync_t s;
	radio_sync_init(&s, 3, 2000, 2000, 300);
	CHECK(s.role_changes == 0);
	sync_frame_t f = make_sync(5, 1, 0);
	radio_sync_on_sync_rx(&s, &f, 500, 5);
	CHECK(s.role_changes == 1);
	for (int i = 0; i < SYNC_LOST_THRESHOLD; i++)
		radio_sync_on_beacon_timeout(&s, 1000 + i * 5000);
	CHECK(s.role_changes == 2);
}

static void test_cold_start_5_nodes(void)
{
	radio_sync_t nodes[5];
	for (int i = 0; i < 5; i++)
		radio_sync_init(&nodes[i], (uint8_t)(i + 1), 2000, 2000, 300);
	for (int i = 0; i < 5; i++)
		radio_sync_tick(&nodes[i], 0);
	for (int i = 0; i < 5; i++)
		radio_sync_tick(&nodes[i], SYNC_ELECTION_TIMEOUT_US);
	for (int i = 0; i < 5; i++)
		CHECK(nodes[i].role == RADIO_ROLE_MASTER);
	sync_frame_t sync5 = make_sync(5, 1, 0);
	for (int i = 0; i < 4; i++)
		radio_sync_on_sync_rx(&nodes[i], &sync5, SYNC_ELECTION_TIMEOUT_US + 600, 5);
	for (int i = 0; i < 4; i++)
		CHECK(nodes[i].role == RADIO_ROLE_SLAVE);
	CHECK(nodes[4].role == RADIO_ROLE_MASTER);
	CHECK(nodes[4].current_master_id == 5);
	for (int i = 0; i < 4; i++) {
		sync_frame_t f = make_sync((uint8_t)(i + 1), 1, 0);
		CHECK(!radio_sync_on_sync_rx(&nodes[4], &f,
			SYNC_ELECTION_TIMEOUT_US + 600, (uint8_t)(i + 1)));
	}
	CHECK(nodes[4].role == RADIO_ROLE_MASTER);
}

static void test_master_loss_and_reelection(void)
{
	radio_sync_t node1, node4, node5;
	radio_sync_init(&node1, 1, 2000, 2000, 300);
	radio_sync_init(&node4, 4, 2000, 2000, 300);
	radio_sync_init(&node5, 5, 2000, 2000, 300);
	radio_sync_tick(&node5, 0);
	radio_sync_tick(&node5, SYNC_ELECTION_TIMEOUT_US);
	CHECK(node5.role == RADIO_ROLE_MASTER);
	int64_t now = SYNC_ELECTION_TIMEOUT_US + 1000;
	sync_frame_t f5 = make_sync(5, 1, 0);
	f5.num_slots = 2;
	f5.slot_map[0] = 1;
	f5.slot_map[1] = 4;
	radio_sync_on_sync_rx(&node1, &f5, now + 500, 5);
	radio_sync_on_sync_rx(&node4, &f5, now + 500, 5);
	CHECK(node1.role == RADIO_ROLE_SLAVE);
	CHECK(node4.role == RADIO_ROLE_SLAVE);
	for (int i = 0; i < SYNC_LOST_THRESHOLD; i++) {
		radio_sync_on_beacon_timeout(&node1, now + 1000 + i * 5000);
		radio_sync_on_beacon_timeout(&node4, now + 1000 + i * 5000);
	}
	CHECK(node1.role == RADIO_ROLE_CANDIDATE);
	CHECK(node4.role == RADIO_ROLE_CANDIDATE);
	radio_sync_tick(&node1, now + 60000);
	radio_sync_tick(&node4, now + 60000);
	radio_sync_tick(&node1, now + 60000 + SYNC_ELECTION_TIMEOUT_US);
	radio_sync_tick(&node4, now + 60000 + SYNC_ELECTION_TIMEOUT_US);
	CHECK(node4.role == RADIO_ROLE_MASTER);
	CHECK(node1.role == RADIO_ROLE_MASTER);
	sync_frame_t f4 = make_sync(4, 1, 0);
	radio_sync_on_sync_rx(&node1, &f4, now + 61000, 4);
	CHECK(node1.role == RADIO_ROLE_SLAVE);
	CHECK(node1.current_master_id == 4);
	radio_sync_init(&node5, 5, 2000, 2000, 300);
	radio_sync_tick(&node5, now + 70000);
	radio_sync_tick(&node5, now + 70000 + SYNC_ELECTION_TIMEOUT_US);
	CHECK(node5.role == RADIO_ROLE_MASTER);
	sync_frame_t f5b = make_sync(5, 2, 0);
	radio_sync_on_sync_rx(&node4, &f5b, now + 70500, 5);
	radio_sync_on_sync_rx(&node1, &f5b, now + 70500, 5);
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
	CHECK(!radio_sync_should_transmit_ul(&s));
	CHECK(radio_sync_get_offset(&s) == 0);
	sync_frame_t f = make_sync(5, 1, 0);
	radio_sync_on_sync_rx(&s, &f, 500, 5);
	CHECK(radio_sync_get_role(&s) == RADIO_ROLE_SLAVE);
	CHECK(radio_sync_is_synced(&s));
	CHECK(radio_sync_should_transmit_ul(&s));
	CHECK(radio_sync_get_role(NULL) == RADIO_ROLE_CANDIDATE);
	CHECK(!radio_sync_is_synced(NULL));
	CHECK(!radio_sync_should_transmit_ul(NULL));
	CHECK(radio_sync_get_offset(NULL) == 0);
}

int main(void)
{
	const struct { const char *name; void (*fn)(void); } tests[] = {
		{"init_candidate", test_init_candidate},
		{"election_timeout_master", test_election_timeout_become_master},
		{"higher_sync_slave", test_higher_sync_become_slave},
		{"lower_sync_ignored", test_lower_sync_ignored},
		{"equal_sync_ignored", test_equal_sync_ignored},
		{"master_yields_to_higher", test_master_yields_to_higher},
		{"slot_map_assignment", test_slot_map_assignment},
		{"compute_timing_slot0", test_compute_timing_slot0},
		{"compute_timing_slot2", test_compute_timing_slot2},
		{"compute_timing_no_slot", test_compute_timing_no_slot},
		{"adaptive_period_tracks_observed_interval", test_adaptive_period_tracks_observed_interval},
		{"bootstrap_window_extends_period", test_bootstrap_window_extends_period},
		{"dedup_rejects_duplicate_sync", test_dedup_rejects_duplicate_sync},
		{"dedup_passes_new_seq", test_dedup_passes_new_seq},
		{"delay_req_generation", test_delay_req_generation},
		{"beacon_contains_slot_map_and_no_delay_resp", test_beacon_contains_slot_map_and_no_delay_resp},
		{"bootstrap_join_promotes_slave_to_slotted", test_bootstrap_join_promotes_slave_to_slotted},
		{"relay_prepare", test_relay_prepare},
		{"master_ul_packet_refreshes_known_slave", test_master_ul_packet_refreshes_known_slave},
		{"master_ul_packet_does_not_add_unknown_slave", test_master_ul_packet_does_not_add_unknown_slave},
		{"holdover_tx_then_rx_only_then_candidate", test_holdover_tx_then_rx_only_then_candidate},
		{"resync_exits_holdover", test_resync_exits_holdover},
		{"role_change_counter", test_role_change_counter},
		{"cold_start_5_nodes", test_cold_start_5_nodes},
		{"master_loss_and_reelection", test_master_loss_and_reelection},
		{"queries", test_queries},
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