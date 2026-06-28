#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ulama/ulama_frame.h"
#include "radiod/ipc.h"

/* -------------------------------------------------------------------
 * TX Scheduler — priority-based packet queuing for TDMA.
 *
 * Four priority queues (ring buffers):
 *   P0 CTRL     — always flushed first, bypasses rate limit
 *   P1 TELEM    — telemetry
 *   P2 VIDEO    — video stream, fills remaining TX budget
 *   P3 BULK     — OSD, firmware updates
 *
 * Dequeue order: P0 fully → P1 → P2 → P3.
 * ------------------------------------------------------------------- */

#define RADIO_TX_QUEUE_SIZE  64
#define RADIO_TX_MAX_FRAME   (ULAMA_FRAME_HEADER_SIZE + ULAMA_FRAME_MAX_PAYLOAD)

/* ---- Single TX slot in a priority queue ---- */

typedef struct {
	uint8_t  data[RADIO_TX_MAX_FRAME];
	size_t   len;
	uint8_t  reliability;  /* 0=fire-and-forget, 1=ACK+retry */
	bool     has_dst_mac;  /* true = use dst_mac instead of default */
	uint8_t  dst_mac[6];   /* next-hop MAC for relay packets      */
} radio_tx_slot_t;

/* ---- Per-priority ring buffer ---- */

typedef struct {
	radio_tx_slot_t slots[RADIO_TX_QUEUE_SIZE];
	uint16_t head;  /* next write position */
	uint16_t tail;  /* next read position  */
	uint16_t count;
	uint32_t enqueued;
	uint32_t dropped;
} radio_tx_queue_t;

/* ---- TX Scheduler context ---- */

typedef struct {
	radio_tx_queue_t queues[RADIO_PRIO_COUNT];
} radio_tx_scheduler_t;

void radio_tx_scheduler_init(radio_tx_scheduler_t *sched);

/*
 * Enqueue a packet into the appropriate priority queue.
 * Returns 0 on success, -1 if the queue is full (packet dropped).
 */
int  radio_tx_enqueue(radio_tx_scheduler_t *sched,
		      uint8_t priority, uint8_t reliability,
		      const uint8_t *data, size_t len);

/*
 * Dequeue the highest-priority packet available.
 * Returns pointer to internal slot (valid until next dequeue), or NULL.
 * Sets *out_priority to the priority level of the dequeued packet.
 */
const radio_tx_slot_t *radio_tx_dequeue(radio_tx_scheduler_t *sched,
					uint8_t *out_priority);

/* Enqueue with explicit next-hop MAC (for relay packets). */
int  radio_tx_enqueue_relay(radio_tx_scheduler_t *sched,
			    uint8_t priority, uint8_t reliability,
			    const uint8_t *data, size_t len,
			    const uint8_t dst_mac[6]);

/* Peek at the head of a specific priority queue without removing. */
const radio_tx_slot_t *radio_tx_peek(const radio_tx_scheduler_t *sched,
				     uint8_t priority);

/* Total number of packets across all queues. */
int  radio_tx_pending(const radio_tx_scheduler_t *sched);

/* Number of packets in a specific priority queue. */
int  radio_tx_queue_depth(const radio_tx_scheduler_t *sched, uint8_t priority);
