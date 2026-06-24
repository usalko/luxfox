#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CASCADE_FRAME_VERSION 1
#define CASCADE_FRAME_HEADER_SIZE 6
#define CASCADE_FRAME_MAX_PAYLOAD 65000

typedef enum {
	CASCADE_CLASS_CONTROL = 0,
	CASCADE_CLASS_TELEMETRY = 1,
	CASCADE_CLASS_OSD_IMAGE = 2,
	CASCADE_CLASS_VIDEO = 3,
	CASCADE_CLASS_MANAGEMENT = 4,
} cascade_class_t;

typedef struct {
	uint8_t version;
	uint16_t src;
	uint16_t dst;
	uint8_t traffic_class;
	const uint8_t *payload;
	size_t payload_len;
} cascade_frame_view_t;

bool cascade_frame_pack(const cascade_frame_view_t *in, uint8_t *out, size_t out_capacity, size_t *out_len);
bool cascade_frame_unpack(const uint8_t *in, size_t in_len, cascade_frame_view_t *out);
