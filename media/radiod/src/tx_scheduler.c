#include "radiod/tx_scheduler.h"

#include <string.h>

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

	if (sched == NULL || data == NULL || len == 0)
		return -1;
	if (priority >= RADIO_PRIO_COUNT)
		priority = RADIO_PRIO_BULK;
	if (len > RADIO_TX_MAX_FRAME)
		return -1;

	q = &sched->queues[priority];

	if (q->count >= RADIO_TX_QUEUE_SIZE) {
		q->dropped++;
		return -1;
	}

	slot = &q->slots[q->head];
	memcpy(slot->data, data, len);
	slot->len = len;
	slot->reliability = reliability;

	q->head = (uint16_t)((q->head + 1U) % RADIO_TX_QUEUE_SIZE);
	q->count++;
	q->enqueued++;
	return 0;
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
