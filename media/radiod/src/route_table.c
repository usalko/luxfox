#include "radiod/route_table.h"

#include <string.h>

void radio_route_table_init(radio_route_table_t *rt)
{
	if (rt == NULL)
		return;
	memset(rt, 0, sizeof(*rt));
}

void radio_route_learn(radio_route_table_t *rt,
		       uint8_t src_node,
		       const uint8_t src_mac[6],
		       uint8_t ttl,
		       int8_t rssi,
		       bool relayed,
		       int64_t now_us)
{
	radio_route_entry_t *best = NULL;
	radio_route_entry_t *empty = NULL;
	uint8_t hop_count;

	if (rt == NULL || src_mac == NULL || src_node == 0)
		return;

	/* Estimate hop count from remaining TTL.
	 * Direct frame: TTL=8 (default) → hops=1
	 * Relayed once: TTL=7 → hops=2 */
	hop_count = relayed ? (uint8_t)(9U - (ttl > 8U ? 8U : ttl)) : 1U;
	if (hop_count == 0)
		hop_count = 1;

	for (int i = 0; i < RADIO_MAX_ROUTES; i++) {
		radio_route_entry_t *e = &rt->entries[i];
		if (e->active && e->dst_node == src_node) {
			best = e;
			break;
		}
		if (!e->active && empty == NULL)
			empty = e;
	}

	if (best != NULL) {
		/* Prefer shorter path or fresher route */
		if (hop_count <= best->hop_count || !relayed) {
			memcpy(best->next_hop_mac, src_mac, 6);
			best->hop_count = hop_count;
		}
		best->rssi = rssi;
		best->last_seen_us = now_us;
		return;
	}

	/* New node — use empty slot or evict oldest */
	if (empty == NULL) {
		int64_t oldest_ts = now_us;
		for (int i = 0; i < RADIO_MAX_ROUTES; i++) {
			if (rt->entries[i].last_seen_us < oldest_ts) {
				oldest_ts = rt->entries[i].last_seen_us;
				empty = &rt->entries[i];
			}
		}
	}

	if (empty != NULL) {
		empty->dst_node = src_node;
		memcpy(empty->next_hop_mac, src_mac, 6);
		empty->hop_count = hop_count;
		empty->rssi = rssi;
		empty->last_seen_us = now_us;
		empty->active = true;
		rt->learned++;
	}
}

bool radio_route_lookup(const radio_route_table_t *rt,
			uint8_t dst_node,
			uint8_t out_mac[6])
{
	if (rt == NULL || out_mac == NULL)
		return false;

	for (int i = 0; i < RADIO_MAX_ROUTES; i++) {
		const radio_route_entry_t *e = &rt->entries[i];
		if (e->active && e->dst_node == dst_node) {
			memcpy(out_mac, e->next_hop_mac, 6);
			return true;
		}
	}
	return false;
}

int radio_route_expire(radio_route_table_t *rt, int64_t now_us)
{
	int count = 0;

	if (rt == NULL)
		return 0;

	for (int i = 0; i < RADIO_MAX_ROUTES; i++) {
		radio_route_entry_t *e = &rt->entries[i];
		if (!e->active)
			continue;
		if (now_us - e->last_seen_us > RADIO_ROUTE_EXPIRE_US) {
			e->active = false;
			rt->expired++;
			count++;
		}
	}
	return count;
}

int radio_route_count(const radio_route_table_t *rt)
{
	int count = 0;

	if (rt == NULL)
		return 0;

	for (int i = 0; i < RADIO_MAX_ROUTES; i++) {
		if (rt->entries[i].active)
			count++;
	}
	return count;
}
