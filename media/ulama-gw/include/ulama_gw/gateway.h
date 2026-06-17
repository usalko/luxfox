#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ulama_gw/cascade_frame.h"
#include "ulama/ulama_frame.h"
#include "ulama/transport.h"

#define GW_MAX_ADDR_MAP 253

typedef struct {
	char cascade_in[64];
	char cascade_out[64];
	char transport_str[64];
	char iface[32];
	uint8_t node_id;
	uint8_t addr_map[GW_MAX_ADDR_MAP + 1];
	bool addr_map_set;
} gw_config_t;

uint8_t gw_class_cascade_to_ulama(uint8_t cascade_class);
uint8_t gw_class_ulama_to_cascade(uint8_t ulama_class);
uint8_t gw_addr_u16_to_u8(const gw_config_t *cfg, uint16_t addr);
uint16_t gw_addr_u8_to_u16(const gw_config_t *cfg, uint8_t addr);
