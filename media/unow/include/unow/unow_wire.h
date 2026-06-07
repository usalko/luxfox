#pragma once

#include <stdint.h>

#define UNOW_DEFAULT_IFACE "mon0"
#define UNOW_DEFAULT_CHANNEL 6

#define UNOW_VENDOR_CATEGORY 127U
#define UNOW_VENDOR_SUBTYPE_DATA 0x01U

#define UNOW_MAC_ADDR_LEN 6U
#define UNOW_OUI_LEN 3U

#define UNOW_TX_RATE_1MBPS 2U

#define UNOW_RADIOTAP_PRESENT_RATE (1U << 2)
#define UNOW_RADIOTAP_PRESENT_DBM_SIGNAL (1U << 5)
#define UNOW_RADIOTAP_PRESENT_TX_FLAGS (1U << 15)
#define UNOW_RADIOTAP_PRESENT_EXT (1U << 31)

#define UNOW_RADIOTAP_TX_FLAGS_NOACK 0x0008U

#define UNOW_DOT11_FC_ACTION 0x00D0U

#define UNOW_OUI_BYTES {0xFA, 0xCE, 0x01}
#define UNOW_BSSID_BYTES {0x55, 0x4E, 0x4F, 0x57, 0x00, 0x01}

static const uint8_t k_unow_oui[UNOW_OUI_LEN] = UNOW_OUI_BYTES;
static const uint8_t k_unow_bssid[UNOW_MAC_ADDR_LEN] = UNOW_BSSID_BYTES;

struct unow_radiotap_tx_header {
	uint8_t version;
	uint8_t pad;
	uint16_t len;
	uint32_t present;
	uint8_t rate;
	uint8_t rate_pad;
	uint16_t tx_flags;
} __attribute__((packed));

struct unow_dot11_mgmt_header {
	uint16_t frame_control;
	uint16_t duration;
	uint8_t addr1[UNOW_MAC_ADDR_LEN];
	uint8_t addr2[UNOW_MAC_ADDR_LEN];
	uint8_t addr3[UNOW_MAC_ADDR_LEN];
	uint16_t seq_ctrl;
} __attribute__((packed));

struct unow_action_vendor_header {
	uint8_t category;
	uint8_t oui[UNOW_OUI_LEN];
	uint8_t subtype;
} __attribute__((packed));