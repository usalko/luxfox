#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radiod/ipc.h"

/* -------------------------------------------------------------------
 * RX Dispatcher — receives frames from radio, dispatches to clients.
 *
 * During the RX slot, calls pcap_next_ex in a loop:
 *   ACK        → internal: clear async retry slot
 *   DATA_SEQ   → send ACK back, dispatch payload to IPC clients
 *   DATA       → dispatch payload to IPC clients
 *
 * All clients receive all frames; they filter by traffic_class
 * themselves.
 * ------------------------------------------------------------------- */

#define RADIO_DEDUP_WINDOW   64
#define RADIO_ASYNC_SLOTS    32

/* Async slot: pending reliable TX awaiting ACK */
typedef struct {
	uint8_t  frame[2400];  /* full wire frame for pcap_inject retx  */
	size_t   frame_len;
	uint16_t seq;
	int64_t  sent_us;
	uint8_t  attempts_left;
	bool     active;
} radio_async_slot_t;

/* RX statistics */
typedef struct {
	uint32_t rx_total;
	uint32_t rx_self_dropped;
	uint32_t rx_ack;
	uint32_t rx_data;
	uint32_t rx_data_seq;
	uint32_t rx_dispatched;
	uint32_t rx_parse_fail;
	uint32_t tx_ack_ok;
	uint32_t tx_ack_timeout;
	uint32_t tx_retries;
	uint32_t rx_ack_sent;
	uint32_t rx_dedup_dropped;
} radio_rx_stats_t;

/* RX Dispatcher context */
typedef struct {
	radio_ipc_server_t *ipc;

	/* Dedup ring for DATA_SEQ frames */
	uint16_t dedup_ring[RADIO_DEDUP_WINDOW];
	uint16_t dedup_head;
	uint16_t dedup_count;

	/* Async reliable TX slots */
	radio_async_slot_t async_slots[RADIO_ASYNC_SLOTS];
	uint16_t           async_tx_seq;

	/* Statistics */
	radio_rx_stats_t stats;
} radio_rx_dispatcher_t;

void radio_rx_dispatcher_init(radio_rx_dispatcher_t *rxd,
			      radio_ipc_server_t *ipc);

/*
 * Run the RX slot: call pcap_next_ex in a loop until deadline.
 * Processes ACKs internally, dispatches data frames to IPC clients.
 * pcap_handle — the pcap handle owned by radiod
 * own_mac     — our MAC address (to filter self-sent frames)
 * deadline_us — absolute timestamp (µs) when RX slot ends
 */
struct pcap_pkthdr;

void radio_rx_slot(radio_rx_dispatcher_t *rxd,
		   void *pcap_handle,
		   const uint8_t own_mac[6],
		   int64_t deadline_us);

/*
 * Store a TX frame for reliable delivery (ACK tracking).
 * Called after pcap_inject of a DATA_SEQ frame.
 * Returns the async slot index, or -1 if all slots are full.
 */
int  radio_async_store(radio_rx_dispatcher_t *rxd,
		       const uint8_t *wire_frame, size_t frame_len,
		       uint16_t seq);

/* Tick async retries: resend timed-out frames, expire dead ones. */
void radio_async_tick(radio_rx_dispatcher_t *rxd,
		      void *pcap_handle,
		      uint32_t ack_timeout_us,
		      uint32_t max_retry);

/* Get current async TX sequence number (and increment). */
uint16_t radio_async_next_seq(radio_rx_dispatcher_t *rxd);
