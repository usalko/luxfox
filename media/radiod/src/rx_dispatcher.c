#include "radiod/rx_dispatcher.h"

#include <pcap/pcap.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

/* We link against unow sources directly, same as vcpd/ulamad */
#include "unow_internal.h"

static int64_t now_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

void radio_rx_dispatcher_init(radio_rx_dispatcher_t *rxd,
			      radio_ipc_server_t *ipc)
{
	if (rxd == NULL)
		return;
	memset(rxd, 0, sizeof(*rxd));
	rxd->ipc = ipc;
}

/* ---- Dedup ring ---- */

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

/* ---- RX slot: main receive loop ---- */

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

	while (now_us() < deadline_us) {
		status = pcap_next_ex(pcap, &header, &packet);

		if (status == 0)
			continue; /* timeout, try again if within deadline */
		if (status < 0)
			break;    /* error or EOF */

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

		/* DATA_SEQ → send ACK back, strip seq header, check dedup */
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

			/* Strip 2-byte seq header before dispatching */
			memmove(frame.payload, frame.payload + 2U, frame.len - 2U);
			frame.len -= 2U;
		} else {
			rxd->stats.rx_data++;
		}

		/* Dispatch to all IPC clients */
		if (rxd->ipc != NULL && frame.len > 0U) {
			radio_ipc_broadcast_rx(rxd->ipc, frame.rssi,
					       frame.src_mac,
					       frame.payload, frame.len);
			rxd->stats.rx_dispatched++;
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
			slot->attempts_left = 2; /* default, overridden by async_tick */
			slot->active = true;
			return i;
		}
	}
	return -1; /* all slots busy */
}

void radio_async_tick(radio_rx_dispatcher_t *rxd,
		      void *pcap_handle,
		      uint32_t ack_timeout_us,
		      uint32_t max_retry)
{
	pcap_t *pcap = (pcap_t *)pcap_handle;
	int64_t ts = now_us();

	(void)max_retry; /* retry count is set at store time */

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
