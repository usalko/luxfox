#include "radiod/sync.h"
#include <stdio.h>
#include <string.h>

void radio_sync_init(radio_sync_t *s, uint8_t own_node_id,
                     uint16_t dl_us, uint16_t ul_us, uint16_t guard_us)
{
	if (s == NULL)
		return;
	memset(s, 0, sizeof(*s));
	s->own_node_id = own_node_id;
	s->role = RADIO_ROLE_CANDIDATE;
	s->state = SYNC_STATE_SEARCHING;
	s->dl_duration_us = dl_us;
	s->ul_slot_us = ul_us;
	s->guard_us = guard_us;
	s->my_slot_index = 0xFF;
	clock_sync_init(&s->clock);
}

/* ================================================================
 * Dedup ring: prevents processing the same SYNC twice
 * ================================================================ */

static bool dedup_seen(radio_sync_t *s, uint8_t master_id, uint32_t seq)
{
	for (uint16_t i = 0; i < s->dedup_count; i++) {
		uint16_t idx = (uint16_t)((s->dedup_head + SYNC_DEDUP_WINDOW
					   - 1U - i) % SYNC_DEDUP_WINDOW);
		sync_dedup_key_t *k = &s->dedup_ring[idx];
		if (k->master_node_id == master_id && k->superframe_seq == seq)
			return true;
	}

	sync_dedup_key_t *slot = &s->dedup_ring[s->dedup_head];
	slot->master_node_id = master_id;
	slot->superframe_seq = seq;
	s->dedup_head = (uint16_t)((s->dedup_head + 1U) % SYNC_DEDUP_WINDOW);
	if (s->dedup_count < SYNC_DEDUP_WINDOW)
		s->dedup_count++;
	return false;
}

/* ================================================================
 * Slave table helpers (master side)
 * ================================================================ */

static sync_slave_info_t *find_slave(radio_sync_t *s, uint8_t node_id)
{
	for (uint8_t i = 0; i < SYNC_MAX_NODES; i++) {
		if (s->slaves[i].active && s->slaves[i].node_id == node_id)
			return &s->slaves[i];
	}
	return NULL;
}

static sync_slave_info_t *alloc_slave(radio_sync_t *s, uint8_t node_id,
                                      int64_t now_us)
{
	for (uint8_t i = 0; i < SYNC_MAX_NODES; i++) {
		if (!s->slaves[i].active) {
			s->slaves[i].node_id = node_id;
			s->slaves[i].active = true;
			s->slaves[i].last_seen_us = now_us;
			s->slaves[i].delay_resp_pending = false;
			s->num_known_slaves++;
			return &s->slaves[i];
		}
	}
	return NULL;
}

/* ================================================================
 * Event: received SYNC frame
 * ================================================================ */

bool radio_sync_on_sync_rx(radio_sync_t *s,
                           const sync_frame_t *frame,
                           int64_t local_rx_us,
                           uint8_t sender_node_id)
{
	if (s == NULL || frame == NULL)
		return false;

	if (frame->master_node_id == s->own_node_id)
		return false;

	if (frame->master_node_id < s->own_node_id)
		return false;

	if (dedup_seen(s, frame->master_node_id, frame->superframe_seq))
		return false;

	/* Higher master — yield */
	if (s->role == RADIO_ROLE_MASTER) {
		fprintf(stderr, "sync: yielding master to node %u\n",
			frame->master_node_id);
		s->elections_lost++;
		s->role_changes++;
	} else if (s->role == RADIO_ROLE_CANDIDATE) {
		s->role_changes++;
	}

	s->role = RADIO_ROLE_SLAVE;
	s->state = SYNC_STATE_SYNCED;
	s->current_master_id = frame->master_node_id;
	s->missed_beacons = 0;
	s->last_sync_rx_us = local_rx_us;

	clock_sync_on_sync_rx(&s->clock, frame->origin_time_us,
	                       local_rx_us, sender_node_id);

	for (uint8_t i = 0; i < frame->num_delay_resp; i++) {
		if (frame->delay_resp[i].node_id == s->own_node_id) {
			clock_sync_on_delay_resp(&s->clock,
			                         frame->delay_resp[i].t4_us);
			s->delay_resp_rx_count++;
		}
	}

	s->dl_duration_us = frame->dl_duration_us;
	s->ul_slot_us = frame->ul_slot_us;
	s->guard_us = frame->guard_us;
	s->num_slots = frame->num_slots;
	memcpy(s->slot_map, frame->slot_map, SYNC_MAX_SLOTS);

	s->my_slot_index = 0xFF;
	for (uint8_t i = 0; i < s->num_slots; i++) {
		if (s->slot_map[i] == s->own_node_id) {
			s->my_slot_index = i;
			break;
		}
	}

	s->superframe_seq = frame->superframe_seq;
	s->sync_rx_count++;
	return true;
}

/* ================================================================
 * Event: received DELAY_REQ (master side)
 * ================================================================ */

void radio_sync_on_delay_req_rx(radio_sync_t *s,
                                const delay_req_frame_t *dreq,
                                int64_t local_rx_us)
{
	if (s == NULL || dreq == NULL)
		return;
	if (s->role != RADIO_ROLE_MASTER)
		return;
	if (dreq->target_node_id != s->own_node_id)
		return;

	sync_slave_info_t *sl = find_slave(s, dreq->requester_node_id);
	if (sl == NULL)
		sl = alloc_slave(s, dreq->requester_node_id, local_rx_us);
	if (sl == NULL)
		return;

	sl->delay_req_t4 = local_rx_us;
	sl->delay_resp_pending = true;
	sl->last_seen_us = local_rx_us;
}

/* ================================================================
 * Master: build SYNC beacon
 * ================================================================ */

void radio_sync_build_beacon(radio_sync_t *s,
                              sync_frame_t *out_frame,
                              int64_t now_us)
{
	if (s == NULL || out_frame == NULL)
		return;

	memset(out_frame, 0, sizeof(*out_frame));

	s->superframe_seq++;

	out_frame->master_node_id = s->own_node_id;
	out_frame->sender_node_id = s->own_node_id;
	out_frame->superframe_seq = s->superframe_seq;
	out_frame->origin_time_us = now_us;
	out_frame->dl_duration_us = s->dl_duration_us;
	out_frame->ul_slot_us = s->ul_slot_us;
	out_frame->guard_us = s->guard_us;
	out_frame->num_slots = s->num_slots;
	out_frame->relay_hops = 0;
	memcpy(out_frame->slot_map, s->slot_map, SYNC_MAX_SLOTS);

	out_frame->num_delay_resp = 0;
	for (uint8_t i = 0; i < SYNC_MAX_NODES; i++) {
		if (!s->slaves[i].active || !s->slaves[i].delay_resp_pending)
			continue;
		if (out_frame->num_delay_resp >= SYNC_MAX_DELAY_RESP)
			break;
		uint8_t n = out_frame->num_delay_resp;
		out_frame->delay_resp[n].node_id = s->slaves[i].node_id;
		out_frame->delay_resp[n].t4_us = s->slaves[i].delay_req_t4;
		out_frame->num_delay_resp++;
		s->slaves[i].delay_resp_pending = false;
	}

	s->sync_tx_count++;
}

/* ================================================================
 * Master: assign UL slots to known active slaves
 * ================================================================ */

void radio_sync_update_slot_map(radio_sync_t *s, int64_t now_us)
{
	if (s == NULL)
		return;

	uint8_t active_ids[SYNC_MAX_NODES];
	uint8_t n_active = 0;

	int64_t superframe_period = (int64_t)s->dl_duration_us +
		(int64_t)s->num_slots * ((int64_t)s->ul_slot_us + s->guard_us) +
		s->guard_us;
	int64_t stale_threshold = superframe_period * 5;
	if (stale_threshold < 60000)
		stale_threshold = 60000;

	for (uint8_t i = 0; i < SYNC_MAX_NODES; i++) {
		if (!s->slaves[i].active)
			continue;
		if (now_us - s->slaves[i].last_seen_us > stale_threshold) {
			s->slaves[i].active = false;
			s->num_known_slaves--;
			continue;
		}
		if (n_active < SYNC_MAX_NODES)
			active_ids[n_active++] = s->slaves[i].node_id;
	}

	/* Insertion sort by node_id for determinism */
	for (uint8_t i = 1; i < n_active; i++) {
		uint8_t key = active_ids[i];
		int j = (int)i - 1;
		while (j >= 0 && active_ids[j] > key) {
			active_ids[j + 1] = active_ids[j];
			j--;
		}
		active_ids[j + 1] = key;
	}

	memset(s->slot_map, 0, sizeof(s->slot_map));
	s->num_slots = n_active < SYNC_MAX_SLOTS ? n_active : SYNC_MAX_SLOTS;
	for (uint8_t i = 0; i < s->num_slots; i++)
		s->slot_map[i] = active_ids[i];
}

/* ================================================================
 * Slave: compute absolute timing boundaries from last SYNC
 * ================================================================ */

void radio_sync_compute_timing(radio_sync_t *s, int64_t local_now_us)
{
	if (s == NULL)
		return;

	(void)local_now_us;

	s->dl_start_us = s->last_sync_rx_us;
	s->dl_end_us = s->dl_start_us + s->dl_duration_us;

	int64_t ul_base = s->dl_end_us + s->guard_us;

	s->my_ul_start_us = 0;
	s->my_ul_end_us = 0;

	for (uint8_t i = 0; i < s->num_slots; i++) {
		int64_t slot_start = ul_base +
			(int64_t)i * ((int64_t)s->ul_slot_us + s->guard_us);
		if (s->slot_map[i] == s->own_node_id) {
			s->my_ul_start_us = slot_start;
			s->my_ul_end_us = slot_start + s->ul_slot_us;
			s->my_slot_index = i;
		}
	}

	s->next_superframe_us = ul_base +
		(int64_t)s->num_slots * ((int64_t)s->ul_slot_us + s->guard_us) +
		s->guard_us;
}

/* ================================================================
 * Slave: build DELAY_REQ
 * ================================================================ */

bool radio_sync_build_delay_req(radio_sync_t *s,
                                 delay_req_frame_t *out,
                                 int64_t now_us)
{
	if (s == NULL || out == NULL)
		return false;
	if (s->role != RADIO_ROLE_SLAVE)
		return false;
	if (s->clock.delay_req_pending)
		return false;

	memset(out, 0, sizeof(*out));
	out->requester_node_id = s->own_node_id;
	out->target_node_id = s->current_master_id;
	out->t3_us = now_us;
	out->superframe_seq = s->superframe_seq;

	clock_sync_prepare_delay_req(&s->clock, now_us, s->superframe_seq);
	s->delay_req_tx_count++;
	return true;
}

/* ================================================================
 * Tick: advance FSM, check timeouts
 * ================================================================ */

radio_role_t radio_sync_tick(radio_sync_t *s, int64_t now_us)
{
	if (s == NULL)
		return RADIO_ROLE_CANDIDATE;

	switch (s->role) {
	case RADIO_ROLE_CANDIDATE:
		if (s->election_deadline_us == 0)
			s->election_deadline_us = now_us + SYNC_ELECTION_TIMEOUT_US;
		if (now_us >= s->election_deadline_us) {
			s->role = RADIO_ROLE_MASTER;
			s->state = SYNC_STATE_MASTER_TX;
			s->current_master_id = s->own_node_id;
			s->superframe_seq = 0;
			s->elections_won++;
			s->role_changes++;
			fprintf(stderr, "sync: node %u became MASTER\n",
				s->own_node_id);
		}
		break;

	case RADIO_ROLE_MASTER:
		break;

	case RADIO_ROLE_SLAVE: {
		if (s->last_sync_rx_us == 0)
			break;

		int64_t expected = (int64_t)s->dl_duration_us +
			(int64_t)s->num_slots *
			((int64_t)s->ul_slot_us + s->guard_us) +
			s->guard_us;
		if (expected < SYNC_BEACON_INTERVAL_US)
			expected = SYNC_BEACON_INTERVAL_US;

		int64_t elapsed = now_us - s->last_sync_rx_us;

		if (elapsed > expected * SYNC_MISS_THRESHOLD) {
			s->missed_beacons++;
			s->last_sync_rx_us = now_us;

			if (s->missed_beacons >= SYNC_LOST_THRESHOLD) {
				s->role = RADIO_ROLE_CANDIDATE;
				s->state = SYNC_STATE_SEARCHING;
				s->election_deadline_us =
					now_us + SYNC_ELECTION_TIMEOUT_US;
				s->current_master_id = 0;
				s->role_changes++;
				s->missed_beacons = 0;
				fprintf(stderr,
					"sync: SYNC lost, back to CANDIDATE\n");
			}
		}
		break;
	}
	}

	return s->role;
}

/* ================================================================
 * Relay: prepare SYNC for retransmission
 * ================================================================ */

bool radio_sync_prepare_relay(radio_sync_t *s,
                               const sync_frame_t *rx_frame,
                               sync_frame_t *relay_frame,
                               int64_t local_tx_us)
{
	if (s == NULL || rx_frame == NULL || relay_frame == NULL)
		return false;
	if (s->role != RADIO_ROLE_SLAVE)
		return false;
	if (!s->clock.synced)
		return false;

	*relay_frame = *rx_frame;

	relay_frame->origin_time_us =
		clock_sync_to_master(&s->clock, local_tx_us);
	relay_frame->sender_node_id = s->own_node_id;
	relay_frame->relay_hops = rx_frame->relay_hops + 1;
	relay_frame->num_delay_resp = 0;

	s->sync_relay_count++;
	return true;
}

/* ================================================================
 * Queries
 * ================================================================ */

radio_role_t radio_sync_get_role(const radio_sync_t *s)
{
	if (s == NULL)
		return RADIO_ROLE_CANDIDATE;
	return s->role;
}

bool radio_sync_is_synced(const radio_sync_t *s)
{
	if (s == NULL)
		return false;
	return s->role == RADIO_ROLE_SLAVE &&
	       s->state == SYNC_STATE_SYNCED;
}

int64_t radio_sync_get_offset(const radio_sync_t *s)
{
	if (s == NULL)
		return 0;
	return s->clock.offset_us;
}
