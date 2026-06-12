#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ULAMA_FRAME_MAGIC 0xA5
#define ULAMA_FRAME_VERSION 0x02
#define ULAMA_FRAME_HEADER_SIZE 14
#define ULAMA_FRAME_MAX_PAYLOAD 220
#define ULAMA_FRAME_DEFAULT_TTL 8
#define ULAMA_FRAME_ONE_HOP_TTL 1

#define ULAMA_FLAG_ACK_REQ 0x01
#define ULAMA_FLAG_IS_ACK 0x02
#define ULAMA_FLAG_FRAGMENT 0x04
#define ULAMA_FLAG_LAST_FRAGMENT 0x08
#define ULAMA_FLAG_MESH_RELAY 0x10
#define ULAMA_FLAG_ROUTE_DISC 0x20
#define ULAMA_FLAG_DEGRADED 0x40
#define ULAMA_FLAG_DUP_ALLOWED 0x80

typedef enum {
	ULAMA_CLASS_CTRL = 0,
	ULAMA_CLASS_TELEMETRY = 1,
	ULAMA_CLASS_BULK = 2,
	ULAMA_CLASS_VIDEO = 3,
} ulama_class_t;

typedef enum {
	ULAMA_FRAME_STATUS_OK = 0,
	ULAMA_FRAME_STATUS_INVALID_ARGS,
	ULAMA_FRAME_STATUS_SHORT_FRAME,
	ULAMA_FRAME_STATUS_BAD_MAGIC,
	ULAMA_FRAME_STATUS_VERSION_MISMATCH,
	ULAMA_FRAME_STATUS_BAD_CRC,
} ulama_frame_status_t;

typedef struct {
	uint8_t src_node;
	uint8_t dst_node;
	uint8_t flags;
	uint8_t traffic_class;
	uint16_t seq;
	uint8_t frag_idx;
	uint8_t frag_total;
	uint8_t ttl;
	const uint8_t *payload;
	size_t payload_len;
} ulama_frame_view_t;

uint16_t ulama_crc16_ccitt(const uint8_t *data, size_t len);
bool ulama_frame_pack(const ulama_frame_view_t *in, uint8_t *out, size_t out_capacity, size_t *out_len);
ulama_frame_status_t ulama_frame_unpack_ex(const uint8_t *in, size_t in_len, ulama_frame_view_t *out);
bool ulama_frame_unpack(const uint8_t *in, size_t in_len, ulama_frame_view_t *out);