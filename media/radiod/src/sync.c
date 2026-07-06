#include "radiod/sync.h"

#include <stdio.h>
#include <string.h>

static int64_t compute_base_superframe_period_us(const radio_sync_t *s)
{
	int64_t period;

	if (s == NULL)
		return SYNC_BEACON_INTERVAL_US;

	period = (int64_t)s->dl_duration_us +
		(int64_t)s->num_slots * ((int64_t)s->ul_slot_us + s->guard_us) +
		s->guard_us;
	if (period < SYNC_BEACON_INTERVAL_US)
		period = SYNC_BEACON_INTERVAL_US;
	return period;
}

static bool bootstrap_window_active_for_seq(const radio_sync_t *s, uint32_t seq)
{
	if (s == NULL || seq == 0)
		return false;
	if (s->bootstrap_window_us == 0 || s->bootstrap_period == 0)
		return false;
	return (seq % s->bootstrap_period) == 0;
}

static int64_t compute_superframe_period_us(const radio_sync_t *s, uint32_t seq)
{
	int64_t period = compute_base_superframe_period_us(s);

	if (bootstrap_window_active_for_seq(s, seq))
		period += s->bootstrap_window_us;
	return period;
}

static int64_t clamp_period_correction(int64_t correction_us)
{
	if (correction_us > SYNC_PERIOD_CORR_MAX_US)
		return SYNC_PERIOD_CORR_MAX_US;
	if (correction_us < -SYNC_PERIOD_CORR_MAX_US)
		return -SYNC_PERIOD_CORR_MAX_US;
	return correction_us;
}

static int64_t limit_period_correction_step(int64_t prev_correction_us,
					     int64_t target_correction_us)
{
	int64_t delta = target_correction_us - prev_correction_us;

	if (delta > SYNC_PLL_CORR_STEP_MAX_US)
		delta = SYNC_PLL_CORR_STEP_MAX_US;
	else if (delta < -SYNC_PLL_CORR_STEP_MAX_US)
		delta = -SYNC_PLL_CORR_STEP_MAX_US;

	return prev_correction_us + delta;
}

static int64_t compute_slave_superframe_period_us(const radio_sync_t *s, uint32_t seq)
{
	int64_t period = compute_superframe_period_us(s, seq);

	if (s == NULL)
		return period;
	if (!s->period_correction_valid)
		return period;

	period += clamp_period_correction(s->period_correction_us);
	if (period < SYNC_BEACON_INTERVAL_US)
		period = SYNC_BEACON_INTERVAL_US;
	return period;
}

static void update_slave_phase_pll(radio_sync_t *s, int64_t raw_phase_error_us)
{
	int64_t prev_correction_us;
	int64_t target_correction_us;

	if (s == NULL)
		return;

	prev_correction_us = s->period_correction_valid
		? s->period_correction_us : 0;

	if (!s->period_correction_valid) {
		s->filtered_phase_error_us = raw_phase_error_us;
		s->period_correction_valid = true;
	} else {
		s->filtered_phase_error_us +=
			(raw_phase_error_us - s->filtered_phase_error_us) >> SYNC_PLL_ERROR_EMA_SHIFT;
	}
	target_correction_us = clamp_period_correction(
		s->filtered_phase_error_us >> SYNC_PLL_CORR_SHIFT);
	s->period_correction_us = limit_period_correction_step(prev_correction_us,
		target_correction_us);
}

static bool should_update_slave_phase_pll(const radio_sync_t *s,
					      int64_t phase_error_us,
					      const sync_frame_t *frame,
					      uint32_t prev_seq)
{
	if (s == NULL || frame == NULL)
		return false;
	if (s->missed_beacons != 0)
		return false;
	if (frame->superframe_seq != prev_seq + 1U)
		return false;
	if (frame->dl_duration_us != s->dl_duration_us)
		return false;
	if (frame->ul_slot_us != s->ul_slot_us)
		return false;
	if (frame->guard_us != s->guard_us)
		return false;
	if (frame->num_slots != s->num_slots)
		return false;
	if (memcmp(frame->slot_map, s->slot_map, SYNC_MAX_SLOTS) != 0)
		return false;
	if (phase_error_us > SYNC_PLL_PHASE_GATE_US ||
	    phase_error_us < -SYNC_PLL_PHASE_GATE_US)
		return false;
	return true;
}

static void reset_to_candidate(radio_sync_t *s, int64_t now_us, const char *reason)
{
	if (s == NULL)
		return;

	s->role = RADIO_ROLE_CANDIDATE;
	s->state = SYNC_STATE_SEARCHING;
	s->election_deadline_us = now_us + SYNC_ELECTION_TIMEOUT_US;
	s->current_master_id = 0;
	s->missed_beacons = 0;
	s->last_sync_rx_us = 0;
	s->last_beacon_phase_error_us = 0;
	s->last_beacon_phase_error_valid = false;
	s->filtered_phase_error_us = 0;
	s->predicted_anchor_us = 0;
	s->superframe_period_us = 0;
	s->period_correction_us = 0;
	s->period_correction_valid = false;
	s->next_superframe_us = 0;
	s->my_ul_start_us = 0;
	s->my_ul_end_us = 0;
	s->my_slot_index = 0xFF;
	s->role_changes++;

	if (reason != NULL)
		fprintf(stderr, "sync: %s, back to CANDIDATE\n", reason);
}

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
	s->bootstrap_window_us = SYNC_BOOTSTRAP_WINDOW_US;
	s->bootstrap_period = SYNC_BOOTSTRAP_PERIOD;
	s->my_slot_index = 0xFF;
	s->superframe_period_us = compute_base_superframe_period_us(s);
	clock_sync_init(&s->clock);
}

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
			s->num_known_slaves++;
			fprintf(stderr, "sync: node %u joined (known=%u)\n",
				node_id, s->num_known_slaves);
			return &s->slaves[i];
		}
	}
	return NULL;
}

static void update_slot_map_entries(radio_sync_t *s,
				    const uint8_t *active_ids,
				    uint8_t n_active)
{
	uint8_t other_ids[SYNC_MAX_NODES];
	uint8_t n_others = 0;
	uint8_t priority_id = 0;
	bool priority_present = false;

	memset(s->slot_map, 0, sizeof(s->slot_map));
	if (n_active == 0) {
		s->num_slots = 0;
		return;
	}

	for (uint8_t i = 0; i < n_active; i++) {
		if (!priority_present && active_ids[i] == SYNC_PRIORITY_NODE_ID) {
			priority_id = active_ids[i];
			priority_present = true;
			continue;
		}
		other_ids[n_others++] = active_ids[i];
	}

	/* Give the video-priority node a second UL slot only when there is at least
	 * one OTHER active slave to separate the copies. With a single active slave,
	 * duplicating its slot back-to-back only stretches every superframe while
	 * giving no spacing benefit. */
	if (!priority_present || SYNC_PRIORITY_SLOT_WEIGHT <= 1U || n_others == 0U) {
		s->num_slots = n_active < SYNC_MAX_SLOTS ? n_active : SYNC_MAX_SLOTS;
		for (uint8_t i = 0; i < s->num_slots; i++)
			s->slot_map[i] = active_ids[i];
		return;
	}

	s->num_slots = (uint8_t)(n_active + (SYNC_PRIORITY_SLOT_WEIGHT - 1U));
	if (s->num_slots > SYNC_MAX_SLOTS)
		s->num_slots = SYNC_MAX_SLOTS;

	uint8_t priority_left = SYNC_PRIORITY_SLOT_WEIGHT;
	uint8_t others_emitted = 0;
	uint8_t other_start = n_others > 0
		? (uint8_t)(s->superframe_seq % n_others)
		: 0;
	bool priority_turn = true;

	for (uint8_t slot = 0; slot < s->num_slots; ) {
		if (priority_turn && priority_left > 0) {
			s->slot_map[slot++] = priority_id;
			priority_left--;
			priority_turn = false;
			continue;
		}
		if (n_others > 0 && others_emitted < n_others) {
			s->slot_map[slot++] =
				other_ids[(other_start + others_emitted) % n_others];
			others_emitted++;
			priority_turn = true;
			continue;
		}
		if (priority_left > 0) {
			s->slot_map[slot++] = priority_id;
			priority_left--;
			continue;
		}
		if (n_others > 0) {
			s->slot_map[slot++] =
				other_ids[(other_start + (others_emitted % n_others)) % n_others];
			others_emitted++;
			continue;
		}
		break;
	}
}

bool radio_sync_on_sync_rx(radio_sync_t *s,
			   const sync_frame_t *frame,
			   int64_t local_rx_us,
			   uint8_t sender_node_id)
{
	uint32_t prev_seq;
	bool had_prediction;
	bool allow_pll_update = false;
	int64_t phase_error_us = 0;

	(void)sender_node_id;

	if (s == NULL || frame == NULL)
		return false;
	if (frame->master_node_id == s->own_node_id)
		return false;
	if (frame->master_node_id < s->own_node_id)
		return false;
	if (dedup_seen(s, frame->master_node_id, frame->superframe_seq))
		return false;

	if (s->role == RADIO_ROLE_MASTER) {
		fprintf(stderr, "sync: yielding master to node %u\n",
			frame->master_node_id);
		s->elections_lost++;
		s->role_changes++;
	} else if (s->role == RADIO_ROLE_CANDIDATE) {
		s->role_changes++;
	}

	prev_seq = s->superframe_seq;
	had_prediction = (s->role == RADIO_ROLE_SLAVE) && (s->next_superframe_us != 0);
	if (had_prediction)
		phase_error_us = local_rx_us - s->next_superframe_us;
	allow_pll_update = had_prediction &&
		should_update_slave_phase_pll(s, phase_error_us, frame, prev_seq);

	s->role = RADIO_ROLE_SLAVE;
	s->state = SYNC_STATE_SYNCED;
	s->current_master_id = frame->master_node_id;
	s->election_deadline_us = 0;
	s->missed_beacons = 0;
	s->last_sync_rx_us = local_rx_us;
	s->last_beacon_phase_error_us = phase_error_us;
	s->last_beacon_phase_error_valid = had_prediction;
	/* Hard phase correction: every accepted master beacon becomes the new
	 * superframe anchor immediately. The PLL only adjusts the period used for
	 * predicting the NEXT beacon; it does not preserve a stale phase offset. */
	s->predicted_anchor_us = s->last_sync_rx_us;

	s->dl_duration_us = frame->dl_duration_us;
	s->ul_slot_us = frame->ul_slot_us;
	s->guard_us = frame->guard_us;
	s->num_slots = frame->num_slots;
	s->bootstrap_window_us = frame->bootstrap_window_us;
	s->bootstrap_period = frame->bootstrap_period;
	memcpy(s->slot_map, frame->slot_map, SYNC_MAX_SLOTS);
	s->superframe_seq = frame->superframe_seq;
	if (allow_pll_update)
		update_slave_phase_pll(s, phase_error_us);
	s->superframe_period_us = compute_slave_superframe_period_us(s,
						      s->superframe_seq);
	s->next_superframe_us = s->predicted_anchor_us + s->superframe_period_us;

	s->my_slot_index = 0xFF;
	for (uint8_t i = 0; i < s->num_slots; i++) {
		if (s->slot_map[i] == s->own_node_id) {
			s->my_slot_index = i;
			break;
		}
	}

	(void)clock_sync_apply_master_time(&s->clock, frame->master_time_us);
	s->sync_rx_count++;
	return true;
}

void radio_sync_on_delay_req_rx(radio_sync_t *s,
				const delay_req_frame_t *dreq,
				int64_t local_rx_us)
{
	static uint32_t dreq_trace_count;

	if (s == NULL || dreq == NULL)
		return;
	if (s->role != RADIO_ROLE_MASTER) {
		if (dreq_trace_count < 32 || (dreq_trace_count % 128U) == 0U) {
			fprintf(stderr,
				"sync: drop dreq requester=%u target=%u seq=%u role=%u\n",
				dreq->requester_node_id,
				dreq->target_node_id,
				dreq->superframe_seq,
				(unsigned)s->role);
		}
		dreq_trace_count++;
		return;
	}
	if (dreq->target_node_id != s->own_node_id) {
		if (dreq_trace_count < 32 || (dreq_trace_count % 128U) == 0U) {
			fprintf(stderr,
				"sync: drop dreq requester=%u target=%u own=%u seq=%u\n",
				dreq->requester_node_id,
				dreq->target_node_id,
				s->own_node_id,
				dreq->superframe_seq);
		}
		dreq_trace_count++;
		return;
	}

	sync_slave_info_t *sl = find_slave(s, dreq->requester_node_id);
	if (sl == NULL)
		sl = alloc_slave(s, dreq->requester_node_id, local_rx_us);
	if (sl == NULL) {
		if (dreq_trace_count < 32 || (dreq_trace_count % 128U) == 0U) {
			fprintf(stderr,
				"sync: drop dreq requester=%u target=%u seq=%u no-slave-slot known=%u\n",
				dreq->requester_node_id,
				dreq->target_node_id,
				dreq->superframe_seq,
				s->num_known_slaves);
		}
		dreq_trace_count++;
		return;
	}

	sl->last_seen_us = local_rx_us;
	if (dreq_trace_count < 32 || (dreq_trace_count % 128U) == 0U) {
		fprintf(stderr,
			"sync: accept dreq requester=%u target=%u seq=%u known=%u active=%u last_seen=%lld\n",
			dreq->requester_node_id,
			dreq->target_node_id,
			dreq->superframe_seq,
			s->num_known_slaves,
			(unsigned)sl->active,
			(long long)local_rx_us);
	}
	dreq_trace_count++;
}

void radio_sync_on_ul_packet_rx(radio_sync_t *s,
				uint8_t src_node,
				int64_t local_rx_us)
{
	if (s == NULL)
		return;
	if (s->role != RADIO_ROLE_MASTER)
		return;
	if (src_node == 0 || src_node == s->own_node_id)
		return;

	sync_slave_info_t *sl = find_slave(s, src_node);
	if (sl == NULL)
		return;

	sl->last_seen_us = local_rx_us;
}

void radio_sync_build_beacon(radio_sync_t *s,
			      sync_frame_t *out_frame,
			      int64_t now_us)
{
	(void)now_us;

	if (s == NULL || out_frame == NULL)
		return;

	memset(out_frame, 0, sizeof(*out_frame));

	s->superframe_seq++;

	out_frame->master_node_id = s->own_node_id;
	out_frame->sender_node_id = s->own_node_id;
	out_frame->superframe_seq = s->superframe_seq;
	out_frame->master_time_us = 0;
	out_frame->dl_duration_us = s->dl_duration_us;
	out_frame->ul_slot_us = s->ul_slot_us;
	out_frame->guard_us = s->guard_us;
	out_frame->num_slots = s->num_slots;
	out_frame->relay_hops = 0;
	memcpy(out_frame->slot_map, s->slot_map, SYNC_MAX_SLOTS);
	out_frame->bootstrap_window_us = s->bootstrap_window_us;
	out_frame->bootstrap_period = s->bootstrap_period;
	s->superframe_period_us = compute_superframe_period_us(s, s->superframe_seq);

	s->sync_tx_count++;
}

void radio_sync_update_slot_map(radio_sync_t *s, int64_t now_us)
{
	if (s == NULL)
		return;

	uint8_t active_ids[SYNC_MAX_NODES];
	uint8_t n_active = 0;
	int64_t base_period;
	int64_t bootstrap_gap;
	int64_t stale_threshold;

	base_period = compute_base_superframe_period_us(s);
	bootstrap_gap = (s->bootstrap_period > 0)
		? (base_period * (int64_t)s->bootstrap_period) + s->bootstrap_window_us
		: 0;
	s->superframe_period_us = base_period;
	if (s->bootstrap_window_us > 0)
		s->superframe_period_us += s->bootstrap_window_us;
	stale_threshold = s->superframe_period_us * 5;
	if (bootstrap_gap > 0 && stale_threshold < bootstrap_gap + base_period)
		stale_threshold = bootstrap_gap + base_period;
	if (stale_threshold < SYNC_SLAVE_TIMEOUT_US)
		stale_threshold = SYNC_SLAVE_TIMEOUT_US;
	if (stale_threshold < 60000)
		stale_threshold = 60000;

	for (uint8_t i = 0; i < SYNC_MAX_NODES; i++) {
		if (!s->slaves[i].active)
			continue;
		if (now_us - s->slaves[i].last_seen_us > stale_threshold) {
			fprintf(stderr,
				"sync: node %u timed out (silent %lld us > "
				"%lld us), known=%u\n",
				s->slaves[i].node_id,
				(long long)(now_us - s->slaves[i].last_seen_us),
				(long long)stale_threshold,
				s->num_known_slaves > 0 ? s->num_known_slaves - 1 : 0);
			s->slaves[i].active = false;
			if (s->num_known_slaves > 0)
				s->num_known_slaves--;
			continue;
		}
		if (n_active < SYNC_MAX_NODES)
			active_ids[n_active++] = s->slaves[i].node_id;
	}

	for (uint8_t i = 1; i < n_active; i++) {
		uint8_t key = active_ids[i];
		int j = (int)i - 1;
		while (j >= 0 && active_ids[j] > key) {
			active_ids[j + 1] = active_ids[j];
			j--;
		}
		active_ids[j + 1] = key;
	}

	update_slot_map_entries(s, active_ids, n_active);
	s->superframe_period_us = compute_superframe_period_us(s, s->superframe_seq);
}

void radio_sync_compute_timing(radio_sync_t *s, int64_t local_now_us)
{
	if (s == NULL)
		return;

	int64_t anchor = s->predicted_anchor_us;
	if (anchor == 0)
		anchor = s->last_sync_rx_us != 0 ? s->last_sync_rx_us : local_now_us;

	s->superframe_period_us = compute_slave_superframe_period_us(s,
						      s->superframe_seq);
	s->dl_start_us = anchor;
	s->dl_end_us = s->dl_start_us + s->dl_duration_us;

	int64_t ul_base = s->dl_end_us + s->guard_us;

	s->my_ul_start_us = 0;
	s->my_ul_end_us = 0;
	s->my_slot_index = 0xFF;

	for (uint8_t i = 0; i < s->num_slots; i++) {
		int64_t slot_start = ul_base +
			(int64_t)i * ((int64_t)s->ul_slot_us + s->guard_us);
		if (s->slot_map[i] == s->own_node_id) {
			s->my_ul_start_us = slot_start;
			s->my_ul_end_us = slot_start + s->ul_slot_us;
			s->my_slot_index = i;
			break;
		}
	}

	s->next_superframe_us = anchor + s->superframe_period_us;
}

bool radio_sync_build_delay_req(radio_sync_t *s,
				 delay_req_frame_t *out,
				 int64_t now_us)
{
	(void)now_us;

	if (s == NULL || out == NULL)
		return false;
	if (s->role != RADIO_ROLE_SLAVE)
		return false;
	if (s->current_master_id == 0)
		return false;
	if (s->my_slot_index != 0xFF &&
	    (s->superframe_seq % SYNC_DELAY_REQ_KEEPALIVE_PERIOD) != 0U)
		return false;

	memset(out, 0, sizeof(*out));
	out->requester_node_id = s->own_node_id;
	out->target_node_id = s->current_master_id;
	out->superframe_seq = s->superframe_seq;
	s->delay_req_tx_count++;
	return true;
}

void radio_sync_on_beacon_timeout(radio_sync_t *s, int64_t now_us)
{
	if (s == NULL || s->role != RADIO_ROLE_SLAVE)
		return;

	if (s->predicted_anchor_us == 0)
		s->predicted_anchor_us =
			s->last_sync_rx_us != 0 ? s->last_sync_rx_us : now_us;

	/* Holdover advances using the current PLL-corrected period estimate so a
	 * few missed beacons do not instantly throw the slave off cadence. */
	int64_t elapsed_period = compute_slave_superframe_period_us(s,
					     s->superframe_seq);
	s->predicted_anchor_us += elapsed_period;
	s->superframe_seq++;
	s->superframe_period_us = compute_slave_superframe_period_us(s,
						      s->superframe_seq);
	s->next_superframe_us = s->predicted_anchor_us + s->superframe_period_us;
	s->missed_beacons++;

	if (s->missed_beacons >= SYNC_LOST_THRESHOLD) {
		reset_to_candidate(s, now_us, "SYNC lost");
		return;
	}

	if (s->missed_beacons <= SYNC_HOLDOVER_TX_MAX)
		s->state = SYNC_STATE_HOLDOVER_TX;
	else
		s->state = SYNC_STATE_HOLDOVER_RX_ONLY;
}

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

	case RADIO_ROLE_SLAVE:
		break;
	}

	return s->role;
}

bool radio_sync_prepare_relay(radio_sync_t *s,
			       const sync_frame_t *rx_frame,
			       sync_frame_t *relay_frame,
			       int64_t local_tx_us)
{
	(void)local_tx_us;

	if (s == NULL || rx_frame == NULL || relay_frame == NULL)
		return false;
	if (s->role != RADIO_ROLE_SLAVE)
		return false;
	if (!radio_sync_is_synced(s))
		return false;

	*relay_frame = *rx_frame;
	relay_frame->sender_node_id = s->own_node_id;
	relay_frame->relay_hops = rx_frame->relay_hops + 1;

	s->sync_relay_count++;
	return true;
}

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
	if (s->role != RADIO_ROLE_SLAVE)
		return false;
	return s->state == SYNC_STATE_SYNCED ||
	       s->state == SYNC_STATE_HOLDOVER_TX ||
	       s->state == SYNC_STATE_HOLDOVER_RX_ONLY;
}

bool radio_sync_should_transmit_ul(const radio_sync_t *s)
{
	if (!radio_sync_is_synced(s))
		return false;
	return s->state == SYNC_STATE_SYNCED ||
	       s->state == SYNC_STATE_HOLDOVER_TX;
}

int64_t radio_sync_get_offset(const radio_sync_t *s)
{
	(void)s;
	return 0;
}