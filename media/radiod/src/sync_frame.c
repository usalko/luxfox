#include "radiod/sync_frame.h"
#include <string.h>

static void write_le16(uint8_t *p, uint16_t v)
{
	memcpy(p, &v, 2);
}

static void write_le32(uint8_t *p, uint32_t v)
{
	memcpy(p, &v, 4);
}

static void write_le64(uint8_t *p, int64_t v)
{
	memcpy(p, &v, 8);
}

static uint16_t read_le16(const uint8_t *p)
{
	uint16_t v;
	memcpy(&v, p, 2);
	return v;
}

static uint32_t read_le32(const uint8_t *p)
{
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

static int64_t read_le64(const uint8_t *p)
{
	int64_t v;
	memcpy(&v, p, 8);
	return v;
}

/*
 * SYNC Frame layout:
 *  [0]     magic       0xBE
 *  [1]     version     0x01
 *  [2]     master_node_id
 *  [3]     sender_node_id
 *  [4..7]  superframe_seq   (LE32)
 *  [8..15] origin_time_us   (LE64)
 *  [16..17] dl_duration_us  (LE16)
 *  [18..19] ul_slot_us      (LE16)
 *  [20..21] guard_us        (LE16)
 *  [22]    num_slots
 *  [23]    relay_hops
 *  [24..27] slot_map[4]
 *  [28]    num_delay_resp
 *  [29..]  delay_resp[]: node_id(1) + t4_us(8) each
 */

bool sync_frame_pack(const sync_frame_t *in,
                     uint8_t *out, size_t capacity, size_t *out_len)
{
	if (in == NULL || out == NULL || out_len == NULL)
		return false;

	if (in->num_slots > SYNC_MAX_SLOTS)
		return false;
	if (in->num_delay_resp > SYNC_MAX_DELAY_RESP)
		return false;

	size_t needed = SYNC_FRAME_MIN_SIZE + (size_t)in->num_delay_resp * 9;
	if (capacity < needed)
		return false;

	out[0] = SYNC_FRAME_MAGIC;
	out[1] = SYNC_FRAME_VERSION;
	out[2] = in->master_node_id;
	out[3] = in->sender_node_id;
	write_le32(&out[4], in->superframe_seq);
	write_le64(&out[8], in->origin_time_us);
	write_le16(&out[16], in->dl_duration_us);
	write_le16(&out[18], in->ul_slot_us);
	write_le16(&out[20], in->guard_us);
	out[22] = in->num_slots;
	out[23] = in->relay_hops;
	out[24] = in->slot_map[0];
	out[25] = in->slot_map[1];
	out[26] = in->slot_map[2];
	out[27] = in->slot_map[3];
	out[28] = in->num_delay_resp;

	for (uint8_t i = 0; i < in->num_delay_resp; i++) {
		size_t off = 29 + (size_t)i * 9;
		out[off] = in->delay_resp[i].node_id;
		write_le64(&out[off + 1], in->delay_resp[i].t4_us);
	}

	*out_len = needed;
	return true;
}

bool sync_frame_unpack(const uint8_t *in, size_t in_len,
                       sync_frame_t *out)
{
	if (in == NULL || out == NULL)
		return false;
	if (in_len < SYNC_FRAME_MIN_SIZE)
		return false;
	if (in[0] != SYNC_FRAME_MAGIC)
		return false;
	if (in[1] != SYNC_FRAME_VERSION)
		return false;

	out->master_node_id = in[2];
	out->sender_node_id = in[3];
	out->superframe_seq = read_le32(&in[4]);
	out->origin_time_us = read_le64(&in[8]);
	out->dl_duration_us = read_le16(&in[16]);
	out->ul_slot_us     = read_le16(&in[18]);
	out->guard_us       = read_le16(&in[20]);
	out->num_slots      = in[22];
	out->relay_hops     = in[23];
	out->slot_map[0]    = in[24];
	out->slot_map[1]    = in[25];
	out->slot_map[2]    = in[26];
	out->slot_map[3]    = in[27];
	out->num_delay_resp = in[28];

	if (out->num_slots > SYNC_MAX_SLOTS)
		return false;
	if (out->num_delay_resp > SYNC_MAX_DELAY_RESP)
		return false;
	if (in_len < SYNC_FRAME_MIN_SIZE + (size_t)out->num_delay_resp * 9)
		return false;

	for (uint8_t i = 0; i < out->num_delay_resp; i++) {
		size_t off = 29 + (size_t)i * 9;
		out->delay_resp[i].node_id = in[off];
		out->delay_resp[i].t4_us   = read_le64(&in[off + 1]);
	}

	return true;
}

/*
 * DELAY_REQ layout:
 *  [0]     magic       0xBD
 *  [1]     version     0x01
 *  [2]     requester_node_id
 *  [3]     target_node_id
 *  [4..11] t3_us       (LE64)
 *  [12..15] superframe_seq (LE32)
 */

bool delay_req_pack(const delay_req_frame_t *in,
                    uint8_t *out, size_t capacity, size_t *out_len)
{
	if (in == NULL || out == NULL || out_len == NULL)
		return false;
	if (capacity < DELAY_REQ_FRAME_SIZE)
		return false;

	out[0] = DELAY_REQ_MAGIC;
	out[1] = DELAY_REQ_VERSION;
	out[2] = in->requester_node_id;
	out[3] = in->target_node_id;
	write_le64(&out[4], in->t3_us);
	write_le32(&out[12], in->superframe_seq);

	*out_len = DELAY_REQ_FRAME_SIZE;
	return true;
}

bool delay_req_unpack(const uint8_t *in, size_t in_len,
                      delay_req_frame_t *out)
{
	if (in == NULL || out == NULL)
		return false;
	if (in_len < DELAY_REQ_FRAME_SIZE)
		return false;
	if (in[0] != DELAY_REQ_MAGIC)
		return false;
	if (in[1] != DELAY_REQ_VERSION)
		return false;

	out->requester_node_id = in[2];
	out->target_node_id    = in[3];
	out->t3_us             = read_le64(&in[4]);
	out->superframe_seq    = read_le32(&in[12]);

	return true;
}
