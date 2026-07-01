#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CASCADE_FRAME_VERSION 1
#define CASCADE_FRAME_HEADER_SIZE 6
/*
 * Cascade frames travel over a UDP socket (cascade-in/cascade-out), so the
 * total datagram (header + payload) must fit the IPv4 UDP payload ceiling of
 * 65507 bytes (65535 - 20 byte IP header - 8 byte UDP header), or sendto()
 * fails. This is slightly below VIDEO_RING_SLOT_MAX (65536) — a maximal
 * ring-slot frame from vcpd is vanishingly rare in practice (typical encoded
 * H.265 frames at the configured bitrates are a few KB to a few tens of KB),
 * but deliver_video_frame()/send_cascade_frame() must still handle the
 * rejection cleanly rather than silently dropping a frame on the floor.
 */
#define CASCADE_FRAME_MAX_PAYLOAD (65507 - CASCADE_FRAME_HEADER_SIZE)

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
