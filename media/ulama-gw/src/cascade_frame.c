#include "ulama_gw/cascade_frame.h"

#include <string.h>

bool cascade_frame_pack(const cascade_frame_view_t *in, uint8_t *out, size_t out_capacity, size_t *out_len)
{
	if (!in || !out || !out_len)
		return false;
	if (in->payload_len > CASCADE_FRAME_MAX_PAYLOAD)
		return false;

	size_t total = CASCADE_FRAME_HEADER_SIZE + in->payload_len;
	if (total > out_capacity)
		return false;

	uint8_t version = in->version;
	if (version == 0)
		version = CASCADE_FRAME_VERSION;

	out[0] = version;
	out[1] = (uint8_t)(in->src >> 8);
	out[2] = (uint8_t)(in->src & 0xFF);
	out[3] = (uint8_t)(in->dst >> 8);
	out[4] = (uint8_t)(in->dst & 0xFF);
	out[5] = in->traffic_class;

	if (in->payload_len > 0 && in->payload)
		memcpy(out + CASCADE_FRAME_HEADER_SIZE, in->payload, in->payload_len);

	*out_len = total;
	return true;
}

bool cascade_frame_unpack(const uint8_t *in, size_t in_len, cascade_frame_view_t *out)
{
	if (!in || !out)
		return false;
	if (in_len < CASCADE_FRAME_HEADER_SIZE)
		return false;

	size_t payload_len = in_len - CASCADE_FRAME_HEADER_SIZE;
	if (payload_len > CASCADE_FRAME_MAX_PAYLOAD)
		return false;

	out->version = in[0];
	out->src = ((uint16_t)in[1] << 8) | in[2];
	out->dst = ((uint16_t)in[3] << 8) | in[4];
	out->traffic_class = in[5];
	out->payload = (payload_len > 0) ? (in + CASCADE_FRAME_HEADER_SIZE) : NULL;
	out->payload_len = payload_len;

	return true;
}
