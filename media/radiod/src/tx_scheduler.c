#include "radiod/tx_scheduler.h"

#include <string.h>

/*
 * Quick-peek a queued blob's ULAMA fragment identity (src/dst/traffic_class
 * are irrelevant here — only fragmentation fields matter for the eviction
 * policy below). Mirrors the header layout in ulama_frame.c's pack routine;
 * returns false for anything that isn't a recognizable ULAMA fragment (e.g.
 * a non-ULAMA/BULK blob), in which case callers fall back to treating the
 * slot as a standalone, safely-evictable unit.
 */
static bool peek_fragment_identity(const uint8_t *data, size_t len,
				   uint16_t *seq, bool *is_last)
{
	if (data == NULL || len < ULAMA_FRAME_HEADER_SIZE)
		return false;
	if (data[0] != ULAMA_FRAME_MAGIC)
		return false;
	if (!(data[4] & ULAMA_FLAG_FRAGMENT))
		return false;

	*seq = (uint16_t)data[6] | ((uint16_t)data[7] << 8);
	*is_last = (data[4] & ULAMA_FLAG_LAST_FRAGMENT) != 0;
	return true;
}

void radio_tx_scheduler_init(radio_tx_scheduler_t *sched)
{
	if (sched == NULL)
		return;
	memset(sched, 0, sizeof(*sched));
}

int radio_tx_enqueue(radio_tx_scheduler_t *sched,
		     uint8_t priority, uint8_t reliability,
		     const uint8_t *data, size_t len)
{
	radio_tx_queue_t *q;
	radio_tx_slot_t *slot;
	uint16_t seq;
	bool is_last;

	if (sched == NULL || data == NULL || len == 0)
		return -1;
	if (priority >= RADIO_PRIO_COUNT)
		priority = RADIO_PRIO_BULK;
	if (len > RADIO_TX_MAX_FRAME)
		return -1;

	q = &sched->queues[priority];

	while (q->count >= RADIO_TX_QUEUE_SIZE) {
		/*
		 * Overflow policy is priority-dependent:
		 *
		 *   VIDEO / BULK  → drop the OLDEST queued FRAME, then admit the new one.
		 *   CTRL  / TELEM → reject the new frame (keep the queued order intact).
		 *
		 * For the real-time video stream the freshest frame is the valuable one:
		 * the oldest queued frame is already stale and, on the receiver, its
		 * sequence has usually been delivered or skipped long ago — so
		 * discarding it rarely punches a NEW hole. The previous "reject newest"
		 * behaviour did the opposite: under a UL burst it dropped the live frame
		 * the ground station was actively waiting for, which tripped the gateway
		 * reorder gate → keyframe-wait stall → the visible video stutter. CTRL is
		 * tiny, reliable and must not be reordered, so it keeps reject-newest.
		 *
		 * A frame is a run of same-seq fragments (vcpd enqueues each frame's
		 * fragments contiguously, see tx_video_frame_pass()). Evicting a SINGLE
		 * fragment from the middle of that run — the old behaviour — can slice
		 * a frame in half: the receiver's reassembly then sees e.g. fragments
		 * 2..4 of a 5-fragment frame with 0..1 silently missing, which is NOT a
		 * clean drop but a corrupted "half frame" once the remaining fragments
		 * complete the (wrong) expected count. So eviction must be atomic per
		 * frame, and a frame that has already started transmitting (dirty —
		 * some of its fragments are already dequeued, maybe already on the air)
		 * must never have its remaining queued fragments evicted out from
		 * under it; reject the new arrival instead in that rare case.
		 */
		if (priority != RADIO_PRIO_VIDEO && priority != RADIO_PRIO_BULK) {
			q->dropped++;
			return -1;
		}

		radio_tx_slot_t *tail_slot = &q->slots[q->tail];
		bool tail_dirty = q->dirty_active && tail_slot->is_fragment &&
				  tail_slot->frame_seq == q->dirty_frame_seq;
		if (tail_dirty) {
			q->dropped++;
			return -1;
		}

		bool evict_valid = tail_slot->is_fragment;
		uint16_t evict_seq = tail_slot->frame_seq;
		do {
			q->tail = (uint16_t)((q->tail + 1U) % RADIO_TX_QUEUE_SIZE);
			q->count--;
			q->dropped++;
		} while (q->count > 0 && evict_valid &&
			 q->slots[q->tail].is_fragment &&
			 q->slots[q->tail].frame_seq == evict_seq);
	}

	slot = &q->slots[q->head];
	memcpy(slot->data, data, len);
	slot->len = len;
	slot->reliability = reliability;
	slot->has_dst_mac = false;
	slot->is_fragment = peek_fragment_identity(data, len, &seq, &is_last);
	slot->frame_seq = slot->is_fragment ? seq : 0;
	slot->is_last_fragment = slot->is_fragment ? is_last : true;

	q->head = (uint16_t)((q->head + 1U) % RADIO_TX_QUEUE_SIZE);
	q->count++;
	q->enqueued++;
	return 0;
}

int radio_tx_enqueue_relay(radio_tx_scheduler_t *sched,
			  uint8_t priority, uint8_t reliability,
			  const uint8_t *data, size_t len,
			  const uint8_t dst_mac[6])
{
	int rc = radio_tx_enqueue(sched, priority, reliability, data, len);
	if (rc == 0 && dst_mac != NULL) {
		radio_tx_queue_t *q = &sched->queues[priority >= RADIO_PRIO_COUNT
						      ? RADIO_PRIO_BULK : priority];
		/* The slot we just wrote is at (head - 1) */
		uint16_t idx = (uint16_t)((q->head + RADIO_TX_QUEUE_SIZE - 1U)
					   % RADIO_TX_QUEUE_SIZE);
		radio_tx_slot_t *slot = &q->slots[idx];
		memcpy(slot->dst_mac, dst_mac, 6);
		slot->has_dst_mac = true;
	}
	return rc;
}

const radio_tx_slot_t *radio_tx_dequeue(radio_tx_scheduler_t *sched,
					uint8_t *out_priority)
{
	if (sched == NULL)
		return NULL;

	/* Strict priority: always drain higher priority first */
	for (uint8_t p = 0; p < RADIO_PRIO_COUNT; p++) {
		radio_tx_queue_t *q = &sched->queues[p];
		if (q->count == 0)
			continue;

		radio_tx_slot_t *slot = &q->slots[q->tail];
		q->tail = (uint16_t)((q->tail + 1U) % RADIO_TX_QUEUE_SIZE);
		q->count--;

		/* Mark/clear "in flight" so a later overflow in radio_tx_enqueue()
		 * knows this frame's remaining queued fragments (if any) must not
		 * be evicted — see the eviction comment there. */
		if (slot->is_fragment) {
			if (!slot->is_last_fragment) {
				q->dirty_active = true;
				q->dirty_frame_seq = slot->frame_seq;
			} else if (q->dirty_active && q->dirty_frame_seq == slot->frame_seq) {
				q->dirty_active = false;
			}
		}

		if (out_priority != NULL)
			*out_priority = p;
		return slot;
	}
	return NULL;
}

const radio_tx_slot_t *radio_tx_peek(const radio_tx_scheduler_t *sched,
				     uint8_t priority)
{
	const radio_tx_queue_t *q;

	if (sched == NULL || priority >= RADIO_PRIO_COUNT)
		return NULL;

	q = &sched->queues[priority];
	if (q->count == 0)
		return NULL;

	return &q->slots[q->tail];
}

int radio_tx_pending(const radio_tx_scheduler_t *sched)
{
	int total = 0;

	if (sched == NULL)
		return 0;

	for (int p = 0; p < RADIO_PRIO_COUNT; p++)
		total += sched->queues[p].count;

	return total;
}

int radio_tx_queue_depth(const radio_tx_scheduler_t *sched, uint8_t priority)
{
	if (sched == NULL || priority >= RADIO_PRIO_COUNT)
		return 0;
	return sched->queues[priority].count;
}
