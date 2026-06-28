#pragma once

#include <stdbool.h>
#include <stdint.h>

/* -------------------------------------------------------------------
 * Route table — auto-learning topology from RX traffic.
 *
 * For each known dst_node stores the best next-hop MAC address.
 * Learned passively: every RX frame teaches us
 *   "src_node is reachable through src_mac".
 *
 * Used by relay engine to choose unicast vs broadcast for relay TX.
 * ------------------------------------------------------------------- */

#define RADIO_MAX_ROUTES       32
#define RADIO_ROUTE_EXPIRE_US  30000000  /* 30 seconds */

typedef struct {
	uint8_t  dst_node;
	uint8_t  next_hop_mac[6];
	uint8_t  hop_count;
	int8_t   rssi;
	int64_t  last_seen_us;
	bool     active;
} radio_route_entry_t;

typedef struct {
	radio_route_entry_t entries[RADIO_MAX_ROUTES];
	uint32_t learned;
	uint32_t expired;
} radio_route_table_t;

void radio_route_table_init(radio_route_table_t *rt);

/*
 * Learn a route from an incoming frame.
 * direct=true  → frame came directly from src_node (hop_count=1)
 * direct=false → frame was relayed (hop_count estimated from TTL)
 */
void radio_route_learn(radio_route_table_t *rt,
		       uint8_t src_node,
		       const uint8_t src_mac[6],
		       uint8_t ttl,
		       int8_t rssi,
		       bool relayed,
		       int64_t now_us);

/*
 * Look up next-hop MAC for a destination node.
 * Returns true if a route exists, fills out_mac.
 * Returns false if unknown — caller should broadcast.
 */
bool radio_route_lookup(const radio_route_table_t *rt,
			uint8_t dst_node,
			uint8_t out_mac[6]);

/* Remove expired routes. Returns number of routes expired. */
int  radio_route_expire(radio_route_table_t *rt, int64_t now_us);

/* Number of active routes. */
int  radio_route_count(const radio_route_table_t *rt);
