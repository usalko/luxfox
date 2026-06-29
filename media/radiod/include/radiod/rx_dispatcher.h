#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radiod/ipc.h"
#include "radiod/route_table.h"

/* -------------------------------------------------------------------
 * RX Dispatcher — receives frames from radio, dispatches to clients.
 *
 * During the RX slot, calls pcap_next_ex in a loop:
 *   ACK        → internal: clear async retry slot
 *   DATA_SEQ   → send ACK back, route/dispatch/relay
 *   DATA       → route/dispatch/relay
 *
 * Routing decision per ULAMA frame:
 *   dst_node == own_node → dispatch to IPC clients
 *   dst_node == 0xFF     → dispatch to IPC clients AND relay
 *   dst_node == other    → relay only (don't dispatch locally)
 *
 * When relay is disabled, all frames are dispatched locally
 * regardless of dst_node (backward compatible).
 * ------------------------------------------------------------------- */

#define RADIO_DEDUP_WINDOW       64
#define RADIO_ULAMA_DEDUP_WINDOW 128
#define RADIO_ASYNC_SLOTS        32

/* Async slot: pending reliable TX awaiting ACK */
typedef struct {
	uint8_t  frame[2400];
	size_t   frame_len;
	uint16_t seq;
	int64_t  sent_us;
	uint8_t  attempts_left;
	bool     active;
} radio_async_slot_t;

/* ULAMA-level dedup key: (src_node, ulama_seq) */
typedef struct {
	uint8_t  src_node;
	uint16_t seq;
} radio_ulama_dedup_key_t;

/* RX statistics */
typedef struct {
	uint32_t rx_total;
	uint32_t rx_self_dropped;
	uint32_t rx_ack;
	uint32_t rx_data;
	uint32_t rx_data_seq;
	uint32_t rx_dispatched;
	uint32_t rx_parse_fail;
	uint32_t rx_pcap_error;  /* pcap_next_ex returned -1 */
	uint32_t tx_ack_ok;
	uint32_t tx_ack_timeout;
	uint32_t tx_retries;
	uint32_t rx_ack_sent;
	uint32_t rx_dedup_dropped;
	/* ULAMA-level dedup (mesh) */
	uint32_t rx_ulama_dedup_dropped;
	/* Relay counters */
	uint32_t relay_forwarded;
	uint32_t relay_dropped_ttl;
	uint32_t relay_by_prio[4];
} radio_rx_stats_t;

/* Opaque pointer — avoids circular include with tx_scheduler.h */
typedef struct radio_tx_scheduler_opaque radio_tx_scheduler_opaque_t;

/* RX Dispatcher context */
typedef struct {
	radio_ipc_server_t *ipc;

	/* UNOW-level dedup ring (by DATA_SEQ sequence) */
	uint16_t dedup_ring[RADIO_DEDUP_WINDOW];
	uint16_t dedup_head;
	uint16_t dedup_count;

	/* ULAMA-level dedup ring (by src_node + ulama_seq) */
	radio_ulama_dedup_key_t ulama_dedup_ring[RADIO_ULAMA_DEDUP_WINDOW];
	uint16_t ulama_dedup_head;
	uint16_t ulama_dedup_count;

	/* Async reliable TX slots */
	radio_async_slot_t async_slots[RADIO_ASYNC_SLOTS];
	uint16_t           async_tx_seq;

	/* Mesh relay configuration */
	bool                        relay_enabled;
	uint8_t                     own_node_id;
	void                       *relay_sched;  /* radio_tx_scheduler_t* */
	radio_route_table_t        *route_table;

	/* Per-cycle feedback for watchdog (reset each cycle) */
	uint32_t ctrl_for_us;

	/* Statistics */
	radio_rx_stats_t stats;
} radio_rx_dispatcher_t;

void radio_rx_dispatcher_init(radio_rx_dispatcher_t *rxd,
			      radio_ipc_server_t *ipc);

/* Configure mesh relay (call after init, before first rx_slot). */
/* sched is radio_tx_scheduler_t* — void* to avoid circular include */
void radio_rx_dispatcher_enable_relay(radio_rx_dispatcher_t *rxd,
				      uint8_t own_node_id,
				      void *sched,
				      radio_route_table_t *rt);

/*
 * Run the RX slot: call pcap_next_ex in a loop until deadline.
 * Processes ACKs, performs ULAMA routing (dispatch/relay/both).
 * Resets ctrl_for_us counter at start of each call.
 */
struct pcap_pkthdr;

void radio_rx_slot(radio_rx_dispatcher_t *rxd,
		   void *pcap_handle,
		   const uint8_t own_mac[6],
		   int64_t deadline_us);

int  radio_async_store(radio_rx_dispatcher_t *rxd,
		       const uint8_t *wire_frame, size_t frame_len,
		       uint16_t seq);

void radio_async_tick(radio_rx_dispatcher_t *rxd,
		      void *pcap_handle,
		      uint32_t ack_timeout_us,
		      uint32_t max_retry);

uint16_t radio_async_next_seq(radio_rx_dispatcher_t *rxd);
