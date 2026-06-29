#include "radiod/rx_dispatcher.h"
#include "radiod/tx_scheduler.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "ulama/ulama_frame.h"

#if ULAMA_WITH_UNOW
#include <pcap/pcap.h>
#include <poll.h>
#include "unow_internal.h"
#include "radiod/sync.h"
#include "radiod/sync_frame.h"
#endif

void radio_rx_dispatcher_init(radio_rx_dispatcher_t *rxd,
			      radio_ipc_server_t *ipc)
{
	if (rxd == NULL)
		return;
	memset(rxd, 0, sizeof(*rxd));
	rxd->ipc = ipc;
}

void radio_rx_dispatcher_enable_relay(radio_rx_dispatcher_t *rxd,
				      uint8_t own_node_id,
				      void *sched,
				      radio_route_table_t *rt)
{
	if (rxd == NULL)
		return;
	rxd->relay_enabled = true;
	rxd->own_node_id = own_node_id;
	rxd->relay_sched = sched;
	rxd->route_table = rt;
}

void radio_rx_dispatcher_set_sync(radio_rx_dispatcher_t *rxd, void *sync_ctx)
{
	if (rxd != NULL)
		rxd->sync_ctx = sync_ctx;
}

#if ULAMA_WITH_UNOW

static int64_t now_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

#endif /* ULAMA_WITH_UNOW */

/* ================================================================
 * UNOW-level dedup (by DATA_SEQ sequence number)
 * ================================================================ */

#if ULAMA_WITH_UNOW
static bool dedup_check_and_add(radio_rx_dispatcher_t *rxd, uint16_t seq)
{
	for (uint16_t i = 0; i < rxd->dedup_count; i++) {
		uint16_t idx = (uint16_t)((rxd->dedup_head + RADIO_DEDUP_WINDOW
					   - 1U - i) % RADIO_DEDUP_WINDOW);
		if (rxd->dedup_ring[idx] == seq)
			return true;
	}
	rxd->dedup_ring[rxd->dedup_head] = seq;
	rxd->dedup_head = (uint16_t)((rxd->dedup_head + 1U) % RADIO_DEDUP_WINDOW);
	if (rxd->dedup_count < RADIO_DEDUP_WINDOW)
		rxd->dedup_count++;
	return false;
}

/* ================================================================
 * ULAMA-level dedup (by src_node + ulama_seq)
 *
 * Prevents broadcast storm in mesh: the same ULAMA frame arrives
 * via different relay paths with different UNOW sequences.
 * ================================================================ */

static bool ulama_dedup_check_and_add(radio_rx_dispatcher_t *rxd,
				      uint8_t src_node, uint16_t seq)
{
	for (uint16_t i = 0; i < rxd->ulama_dedup_count; i++) {
		uint16_t idx = (uint16_t)((rxd->ulama_dedup_head
					   + RADIO_ULAMA_DEDUP_WINDOW
					   - 1U - i) % RADIO_ULAMA_DEDUP_WINDOW);
		radio_ulama_dedup_key_t *k = &rxd->ulama_dedup_ring[idx];
		if (k->src_node == src_node && k->seq == seq)
			return true;
	}

	radio_ulama_dedup_key_t *slot = &rxd->ulama_dedup_ring[rxd->ulama_dedup_head];
	slot->src_node = src_node;
	slot->seq = seq;
	rxd->ulama_dedup_head = (uint16_t)((rxd->ulama_dedup_head + 1U)
					    % RADIO_ULAMA_DEDUP_WINDOW);
	if (rxd->ulama_dedup_count < RADIO_ULAMA_DEDUP_WINDOW)
		rxd->ulama_dedup_count++;
	return false;
}

/* ================================================================
 * ULAMA header quick-parse from packed wire bytes.
 * Avoids full unpack — just reads routing-relevant fields.
 * ================================================================ */

static bool ulama_header_peek(const uint8_t *data, size_t len,
			      uint8_t *src_node, uint8_t *dst_node,
			      uint8_t *flags, uint8_t *traffic_class,
			      uint16_t *seq, uint8_t *ttl)
{
	if (data == NULL || len < ULAMA_FRAME_HEADER_SIZE)
		return false;
	if (data[0] != ULAMA_FRAME_MAGIC)
		return false;

	*src_node = data[2];
	*dst_node = data[3];
	*flags = data[4];
	*traffic_class = data[5];
	*seq = (uint16_t)data[6] | ((uint16_t)data[7] << 8);
	*ttl = data[10];
	return true;
}

/* Map ULAMA traffic class → radiod priority */
static uint8_t class_to_prio(uint8_t traffic_class)
{
	switch (traffic_class) {
	case ULAMA_CLASS_CTRL:      return RADIO_PRIO_CTRL;
	case ULAMA_CLASS_TELEMETRY: return RADIO_PRIO_TELEM;
	case ULAMA_CLASS_VIDEO:     return RADIO_PRIO_VIDEO;
	default:                    return RADIO_PRIO_BULK;
	}
}

/* ================================================================
 * Relay: modify packed frame in-place, re-enqueue for TX
 * ================================================================ */

static void relay_frame(radio_rx_dispatcher_t *rxd,
			const uint8_t *ulama_data, size_t ulama_len,
			uint8_t dst_node, uint8_t traffic_class,
			uint8_t ttl)
{
	radio_tx_scheduler_t *sched = (radio_tx_scheduler_t *)rxd->relay_sched;
	uint8_t relay_buf[ULAMA_FRAME_HEADER_SIZE + ULAMA_FRAME_MAX_PAYLOAD];
	uint8_t next_hop[6];

	if (sched == NULL || ulama_len > sizeof(relay_buf))
		return;

	/* TTL exhausted — drop to prevent loops */
	if (ttl <= 1) {
		rxd->stats.relay_dropped_ttl++;
		return;
	}

	/* Copy and patch: decrement TTL, set MESH_RELAY flag */
	memcpy(relay_buf, ulama_data, ulama_len);
	relay_buf[4] |= ULAMA_FLAG_MESH_RELAY;
	relay_buf[10] = ttl - 1;

	/* Recalculate CRC over modified header (bytes 0..11) */
	uint16_t crc = ulama_crc16_ccitt(relay_buf, 12);
	relay_buf[12] = (uint8_t)(crc & 0xFF);
	relay_buf[13] = (uint8_t)(crc >> 8);

	/* Choose next-hop: route table or broadcast */
	bool has_route = false;
	if (rxd->route_table != NULL && dst_node != 0xFF)
		has_route = radio_route_lookup(rxd->route_table, dst_node, next_hop);

	uint8_t prio = class_to_prio(traffic_class);

	if (has_route)
		radio_tx_enqueue_relay(sched, prio, 0, relay_buf, ulama_len, next_hop);
	else
		radio_tx_enqueue(sched, prio, 0, relay_buf, ulama_len);

	rxd->stats.relay_forwarded++;
	if (prio < 4)
		rxd->stats.relay_by_prio[prio]++;
}

/* ---- ACK frame builder ---- */

static void send_ack_frame(pcap_t *pcap, const uint8_t own_mac[6],
			   const uint8_t dst_mac[6], uint16_t seq)
{
	uint8_t ack_pkt[sizeof(struct unow_radiotap_tx_header) +
			sizeof(struct unow_dot11_mgmt_header) +
			sizeof(struct unow_action_vendor_header) + 2U];
	uint8_t seq_bytes[2];
	size_t ack_len;

	seq_bytes[0] = (uint8_t)(seq >> 8);
	seq_bytes[1] = (uint8_t)(seq & 0xFFU);

	ack_len = unow_build_action_frame_ex(
		ack_pkt, sizeof(ack_pkt),
		own_mac, dst_mac, seq_bytes, 2U,
		UNOW_TX_RATE_1MBPS, UNOW_VENDOR_SUBTYPE_ACK);

	if (ack_len > 0U)
		pcap_inject(pcap, ack_pkt, ack_len);
}

/* ---- Async ACK handling ---- */

static void async_ack_received(radio_rx_dispatcher_t *rxd, uint16_t ack_seq)
{
	for (int i = 0; i < RADIO_ASYNC_SLOTS; i++) {
		radio_async_slot_t *slot = &rxd->async_slots[i];
		if (slot->active && slot->seq == ack_seq) {
			slot->active = false;
			rxd->stats.tx_ack_ok++;
			return;
		}
	}
}

/* ---- SYNC relay: prepare and inject relayed SYNC frame ---- */

static void radio_sync_relay_inject(radio_rx_dispatcher_t *rxd,
				    const sync_frame_t *rx_sf,
				    pcap_t *pcap,
				    const uint8_t own_mac[6])
{
	radio_sync_t *sync = (radio_sync_t *)rxd->sync_ctx;
	sync_frame_t relay_sf;
	int64_t tx_time = now_us();

	if (!radio_sync_prepare_relay(sync, rx_sf, &relay_sf, tx_time))
		return;

	uint8_t packed[SYNC_FRAME_MAX_SIZE];
	size_t packed_len;
	if (!sync_frame_pack(&relay_sf, packed, sizeof(packed), &packed_len))
		return;

	uint8_t wire[sizeof(struct unow_radiotap_tx_header) +
		     sizeof(struct unow_dot11_mgmt_header) +
		     sizeof(struct unow_action_vendor_header) +
		     SYNC_FRAME_MAX_SIZE];
	const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	size_t wire_len = unow_build_action_frame_ex(
		wire, sizeof(wire), own_mac, broadcast,
		packed, packed_len,
		UNOW_TX_RATE_1MBPS, UNOW_VENDOR_SUBTYPE_SYNC);
	if (wire_len > 0U) {
		pcap_inject(pcap, wire, wire_len);
		rxd->stats.sync_relayed++;
	}
}

/* ================================================================
 * RX slot: main receive loop with mesh routing
 * ================================================================ */

void radio_rx_slot(radio_rx_dispatcher_t *rxd,
		   void *pcap_handle,
		   const uint8_t own_mac[6],
		   int64_t deadline_us)
{
	pcap_t *pcap = (pcap_t *)pcap_handle;
	struct pcap_pkthdr *header;
	const uint8_t *packet;
	unow_diag_frame_t frame;
	int status;

	if (rxd == NULL || pcap == NULL)
		return;

	/* Reset per-cycle feedback counters */
	rxd->ctrl_for_us = 0;

	/* Use poll() on pcap fd instead of busy-spinning.
	 * pcap_next_ex in nonblock mode returns 0 immediately when no
	 * packet is available — without poll this burns 100% CPU. */
	int pcap_fd = pcap_get_selectable_fd(pcap);

	while (now_us() < deadline_us) {
		/* Sleep until packet arrives or deadline */
		if (pcap_fd >= 0) {
			int remaining_ms = (int)((deadline_us - now_us()) / 1000);
			if (remaining_ms <= 0)
				break;
			struct pollfd pfd = { .fd = pcap_fd, .events = POLLIN };
			poll(&pfd, 1, remaining_ms > 0 ? remaining_ms : 1);
		}

		status = pcap_next_ex(pcap, &header, &packet);

		if (status == 0)
			continue;
		if (status < 0) {
			rxd->stats.rx_pcap_error++;
			break;
		}

		rxd->stats.rx_total++;

		if (!unow_parse_action_frame(packet, header->caplen, &frame)) {
			rxd->stats.rx_parse_fail++;
			continue;
		}

		/* Drop self-sent frames */
		if (memcmp(frame.src_mac, own_mac, 6) == 0) {
			rxd->stats.rx_self_dropped++;
			continue;
		}

		/* SYNC frame → delegate to sync engine */
		if (frame.subtype == UNOW_VENDOR_SUBTYPE_SYNC) {
			rxd->stats.rx_sync++;
			if (rxd->sync_ctx != NULL) {
				sync_frame_t sf;
				if (sync_frame_unpack(frame.payload,
						      frame.len, &sf)) {
					int64_t rx_time = now_us();
					bool should_relay =
						radio_sync_on_sync_rx(
						    (radio_sync_t *)rxd->sync_ctx,
						    &sf, rx_time,
						    sf.sender_node_id);
					if (should_relay)
						radio_sync_relay_inject(
						    rxd, &sf, pcap, own_mac);
				}
			}
			continue;
		}

		/* DELAY_REQ → master records T4 */
		if (frame.subtype == UNOW_VENDOR_SUBTYPE_DELAY_REQ) {
			rxd->stats.rx_delay_req++;
			if (rxd->sync_ctx != NULL) {
				delay_req_frame_t dreq;
				if (delay_req_unpack(frame.payload,
						     frame.len, &dreq)) {
					radio_sync_on_delay_req_rx(
					    (radio_sync_t *)rxd->sync_ctx,
					    &dreq, now_us());
				}
			}
			continue;
		}

		/* ACK frame → clear pending async slot */
		if (frame.subtype == UNOW_VENDOR_SUBTYPE_ACK) {
			rxd->stats.rx_ack++;
			if (frame.len >= 2U) {
				uint16_t ack_seq = ((uint16_t)frame.payload[0] << 8) |
						   (uint16_t)frame.payload[1];
				async_ack_received(rxd, ack_seq);
			}
			continue;
		}

		/* DATA_SEQ → send ACK back, strip seq header, UNOW dedup */
		if (frame.subtype == UNOW_VENDOR_SUBTYPE_DATA_SEQ && frame.len >= 2U) {
			uint16_t seq = ((uint16_t)frame.payload[0] << 8) |
				       (uint16_t)frame.payload[1];

			send_ack_frame(pcap, own_mac, frame.src_mac, seq);
			rxd->stats.rx_ack_sent++;
			rxd->stats.rx_data_seq++;

			if (dedup_check_and_add(rxd, seq)) {
				rxd->stats.rx_dedup_dropped++;
				continue;
			}

			/* Strip 2-byte seq header */
			memmove(frame.payload, frame.payload + 2U, frame.len - 2U);
			frame.len -= 2U;
		} else {
			rxd->stats.rx_data++;
		}

		if (frame.len == 0U)
			continue;

		/* ---- Parse ULAMA header for routing decision ---- */

		uint8_t src_node, dst_node, flags, traffic_class, ttl;
		uint16_t ulama_seq;

		bool is_ulama = ulama_header_peek(
			frame.payload, frame.len,
			&src_node, &dst_node, &flags,
			&traffic_class, &ulama_seq, &ttl);

		/* ULAMA-level dedup (mesh anti-loop) */
		if (is_ulama) {
			if (ulama_dedup_check_and_add(rxd, src_node, ulama_seq)) {
				rxd->stats.rx_ulama_dedup_dropped++;
				continue;
			}
		}

		/* Route learning: src_node is reachable through src_mac */
		if (is_ulama && rxd->route_table != NULL) {
			bool relayed = (flags & ULAMA_FLAG_MESH_RELAY) != 0;
			radio_route_learn(rxd->route_table, src_node,
					  frame.src_mac, ttl, frame.rssi,
					  relayed, now_us());
		}

		/* ---- Routing decision ---- */

		bool deliver_local = true;
		bool do_relay = false;

		if (is_ulama && rxd->relay_enabled) {
			if (dst_node == rxd->own_node_id) {
				deliver_local = true;
				do_relay = false;
			} else if (dst_node == 0xFF) {
				/* Broadcast: deliver locally AND relay */
				deliver_local = true;
				do_relay = true;
			} else {
				/* Addressed to another node: relay only */
				deliver_local = false;
				do_relay = true;
			}

			/* Don't relay frames we originated */
			if (src_node == rxd->own_node_id)
				do_relay = false;
		}

		/* Per-cycle watchdog feedback: CTRL addressed to us */
		if (is_ulama && traffic_class == ULAMA_CLASS_CTRL &&
		    (dst_node == rxd->own_node_id || dst_node == 0xFF))
			rxd->ctrl_for_us++;

		/* Dispatch to local IPC clients */
		if (deliver_local && rxd->ipc != NULL) {
			radio_ipc_broadcast_rx(rxd->ipc, frame.rssi,
					       frame.src_mac,
					       frame.payload, frame.len);
			rxd->stats.rx_dispatched++;
		}

		/* Relay to next hop */
		if (do_relay && is_ulama) {
			relay_frame(rxd, frame.payload, frame.len,
				    dst_node, traffic_class, ttl);
		}
	}
}

/* ---- Async reliable TX ---- */

int radio_async_store(radio_rx_dispatcher_t *rxd,
		      const uint8_t *wire_frame, size_t frame_len,
		      uint16_t seq)
{
	if (rxd == NULL || wire_frame == NULL)
		return -1;
	if (frame_len > sizeof(rxd->async_slots[0].frame))
		return -1;

	for (int i = 0; i < RADIO_ASYNC_SLOTS; i++) {
		radio_async_slot_t *slot = &rxd->async_slots[i];
		if (!slot->active) {
			memcpy(slot->frame, wire_frame, frame_len);
			slot->frame_len = frame_len;
			slot->seq = seq;
			slot->sent_us = now_us();
			slot->attempts_left = 2;
			slot->active = true;
			return i;
		}
	}
	return -1;
}

void radio_async_tick(radio_rx_dispatcher_t *rxd,
		      void *pcap_handle,
		      uint32_t ack_timeout_us,
		      uint32_t max_retry)
{
	pcap_t *pcap = (pcap_t *)pcap_handle;
	int64_t ts = now_us();

	(void)max_retry;

	if (rxd == NULL || pcap == NULL)
		return;

	for (int i = 0; i < RADIO_ASYNC_SLOTS; i++) {
		radio_async_slot_t *slot = &rxd->async_slots[i];
		if (!slot->active)
			continue;

		int64_t elapsed = ts - slot->sent_us;
		if (elapsed < (int64_t)ack_timeout_us)
			continue;

		if (slot->attempts_left > 0) {
			int injected = pcap_inject(pcap, slot->frame, slot->frame_len);
			if (injected >= 0)
				rxd->stats.tx_retries++;
			slot->sent_us = ts;
			slot->attempts_left--;
		} else {
			slot->active = false;
			rxd->stats.tx_ack_timeout++;
		}
	}
}

uint16_t radio_async_next_seq(radio_rx_dispatcher_t *rxd)
{
	if (rxd == NULL)
		return 0;
	return ++rxd->async_tx_seq;
}

#endif /* ULAMA_WITH_UNOW */
