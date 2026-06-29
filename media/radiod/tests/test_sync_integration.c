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

/* Simulate one SYNC exchange: master → slave (in-memory, no pcap) */
static void sim_sync_round(radio_sync_t *master, radio_sync_t *slave,
                           int64_t *now, int64_t link_delay_us)
{
	/* Master builds beacon */
	sync_frame_t beacon;
	radio_sync_build_beacon(master, &beacon, *now);

	/* Slave receives it after link_delay */
	*now += link_delay_us;
	radio_sync_on_sync_rx(slave, &beacon, *now, master->own_node_id);

	/* Slave sends DELAY_REQ in its UL slot */
	radio_sync_compute_timing(slave, *now);

	delay_req_frame_t dreq;
	int64_t dreq_time = *now + slave->dl_duration_us + slave->guard_us;
	if (radio_sync_build_delay_req(slave, &dreq, dreq_time)) {
		/* Master receives DELAY_REQ after link_delay */
		int64_t master_rx = dreq_time + link_delay_us;
		radio_sync_on_delay_req_rx(master, &dreq, master_rx);
	}

	*now = dreq_time + link_delay_us + 1000;
}

/* ---- Test: 2-node integration ---- */

static void test_2node_integration(void)
{
	radio_sync_t node1, node5;
	radio_sync_init(&node1, 1, 2000, 2000, 300);
	radio_sync_init(&node5, 5, 2000, 2000, 300);

	int64_t now = 0;

	/* Both start as CANDIDATE */
	radio_sync_tick(&node1, now);
	radio_sync_tick(&node5, now);

	/* Election timeout → both become MASTER */
	now += SYNC_ELECTION_TIMEOUT_US;
	radio_sync_tick(&node1, now);
	radio_sync_tick(&node5, now);
	CHECK(node1.role == RADIO_ROLE_MASTER);
	CHECK(node5.role == RADIO_ROLE_MASTER);

	/* Node 5 sends SYNC → node 1 becomes SLAVE */
	now += 1000;
	radio_sync_update_slot_map(&node5, now);

	for (int round = 0; round < 100; round++) {
		sim_sync_round(&node5, &node1, &now, 500);
		radio_sync_tick(&node1, now);
		radio_sync_tick(&node5, now);
		radio_sync_update_slot_map(&node5, now);
	}

	CHECK(node5.role == RADIO_ROLE_MASTER);
	CHECK(node1.role == RADIO_ROLE_SLAVE);
	CHECK(node1.current_master_id == 5);

	/* Clock offset converges toward link_delay/2 (one-way est) */
	CHECK(node1.clock.synced);
	int64_t offset = node1.clock.offset_us;
	/* EMA converges slowly; with 100 rounds and alpha=1/8 it should
	 * be well within 1000µs of the true offset (~0 for symmetric) */
	CHECK(offset >= -1000 && offset <= 1000);

	CHECK(node1.clock.rtt_us > 0);
	CHECK(node5.sync_tx_count >= 100);
	CHECK(node1.sync_rx_count >= 100);
	CHECK(node1.delay_req_tx_count > 0);
}

/* ---- Test: 5-node cold start ---- */

static void test_5node_cold_start(void)
{
	radio_sync_t nodes[5];
	for (int i = 0; i < 5; i++)
		radio_sync_init(&nodes[i], (uint8_t)(i + 1), 2000, 2000, 300);

	int64_t now = 0;

	/* All start CANDIDATE */
	for (int i = 0; i < 5; i++)
		radio_sync_tick(&nodes[i], now);

	/* Election timeout */
	now += SYNC_ELECTION_TIMEOUT_US;
	for (int i = 0; i < 5; i++)
		radio_sync_tick(&nodes[i], now);

	/* Node 5 (index 4) wins, others hear its SYNC */
	now += 1000;

	/* Run 50 sync rounds: node 5 is master */
	for (int round = 0; round < 50; round++) {
		radio_sync_update_slot_map(&nodes[4], now);

		sync_frame_t beacon;
		radio_sync_build_beacon(&nodes[4], &beacon, now);

		now += 500;

		for (int i = 0; i < 4; i++) {
			radio_sync_on_sync_rx(&nodes[i], &beacon, now,
			                       nodes[4].own_node_id);
			radio_sync_compute_timing(&nodes[i], now);
		}

		/* Each slave sends DELAY_REQ */
		for (int i = 0; i < 4; i++) {
			delay_req_frame_t dreq;
			int64_t t3 = now + 1000 + i * 2300;
			if (radio_sync_build_delay_req(&nodes[i], &dreq, t3)) {
				radio_sync_on_delay_req_rx(&nodes[4], &dreq,
				                            t3 + 500);
			}
		}

		now += 12000;

		for (int i = 0; i < 5; i++)
			radio_sync_tick(&nodes[i], now);
	}

	/* Verify: node 5 = MASTER, all others SLAVE */
	CHECK(nodes[4].role == RADIO_ROLE_MASTER);
	for (int i = 0; i < 4; i++) {
		CHECK(nodes[i].role == RADIO_ROLE_SLAVE);
		CHECK(nodes[i].current_master_id == 5);
	}

	/* Slot map should have all 4 slaves sorted by node_id */
	CHECK(nodes[4].num_slots == 4);
	CHECK(nodes[4].slot_map[0] == 1);
	CHECK(nodes[4].slot_map[1] == 2);
	CHECK(nodes[4].slot_map[2] == 3);
	CHECK(nodes[4].slot_map[3] == 4);

	/* Each slave should know its slot */
	for (int i = 0; i < 4; i++)
		CHECK(nodes[i].my_slot_index == (uint8_t)i);

	/* All clocks synced */
	for (int i = 0; i < 4; i++)
		CHECK(nodes[i].clock.synced);
}

/* ---- Test: master loss + re-election ---- */

static void test_master_loss_reelection(void)
{
	radio_sync_t nodes[5];
	for (int i = 0; i < 5; i++)
		radio_sync_init(&nodes[i], (uint8_t)(i + 1), 2000, 2000, 300);

	int64_t now = 0;

	/* Bootstrap: node 5 as master */
	for (int i = 0; i < 5; i++)
		radio_sync_tick(&nodes[i], now);
	now += SYNC_ELECTION_TIMEOUT_US;
	for (int i = 0; i < 5; i++)
		radio_sync_tick(&nodes[i], now);

	/* Run 20 rounds to establish sync */
	for (int round = 0; round < 20; round++) {
		radio_sync_update_slot_map(&nodes[4], now);
		sync_frame_t beacon;
		radio_sync_build_beacon(&nodes[4], &beacon, now);
		now += 500;
		for (int i = 0; i < 4; i++)
			radio_sync_on_sync_rx(&nodes[i], &beacon, now, 5);
		now += 12000;
		for (int i = 0; i < 5; i++)
			radio_sync_tick(&nodes[i], now);
	}

	CHECK(nodes[4].role == RADIO_ROLE_MASTER);

	/* Node 5 disappears: keep ticking slaves until they notice */
	int64_t expected = 12000;
	int64_t miss_time = expected * SYNC_MISS_THRESHOLD + 1;
	for (int i = 0; i < SYNC_LOST_THRESHOLD + 1; i++) {
		now += miss_time;
		for (int j = 0; j < 4; j++)
			radio_sync_tick(&nodes[j], now);
	}

	/* Nodes 1-4 should be CANDIDATE */
	for (int i = 0; i < 4; i++)
		CHECK(nodes[i].role == RADIO_ROLE_CANDIDATE);

	/* Election timeout → node 4 (index 3) wins */
	now += SYNC_ELECTION_TIMEOUT_US;
	for (int i = 0; i < 4; i++)
		radio_sync_tick(&nodes[i], now);

	/* All become MASTER initially */
	for (int i = 0; i < 4; i++)
		CHECK(nodes[i].role == RADIO_ROLE_MASTER);

	/* Node 4 sends SYNC → others yield */
	now += 1000;
	sync_frame_t beacon4;
	radio_sync_update_slot_map(&nodes[3], now);
	radio_sync_build_beacon(&nodes[3], &beacon4, now);
	now += 500;
	for (int i = 0; i < 3; i++)
		radio_sync_on_sync_rx(&nodes[i], &beacon4, now, 4);

	CHECK(nodes[3].role == RADIO_ROLE_MASTER);
	for (int i = 0; i < 3; i++)
		CHECK(nodes[i].role == RADIO_ROLE_SLAVE);

	/* Node 5 returns */
	radio_sync_init(&nodes[4], 5, 2000, 2000, 300);
	radio_sync_tick(&nodes[4], now);
	now += SYNC_ELECTION_TIMEOUT_US;
	radio_sync_tick(&nodes[4], now);
	CHECK(nodes[4].role == RADIO_ROLE_MASTER);
	/* Start seq high to avoid dedup collisions with old beacons */
	nodes[4].superframe_seq = 1000;

	/* Node 5 sends SYNC → all others yield */
	now += 1000;
	sync_frame_t beacon5;
	radio_sync_build_beacon(&nodes[4], &beacon5, now);
	now += 500;
	for (int i = 0; i < 4; i++)
		radio_sync_on_sync_rx(&nodes[i], &beacon5, now, 5);

	CHECK(nodes[4].role == RADIO_ROLE_MASTER);
	for (int i = 0; i < 4; i++)
		CHECK(nodes[i].role == RADIO_ROLE_SLAVE);
	for (int i = 0; i < 4; i++)
		CHECK(nodes[i].current_master_id == 5);
}

/* ---- Test: relay (3-hop) ---- */

static void test_relay_3hop(void)
{
	/* node1 ←→ node3 ←→ node5
	 * node1 cannot see node5 directly */
	radio_sync_t node1, node3, node5;
	radio_sync_init(&node1, 1, 2000, 2000, 300);
	radio_sync_init(&node3, 3, 2000, 2000, 300);
	radio_sync_init(&node5, 5, 2000, 2000, 300);

	int64_t now = 0;

	/* Election → all MASTER */
	radio_sync_tick(&node1, now);
	radio_sync_tick(&node3, now);
	radio_sync_tick(&node5, now);
	now += SYNC_ELECTION_TIMEOUT_US;
	radio_sync_tick(&node1, now);
	radio_sync_tick(&node3, now);
	radio_sync_tick(&node5, now);

	/* Run sync: node5 → node3 (direct), node3 → node1 (relay) */
	for (int round = 0; round < 50; round++) {
		now += 1000;

		/* Node 5 sends SYNC */
		radio_sync_update_slot_map(&node5, now);
		sync_frame_t beacon;
		radio_sync_build_beacon(&node5, &beacon, now);

		/* Node 3 receives directly from node 5 */
		now += 300;
		radio_sync_on_sync_rx(&node3, &beacon, now, 5);
		radio_sync_compute_timing(&node3, now);

		/* Node 3 relays to node 1 */
		sync_frame_t relay;
		now += 100;
		if (radio_sync_prepare_relay(&node3, &beacon, &relay, now)) {
			now += 300;
			radio_sync_on_sync_rx(&node1, &relay, now, 3);
			radio_sync_compute_timing(&node1, now);
		}

		/* DELAY_REQ from node3 to node5 */
		delay_req_frame_t dreq3;
		int64_t t3 = now + 1000;
		if (radio_sync_build_delay_req(&node3, &dreq3, t3))
			radio_sync_on_delay_req_rx(&node5, &dreq3, t3 + 300);

		now += 12000;
		radio_sync_tick(&node1, now);
		radio_sync_tick(&node3, now);
		radio_sync_tick(&node5, now);
	}

	CHECK(node5.role == RADIO_ROLE_MASTER);
	CHECK(node3.role == RADIO_ROLE_SLAVE);
	CHECK(node1.role == RADIO_ROLE_SLAVE);

	CHECK(node3.current_master_id == 5);
	CHECK(node1.current_master_id == 5);

	/* node1 received via relay */
	CHECK(node3.clock.synced);
	CHECK(node1.clock.synced);

	/* Relay counter on node3 */
	CHECK(node3.sync_relay_count > 0);
}

/* ---- Test: wire roundtrip through pack/unpack in integration ---- */

static void test_packed_sync_exchange(void)
{
	radio_sync_t master, slave;
	radio_sync_init(&master, 5, 2000, 2000, 300);
	radio_sync_init(&slave, 1, 2000, 2000, 300);

	int64_t now = 0;

	/* Master election */
	radio_sync_tick(&master, now);
	now += SYNC_ELECTION_TIMEOUT_US;
	radio_sync_tick(&master, now);
	CHECK(master.role == RADIO_ROLE_MASTER);

	/* Full pack/unpack cycle */
	for (int round = 0; round < 30; round++) {
		now += 12000;

		/* Build and pack SYNC */
		radio_sync_update_slot_map(&master, now);
		sync_frame_t beacon;
		radio_sync_build_beacon(&master, &beacon, now);

		uint8_t packed[SYNC_FRAME_MAX_SIZE];
		size_t packed_len;
		CHECK(sync_frame_pack(&beacon, packed, sizeof(packed), &packed_len));

		/* Unpack on slave side */
		sync_frame_t unpacked;
		CHECK(sync_frame_unpack(packed, packed_len, &unpacked));

		/* Process */
		now += 500;
		radio_sync_on_sync_rx(&slave, &unpacked, now, 5);

		/* DELAY_REQ: pack → unpack */
		delay_req_frame_t dreq;
		if (radio_sync_build_delay_req(&slave, &dreq, now + 1000)) {
			uint8_t dreq_packed[DELAY_REQ_FRAME_SIZE];
			size_t dreq_len;
			CHECK(delay_req_pack(&dreq, dreq_packed,
			                      sizeof(dreq_packed), &dreq_len));

			delay_req_frame_t dreq_unpacked;
			CHECK(delay_req_unpack(dreq_packed, dreq_len,
			                       &dreq_unpacked));

			radio_sync_on_delay_req_rx(&master, &dreq_unpacked,
			                            now + 1500);
		}

		radio_sync_tick(&master, now);
		radio_sync_tick(&slave, now);
	}

	CHECK(slave.role == RADIO_ROLE_SLAVE);
	CHECK(slave.current_master_id == 5);
	CHECK(slave.clock.synced);
	CHECK(slave.delay_resp_rx_count > 0);
}

int main(void)
{
	const struct { const char *name; void (*fn)(void); } tests[] = {
		{"2node_integration",       test_2node_integration},
		{"5node_cold_start",        test_5node_cold_start},
		{"master_loss_reelection",  test_master_loss_reelection},
		{"relay_3hop",              test_relay_3hop},
		{"packed_sync_exchange",    test_packed_sync_exchange},
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
