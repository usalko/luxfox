#include "vcpd/uvcp.h"

#include <stdio.h>
#include <string.h>

bool uvcp_is_control(const uint8_t *data, size_t len)
{
	return len >= UVCP_PREFIX_LEN && memcmp(data, UVCP_PREFIX, UVCP_PREFIX_LEN) == 0;
}

static uvcp_verb_t parse_verb(const char *str, size_t len)
{
	if (len == 5 && memcmp(str, "READY", 5) == 0) return UVCP_VERB_READY;
	if (len == 5 && memcmp(str, "START", 5) == 0) return UVCP_VERB_START;
	if (len == 4 && memcmp(str, "STOP", 4) == 0) return UVCP_VERB_STOP;
	if (len == 4 && memcmp(str, "PING", 4) == 0) return UVCP_VERB_PING;
	if (len == 4 && memcmp(str, "PONG", 4) == 0) return UVCP_VERB_PONG;
	return UVCP_VERB_UNKNOWN;
}

bool uvcp_parse(const uint8_t *data, size_t len, uvcp_message_t *out)
{
	if (!uvcp_is_control(data, len) || !out)
		return false;

	memset(out, 0, sizeof(*out));

	char buf[UVCP_MAX_MSG];
	size_t copy_len = (len < sizeof(buf) - 1) ? len : (sizeof(buf) - 1);
	memcpy(buf, data, copy_len);
	buf[copy_len] = '\0';

	size_t end = copy_len;
	while (end > 0 && (buf[end - 1] == '\n' || buf[end - 1] == '\r'))
		buf[--end] = '\0';

	const char *cursor = buf + UVCP_PREFIX_LEN;
	const char *space = strchr(cursor, ' ');
	size_t verb_len = space ? (size_t)(space - cursor) : strlen(cursor);
	out->verb = parse_verb(cursor, verb_len);
	if (out->verb == UVCP_VERB_UNKNOWN)
		return false;

	if (!space)
		return true;

	cursor = space + 1;
	while (*cursor && out->num_fields < UVCP_MAX_FIELDS) {
		while (*cursor == ' ') cursor++;
		if (!*cursor) break;

		const char *eq = strchr(cursor, '=');
		const char *next_space = strchr(cursor, ' ');
		if (!eq || (next_space && eq > next_space)) {
			cursor = next_space ? next_space + 1 : cursor + strlen(cursor);
			continue;
		}

		size_t key_len = (size_t)(eq - cursor);
		if (key_len >= sizeof(out->fields[0].key))
			key_len = sizeof(out->fields[0].key) - 1;

		const char *val_start = eq + 1;
		const char *val_end = next_space ? next_space : cursor + strlen(cursor);
		size_t val_len = (size_t)(val_end - val_start);
		if (val_len >= sizeof(out->fields[0].value))
			val_len = sizeof(out->fields[0].value) - 1;

		uvcp_field_t *f = &out->fields[out->num_fields];
		memcpy(f->key, cursor, key_len);
		f->key[key_len] = '\0';
		memcpy(f->value, val_start, val_len);
		f->value[val_len] = '\0';
		out->num_fields++;

		cursor = val_end;
	}

	return true;
}

size_t uvcp_serialize(const uvcp_message_t *msg, uint8_t *out, size_t out_capacity)
{
	if (!msg || !out)
		return 0;

	const char *verb_str;
	switch (msg->verb) {
	case UVCP_VERB_READY: verb_str = "READY"; break;
	case UVCP_VERB_START: verb_str = "START"; break;
	case UVCP_VERB_STOP:  verb_str = "STOP"; break;
	case UVCP_VERB_PING:  verb_str = "PING"; break;
	case UVCP_VERB_PONG:  verb_str = "PONG"; break;
	default: return 0;
	}

	int written = snprintf((char *)out, out_capacity, "%s%s", UVCP_PREFIX, verb_str);
	if (written < 0 || (size_t)written >= out_capacity)
		return 0;

	size_t pos = (size_t)written;
	for (int i = 0; i < msg->num_fields; i++) {
		int n = snprintf((char *)out + pos, out_capacity - pos, " %s=%s",
				 msg->fields[i].key, msg->fields[i].value);
		if (n < 0 || pos + (size_t)n >= out_capacity)
			break;
		pos += (size_t)n;
	}

	if (pos + 1 < out_capacity) {
		out[pos++] = '\n';
		out[pos] = '\0';
	}

	return pos;
}

size_t uvcp_build_pong(uint8_t *out, size_t out_capacity)
{
	uvcp_message_t msg = {.verb = UVCP_VERB_PONG, .num_fields = 0};
	return uvcp_serialize(&msg, out, out_capacity);
}

void uvcp_session_init(uvcp_session_t *sess, uint64_t lease_ms)
{
	memset(sess, 0, sizeof(*sess));
	sess->state = UVCP_STATE_IDLE;
	sess->lease_ms = (lease_ms > 0) ? lease_ms : UVCP_LEASE_DEFAULT_MS;
}

uvcp_state_t uvcp_session_handle(uvcp_session_t *sess, const uvcp_message_t *msg, uint64_t now_ms)
{
	switch (msg->verb) {
	case UVCP_VERB_READY:
		sess->last_ready_ms = now_ms;
		sess->state = UVCP_STATE_STREAMING;
		break;
	case UVCP_VERB_STOP:
		sess->state = UVCP_STATE_IDLE;
		break;
	case UVCP_VERB_PING:
		sess->last_ready_ms = now_ms;
		break;
	default:
		break;
	}
	return sess->state;
}

bool uvcp_session_is_active(const uvcp_session_t *sess, uint64_t now_ms)
{
	if (sess->state != UVCP_STATE_STREAMING)
		return false;
	if (sess->last_ready_ms == 0)
		return false;
	return (now_ms - sess->last_ready_ms) < sess->lease_ms;
}

bool uvcp_session_should_pong(const uvcp_session_t *sess, uint64_t now_ms)
{
	if (sess->state != UVCP_STATE_STREAMING)
		return false;
	return (now_ms - sess->last_pong_ms) >= UVCP_PONG_INTERVAL_MS;
}
