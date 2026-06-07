#include "unow/radio_unow.h"
#include "unow/unow_diag.h"

#include <ctype.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void print_usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [--iface mon0] [--node-id N] [--log-level LEVEL] [--send-hex HEX] [--send-text TEXT] [--dst aa:bb:cc:dd:ee:ff] [--listen N] [--no-dump]\n",
		prog);
}

static int hex_nibble(char ch)
{
	if (ch >= '0' && ch <= '9') {
		return ch - '0';
	}
	if (ch >= 'a' && ch <= 'f') {
		return ch - 'a' + 10;
	}
	if (ch >= 'A' && ch <= 'F') {
		return ch - 'A' + 10;
	}
	return -1;
}

static bool parse_hex_bytes(const char *text, uint8_t *out, size_t out_size, size_t *out_len)
{
	int high = -1;
	size_t len = 0;

	if (text == NULL || out == NULL || out_len == NULL) {
		return false;
	}
	for (; *text != '\0'; ++text) {
		int nibble;

		if (*text == ':' || *text == '-' || *text == ' ' || *text == '\t') {
			continue;
		}
		nibble = hex_nibble(*text);
		if (nibble < 0) {
			return false;
		}
		if (high < 0) {
			high = nibble;
			continue;
		}
		if (len >= out_size) {
			return false;
		}
		out[len++] = (uint8_t)((high << 4) | nibble);
		high = -1;
	}
	if (high >= 0) {
		return false;
	}
	*out_len = len;
	return true;
}

static bool parse_log_level(const char *text, unow_log_level_t *level)
{
	if (text == NULL || level == NULL) {
		return false;
	}
	if (strcasecmp(text, "error") == 0) {
		*level = UNOW_LOG_ERROR;
		return true;
	}
	if (strcasecmp(text, "warn") == 0 || strcasecmp(text, "warning") == 0) {
		*level = UNOW_LOG_WARN;
		return true;
	}
	if (strcasecmp(text, "info") == 0) {
		*level = UNOW_LOG_INFO;
		return true;
	}
	if (strcasecmp(text, "debug") == 0) {
		*level = UNOW_LOG_DEBUG;
		return true;
	}
	if (strcasecmp(text, "trace") == 0) {
		*level = UNOW_LOG_TRACE;
		return true;
	}
	return false;
}

int main(int argc, char **argv)
{
	char iface[IFNAMSIZ] = {0};
	uint8_t dst_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	uint8_t payload[ULAMA_ESPNOW_MAX_PAYLOAD];
	size_t payload_len = 0;
	unsigned int node_id = 0;
	int listen_count = 0;
	bool do_dump = true;
	bool want_send = false;
	int i;

	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--iface") == 0 && i + 1 < argc) {
			snprintf(iface, sizeof(iface), "%s", argv[++i]);
			continue;
		}
		if (strcmp(argv[i], "--node-id") == 0 && i + 1 < argc) {
			node_id = (unsigned int)strtoul(argv[++i], NULL, 0);
			continue;
		}
		if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
			unow_log_level_t level;

			if (!parse_log_level(argv[++i], &level)) {
				fprintf(stderr, "invalid log level\n");
				return 2;
			}
			unow_diag_set_level(level);
			continue;
		}
		if (strcmp(argv[i], "--send-hex") == 0 && i + 1 < argc) {
			if (!parse_hex_bytes(argv[++i], payload, sizeof(payload), &payload_len)) {
				fprintf(stderr, "invalid hex payload\n");
				return 2;
			}
			want_send = true;
			continue;
		}
		if (strcmp(argv[i], "--send-text") == 0 && i + 1 < argc) {
			const char *text = argv[++i];

			payload_len = strlen(text);
			if (payload_len > sizeof(payload)) {
				fprintf(stderr, "text payload too long\n");
				return 2;
			}
			memcpy(payload, text, payload_len);
			want_send = true;
			continue;
		}
		if (strcmp(argv[i], "--dst") == 0 && i + 1 < argc) {
			if (!unow_diag_parse_mac(argv[++i], dst_mac)) {
				fprintf(stderr, "invalid destination mac\n");
				return 2;
			}
			continue;
		}
		if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
			listen_count = atoi(argv[++i]);
			continue;
		}
		if (strcmp(argv[i], "--no-dump") == 0) {
			do_dump = false;
			continue;
		}
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			print_usage(argv[0]);
			return 0;
		}
		fprintf(stderr, "unknown argument: %s\n", argv[i]);
		print_usage(argv[0]);
		return 2;
	}
	if (iface[0] != '\0') {
		esp_err_t err = unow_configure_iface(iface);

		if (err != ESP_OK) {
			fprintf(stderr, "unow_configure_iface failed: %s\n", esp_err_to_name(err));
			return 1;
		}
	}
	if (unow_init_iface((uint8_t)node_id, iface[0] != '\0' ? iface : NULL) != ESP_OK) {
		fprintf(stderr, "unow_init_iface failed\n");
		return 1;
	}
	if (do_dump) {
		unow_dump_state(stdout);
	}
	if (want_send) {
		esp_err_t err = radio_espnow_send(dst_mac, payload, payload_len);

		if (err != ESP_OK) {
			fprintf(stderr, "radio_espnow_send failed: %s\n", esp_err_to_name(err));
			return 1;
		}
		fprintf(stdout, "sent %zu bytes\n", payload_len);
	}
	for (i = 0; i < listen_count; ++i) {
		unow_diag_frame_t frame;
		esp_err_t err = unow_diag_recv(&frame, 1000);

		if (err == ESP_OK) {
			unow_diag_dump_frame(stdout, &frame);
			continue;
		}
		if (err == ESP_ERR_NOT_FOUND) {
			fprintf(stdout, "listen timeout\n");
			continue;
		}
		fprintf(stderr, "unow_diag_recv failed: %s\n", esp_err_to_name(err));
		return 1;
	}
	return 0;
}