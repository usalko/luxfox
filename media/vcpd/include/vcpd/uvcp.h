#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UVCP_PREFIX "UVCP/1 "
#define UVCP_PREFIX_LEN 7
#define UVCP_MAX_MSG 256
#define UVCP_MAX_FIELDS 8
#define UVCP_LEASE_DEFAULT_MS 6000
#define UVCP_PONG_INTERVAL_MS 2000

typedef enum {
	UVCP_VERB_UNKNOWN = 0,
	UVCP_VERB_READY,
	UVCP_VERB_START,
	UVCP_VERB_STOP,
	UVCP_VERB_PING,
	UVCP_VERB_PONG,
} uvcp_verb_t;

typedef struct {
	char key[32];
	char value[64];
} uvcp_field_t;

typedef struct {
	uvcp_verb_t verb;
	uvcp_field_t fields[UVCP_MAX_FIELDS];
	int num_fields;
} uvcp_message_t;

typedef enum {
	UVCP_STATE_IDLE = 0,
	UVCP_STATE_STREAMING,
} uvcp_state_t;

typedef struct {
	uvcp_state_t state;
	uint64_t last_ready_ms;
	uint64_t last_pong_ms;
	uint64_t lease_ms;
} uvcp_session_t;

bool uvcp_is_control(const uint8_t *data, size_t len);
bool uvcp_parse(const uint8_t *data, size_t len, uvcp_message_t *out);
size_t uvcp_serialize(const uvcp_message_t *msg, uint8_t *out, size_t out_capacity);
size_t uvcp_build_pong(uint8_t *out, size_t out_capacity);

void uvcp_session_init(uvcp_session_t *sess, uint64_t lease_ms);
uvcp_state_t uvcp_session_handle(uvcp_session_t *sess, const uvcp_message_t *msg, uint64_t now_ms);
bool uvcp_session_is_active(const uvcp_session_t *sess, uint64_t now_ms);
bool uvcp_session_should_pong(const uvcp_session_t *sess, uint64_t now_ms);
