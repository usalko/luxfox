#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ulama/crsf.h"
#include "ulama/serial_uart.h"
#include "ulama/transport.h"
#include "ulama/ulama_frame.h"

static volatile sig_atomic_t g_stop;

typedef enum {
	OUTPUT_MODE_UART = 0,
	OUTPUT_MODE_FILE,
	OUTPUT_MODE_STDOUT,
} output_mode_t;

typedef struct {
	ulama_transport_kind_t transport_kind;
	const char *config_path;
	const char *iface;
	const char *listen_addr;
	uint8_t node_id;
	const char *uart_path;
	uint32_t uart_baud;
	const char *output_path;
	const char *ready_path;
	output_mode_t output_mode;
	unsigned int frame_limit;
	bool verbose;
} app_config_t;

static void init_defaults(app_config_t *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->transport_kind = ULAMA_TRANSPORT_KIND_UNOW;
	cfg->config_path = NULL;
	cfg->iface = "mon0";
	cfg->listen_addr = "0.0.0.0:5000";
	cfg->node_id = 1;
	cfg->uart_path = "/dev/ttyS3";
	cfg->uart_baud = 420000U;
	cfg->output_mode = OUTPUT_MODE_UART;
}

static void usage(FILE *stream)
{
	fprintf(stream,
		"usage: ulamad [options]\n"
		"  --transport udp|unow       transport backend (default: unow)\n"
		"  --config PATH              optional config file (default: disabled)\n"
		"  --iface IFACE              monitor interface for unow (default: mon0)\n"
		"  --listen IP:PORT           udp listen endpoint (default: 0.0.0.0:5000)\n"
		"  --node ID                  local ULAMA node id (default: 1)\n"
		"  --uart PATH                UART output path (default: /dev/ttyS3)\n"
		"  --baud RATE                UART baud (default: 420000)\n"
		"  --output PATH              write CRSF frames to file instead of UART\n"
		"  --stdout                   write CRSF frames to stdout instead of UART\n"
		"  --count N                  exit after N accepted frames (default: 0 = forever)\n"
		"  --ready-file PATH          create marker file after transport/output init\n"
		"  --verbose                  print decoded channel summary\n");
}

static int trim_in_place(char *text)
{
	char *start;
	char *end;

	if (text == NULL) {
		return -1;
	}
	start = text;
	while (*start != '\0' && (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')) {
		start++;
	}
	if (start != text) {
		memmove(text, start, strlen(start) + 1U);
	}
	end = text + strlen(text);
	while (end > text) {
		char ch = end[-1];
		if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
			break;
		}
		end--;
	}
	*end = '\0';
	return 0;
}

static void on_signal(int signum)
{
	(void)signum;
	g_stop = 1;
}

static void install_signals(void)
{
	(void)signal(SIGINT, on_signal);
	(void)signal(SIGTERM, on_signal);
}

static int parse_u8(const char *text, uint8_t *out)
{
	char *endptr;
	long value;

	if (text == NULL || out == NULL) {
		return -1;
	}
	value = strtol(text, &endptr, 10);
	if (endptr == text || *endptr != '\0' || value < 1 || value > 255) {
		return -1;
	}
	*out = (uint8_t)value;
	return 0;
}

static int parse_u32(const char *text, uint32_t *out)
{
	char *endptr;
	unsigned long value;

	if (text == NULL || out == NULL) {
		return -1;
	}
	value = strtoul(text, &endptr, 10);
	if (endptr == text || *endptr != '\0') {
		return -1;
	}
	*out = (uint32_t)value;
	return 0;
}

static int parse_uint(const char *text, unsigned int *out)
{
	char *endptr;
	unsigned long value;

	if (text == NULL || out == NULL) {
		return -1;
	}
	value = strtoul(text, &endptr, 10);
	if (endptr == text || *endptr != '\0') {
		return -1;
	}
	*out = (unsigned int)value;
	return 0;
}

static int parse_bool_text(const char *text, bool *out)
{
	if (text == NULL || out == NULL) {
		return -1;
	}
	if (strcmp(text, "1") == 0 || strcasecmp(text, "true") == 0 || strcasecmp(text, "yes") == 0 || strcasecmp(text, "on") == 0) {
		*out = true;
		return 0;
	}
	if (strcmp(text, "0") == 0 || strcasecmp(text, "false") == 0 || strcasecmp(text, "no") == 0 || strcasecmp(text, "off") == 0) {
		*out = false;
		return 0;
	}
	return -1;
}

static int apply_config_key(app_config_t *cfg, const char *key, const char *value)
{
	if (strcmp(key, "transport") == 0) {
		ulama_transport_kind_t kind = ulama_transport_parse_kind(value);
		if (kind == ULAMA_TRANSPORT_KIND_UNSPEC) {
			return -1;
		}
		cfg->transport_kind = kind;
		return 0;
	}
	if (strcmp(key, "iface") == 0) {
		cfg->iface = strdup(value);
		return cfg->iface == NULL ? -1 : 0;
	}
	if (strcmp(key, "listen") == 0) {
		cfg->listen_addr = strdup(value);
		return cfg->listen_addr == NULL ? -1 : 0;
	}
	if (strcmp(key, "node") == 0 || strcmp(key, "node_id") == 0) {
		return parse_u8(value, &cfg->node_id);
	}
	if (strcmp(key, "uart") == 0 || strcmp(key, "uart_path") == 0) {
		cfg->uart_path = strdup(value);
		cfg->output_mode = OUTPUT_MODE_UART;
		return cfg->uart_path == NULL ? -1 : 0;
	}
	if (strcmp(key, "baud") == 0 || strcmp(key, "uart_baud") == 0) {
		return parse_u32(value, &cfg->uart_baud);
	}
	if (strcmp(key, "output") == 0 || strcmp(key, "output_path") == 0) {
		cfg->output_path = strdup(value);
		cfg->output_mode = OUTPUT_MODE_FILE;
		return cfg->output_path == NULL ? -1 : 0;
	}
	if (strcmp(key, "stdout") == 0) {
		bool enabled = false;
		if (parse_bool_text(value, &enabled) != 0) {
			return -1;
		}
		if (enabled) {
			cfg->output_mode = OUTPUT_MODE_STDOUT;
		}
		return 0;
	}
	if (strcmp(key, "count") == 0) {
		return parse_uint(value, &cfg->frame_limit);
	}
	if (strcmp(key, "ready_file") == 0 || strcmp(key, "ready-path") == 0) {
		cfg->ready_path = strdup(value);
		return cfg->ready_path == NULL ? -1 : 0;
	}
	if (strcmp(key, "verbose") == 0) {
		return parse_bool_text(value, &cfg->verbose);
	}
	return 0;
}

static int load_config_file(app_config_t *cfg, const char *path)
{
	FILE *file;
	char line[512];
	unsigned int line_no = 0;

	if (cfg == NULL || path == NULL) {
		return -1;
	}
	file = fopen(path, "rb");
	if (file == NULL) {
		return -1;
	}
	while (fgets(line, sizeof(line), file) != NULL) {
		char *eq;
		char *key;
		char *value;

		line_no++;
		trim_in_place(line);
		if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
			continue;
		}
		if (line[0] == '[') {
			continue;
		}
		eq = strchr(line, '=');
		if (eq == NULL) {
			fprintf(stderr, "config parse error %s:%u: missing '='\n", path, line_no);
			fclose(file);
			return -1;
		}
		*eq = '\0';
		key = line;
		value = eq + 1;
		trim_in_place(key);
		trim_in_place(value);
		if (apply_config_key(cfg, key, value) != 0) {
			fprintf(stderr, "config parse error %s:%u: invalid value for %s\n", path, line_no, key);
			fclose(file);
			return -1;
		}
	}
	fclose(file);
	return 0;
}

static int create_ready_file(const char *path)
{
	FILE *file;

	if (path == NULL) {
		return 0;
	}
	file = fopen(path, "wb");
	if (file == NULL) {
		return -1;
	}
	fputs("ready\n", file);
	fclose(file);
	return 0;
}

static int parse_args(int argc, char **argv, app_config_t *cfg)
{
	static const struct option options[] = {
		{"transport", required_argument, NULL, 't'},
		{"config", required_argument, NULL, 'f'},
		{"iface", required_argument, NULL, 'i'},
		{"listen", required_argument, NULL, 'l'},
		{"node", required_argument, NULL, 'n'},
		{"uart", required_argument, NULL, 'u'},
		{"baud", required_argument, NULL, 'b'},
		{"output", required_argument, NULL, 'o'},
		{"stdout", no_argument, NULL, 's'},
		{"count", required_argument, NULL, 'c'},
		{"ready-file", required_argument, NULL, 'r'},
		{"verbose", no_argument, NULL, 'v'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int opt;

	while ((opt = getopt_long(argc, argv, "t:f:i:l:n:u:b:o:sc:r:vh", options, NULL)) != -1) {
		switch (opt) {
		case 't':
			cfg->transport_kind = ulama_transport_parse_kind(optarg);
			if (cfg->transport_kind == ULAMA_TRANSPORT_KIND_UNSPEC) {
				return -1;
			}
			break;
		case 'f':
			cfg->config_path = optarg;
			break;
		case 'i':
			cfg->iface = optarg;
			break;
		case 'l':
			cfg->listen_addr = optarg;
			break;
		case 'n':
			if (parse_u8(optarg, &cfg->node_id) != 0) {
				return -1;
			}
			break;
		case 'u':
			cfg->uart_path = optarg;
			cfg->output_mode = OUTPUT_MODE_UART;
			break;
		case 'b':
			if (parse_u32(optarg, &cfg->uart_baud) != 0) {
				return -1;
			}
			break;
		case 'o':
			cfg->output_path = optarg;
			cfg->output_mode = OUTPUT_MODE_FILE;
			break;
		case 's':
			cfg->output_mode = OUTPUT_MODE_STDOUT;
			break;
		case 'c':
			if (parse_uint(optarg, &cfg->frame_limit) != 0) {
				return -1;
			}
			break;
		case 'r':
			cfg->ready_path = optarg;
			break;
		case 'v':
			cfg->verbose = true;
			break;
		case 'h':
			usage(stdout);
			exit(0);
		default:
			return -1;
		}
	}
	return 0;
}

static void print_channel_summary(const uint16_t channels[ULAMA_CRSF_NUM_CHANNELS], uint16_t seq, int8_t rssi)
{
	fprintf(stderr,
		"seq=%u rssi=%d ch1=%u ch2=%u ch3=%u ch4=%u aux1=%u aux2=%u\n",
		(unsigned int)seq,
		(int)rssi,
		(unsigned int)channels[0],
		(unsigned int)channels[1],
		(unsigned int)channels[2],
		(unsigned int)channels[3],
		(unsigned int)channels[4],
		(unsigned int)channels[5]);
}

int main(int argc, char **argv)
{
	app_config_t cfg;
	ulama_rx_transport_t transport;
	ulama_serial_port_t uart = {.fd = -1, .baud = 0};
	FILE *out_file = NULL;
	uint8_t raw_frame[256];
	unsigned int accepted = 0;
	int exit_code = 1;

	init_defaults(&cfg);
	optind = 1;
	if (parse_args(argc, argv, &cfg) != 0) {
		usage(stderr);
		return 2;
	}
	if (cfg.config_path != NULL && load_config_file(&cfg, cfg.config_path) != 0) {
		fprintf(stderr, "failed to load config %s\n", cfg.config_path);
		return 2;
	}
	optind = 1;
	if (parse_args(argc, argv, &cfg) != 0) {
		usage(stderr);
		return 2;
	}
	install_signals();
	memset(&transport, 0, sizeof(transport));
	transport.fd = -1;

	if (cfg.output_mode == OUTPUT_MODE_UART) {
		if (ulama_serial_open(&uart, cfg.uart_path, cfg.uart_baud) != 0) {
			fprintf(stderr, "failed to open uart %s: %s\n", cfg.uart_path, strerror(errno));
			goto cleanup;
		}
	} else if (cfg.output_mode == OUTPUT_MODE_FILE) {
		out_file = fopen(cfg.output_path, "wb");
		if (out_file == NULL) {
			fprintf(stderr, "failed to open output %s: %s\n", cfg.output_path, strerror(errno));
			goto cleanup;
		}
	} else {
		out_file = stdout;
	}

	if (cfg.transport_kind == ULAMA_TRANSPORT_KIND_UDP) {
		if (ulama_transport_rx_init_udp(&transport, cfg.listen_addr) != 0) {
			fprintf(stderr, "failed to bind udp %s: %s\n", cfg.listen_addr, strerror(errno));
			goto cleanup;
		}
	} else {
		if (ulama_transport_rx_init_unow(&transport, cfg.node_id, cfg.iface) != 0) {
			fprintf(stderr, "failed to init unow iface %s: %s\n", cfg.iface, strerror(errno));
			goto cleanup;
		}
	}

	if (create_ready_file(cfg.ready_path) != 0) {
		fprintf(stderr, "failed to create ready file %s: %s\n", cfg.ready_path, strerror(errno));
		goto cleanup;
	}

	fprintf(stderr,
		"ulamad listening transport=%s node=%u output=%s\n",
		ulama_transport_kind_name(cfg.transport_kind),
		(unsigned int)cfg.node_id,
		cfg.output_mode == OUTPUT_MODE_UART ? cfg.uart_path : (cfg.output_mode == OUTPUT_MODE_FILE ? cfg.output_path : "stdout"));

	while (!g_stop) {
		ulama_frame_view_t view;
		uint16_t channels[ULAMA_CRSF_NUM_CHANNELS];
		uint8_t address = 0;
		int8_t rssi = 0;
		ssize_t received = ulama_transport_rx_recv(&transport, raw_frame, sizeof(raw_frame), 250, NULL, &rssi);

		if (received < 0) {
			fprintf(stderr, "receive failed: %s\n", strerror(errno));
			goto cleanup;
		}
		if (received == 0) {
			continue;
		}
		if (!ulama_frame_unpack(raw_frame, (size_t)received, &view)) {
			fprintf(stderr, "drop: bad ulama frame len=%zd\n", received);
			continue;
		}
		if (view.dst_node != cfg.node_id && view.dst_node != 0xFFU) {
			continue;
		}
		if (view.traffic_class != ULAMA_CLASS_CTRL) {
			continue;
		}
		if (!ulama_crsf_parse_rc_channels_frame(view.payload, view.payload_len, &address, channels)) {
			fprintf(stderr, "drop: invalid crsf payload len=%zu\n", view.payload_len);
			continue;
		}
		if (address != ULAMA_CRSF_ADDRESS_FLIGHT_CONTROLLER) {
			fprintf(stderr, "drop: unexpected crsf address 0x%02x\n", address);
			continue;
		}
		if (cfg.output_mode == OUTPUT_MODE_UART) {
			if (ulama_serial_write_all(&uart, view.payload, view.payload_len) < 0) {
				fprintf(stderr, "uart write failed: %s\n", strerror(errno));
				goto cleanup;
			}
		} else {
			if (fwrite(view.payload, 1, view.payload_len, out_file) != view.payload_len) {
				fprintf(stderr, "output write failed\n");
				goto cleanup;
			}
			fflush(out_file);
		}
		accepted++;
		if (cfg.verbose) {
			print_channel_summary(channels, view.seq, rssi);
		}
		if (cfg.frame_limit != 0U && accepted >= cfg.frame_limit) {
			break;
		}
	}

	exit_code = 0;

cleanup:
	if (out_file != NULL && out_file != stdout) {
		fclose(out_file);
	}
	ulama_serial_close(&uart);
	ulama_transport_rx_close(&transport);
	return exit_code;
}