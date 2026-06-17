#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vcpd/uvcp.h"

static int failures = 0;

static void expect_true(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		failures++;
	}
}

static void test_is_control(void)
{
	const char *msg = "UVCP/1 READY stream=0\n";
	expect_true(uvcp_is_control((const uint8_t *)msg, strlen(msg)), "should detect UVCP");
	expect_true(!uvcp_is_control((const uint8_t *)"hello", 5), "should reject non-UVCP");
}

static void test_parse_ready(void)
{
	const char *msg = "UVCP/1 READY stream=0 codec=h264\n";
	uvcp_message_t parsed;
	expect_true(uvcp_parse((const uint8_t *)msg, strlen(msg), &parsed), "parse READY");
	expect_true(parsed.verb == UVCP_VERB_READY, "verb should be READY");
	expect_true(parsed.num_fields == 2, "should have 2 fields");

	bool found_stream = false, found_codec = false;
	for (int i = 0; i < parsed.num_fields; i++) {
		if (strcmp(parsed.fields[i].key, "stream") == 0 &&
		    strcmp(parsed.fields[i].value, "0") == 0)
			found_stream = true;
		if (strcmp(parsed.fields[i].key, "codec") == 0 &&
		    strcmp(parsed.fields[i].value, "h264") == 0)
			found_codec = true;
	}
	expect_true(found_stream, "should have stream=0");
	expect_true(found_codec, "should have codec=h264");
}

static void test_parse_simple_verbs(void)
{
	const char *stop = "UVCP/1 STOP\n";
	uvcp_message_t parsed;
	expect_true(uvcp_parse((const uint8_t *)stop, strlen(stop), &parsed), "parse STOP");
	expect_true(parsed.verb == UVCP_VERB_STOP, "verb should be STOP");
	expect_true(parsed.num_fields == 0, "STOP should have no fields");

	const char *ping = "UVCP/1 PING\n";
	expect_true(uvcp_parse((const uint8_t *)ping, strlen(ping), &parsed), "parse PING");
	expect_true(parsed.verb == UVCP_VERB_PING, "verb should be PING");
}

static void test_serialize_roundtrip(void)
{
	uvcp_message_t msg = {.verb = UVCP_VERB_PONG, .num_fields = 0};
	uint8_t buf[128];
	size_t n = uvcp_serialize(&msg, buf, sizeof(buf));
	expect_true(n > 0, "serialize should produce output");
	expect_true(uvcp_is_control(buf, n), "serialized should be valid UVCP");

	uvcp_message_t parsed;
	expect_true(uvcp_parse(buf, n, &parsed), "parse serialized PONG");
	expect_true(parsed.verb == UVCP_VERB_PONG, "roundtrip verb should be PONG");
}

static void test_session_lifecycle(void)
{
	uvcp_session_t sess;
	uvcp_session_init(&sess, 6000);

	expect_true(!uvcp_session_is_active(&sess, 1000), "idle session should not be active");

	uvcp_message_t ready = {.verb = UVCP_VERB_READY, .num_fields = 0};
	uvcp_session_handle(&sess, &ready, 1000);
	expect_true(uvcp_session_is_active(&sess, 1000), "session should be active after READY");
	expect_true(uvcp_session_is_active(&sess, 6999), "session should be active within lease");
	expect_true(!uvcp_session_is_active(&sess, 7001), "session should expire after lease");

	uvcp_message_t stop = {.verb = UVCP_VERB_STOP, .num_fields = 0};
	uvcp_session_handle(&sess, &stop, 2000);
	expect_true(!uvcp_session_is_active(&sess, 2000), "session should be idle after STOP");
}

int main(void)
{
	test_is_control();
	test_parse_ready();
	test_parse_simple_verbs();
	test_serialize_roundtrip();
	test_session_lifecycle();

	if (failures > 0) {
		fprintf(stderr, "test_uvcp: %d failures\n", failures);
		return 1;
	}
	printf("test_uvcp: ok\n");
	return 0;
}
