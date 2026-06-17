#include "ulama_gw/gateway.h"

uint8_t gw_class_cascade_to_ulama(uint8_t cascade_class)
{
	switch (cascade_class) {
	case CASCADE_CLASS_CONTROL:    return ULAMA_CLASS_CTRL;
	case CASCADE_CLASS_TELEMETRY:  return ULAMA_CLASS_TELEMETRY;
	case CASCADE_CLASS_OSD_IMAGE:  return ULAMA_CLASS_BULK;
	case CASCADE_CLASS_VIDEO:      return ULAMA_CLASS_VIDEO;
	case CASCADE_CLASS_MANAGEMENT: return ULAMA_CLASS_CTRL;
	default:                       return ULAMA_CLASS_BULK;
	}
}

uint8_t gw_class_ulama_to_cascade(uint8_t ulama_class)
{
	switch (ulama_class) {
	case ULAMA_CLASS_CTRL:      return CASCADE_CLASS_CONTROL;
	case ULAMA_CLASS_TELEMETRY: return CASCADE_CLASS_TELEMETRY;
	case ULAMA_CLASS_BULK:      return CASCADE_CLASS_OSD_IMAGE;
	case ULAMA_CLASS_VIDEO:     return CASCADE_CLASS_VIDEO;
	default:                    return CASCADE_CLASS_TELEMETRY;
	}
}

uint8_t gw_addr_u16_to_u8(const gw_config_t *cfg, uint16_t addr)
{
	if (cfg && cfg->addr_map_set) {
		for (int i = 1; i <= GW_MAX_ADDR_MAP; i++) {
			if (cfg->addr_map[i] == (uint8_t)(addr & 0xFF))
				return (uint8_t)i;
		}
	}
	if (addr > 253)
		return 253;
	return (uint8_t)addr;
}

uint16_t gw_addr_u8_to_u16(const gw_config_t *cfg, uint8_t addr)
{
	if (cfg && cfg->addr_map_set && addr > 0 && addr <= GW_MAX_ADDR_MAP)
		return (uint16_t)cfg->addr_map[addr];
	return (uint16_t)addr;
}
