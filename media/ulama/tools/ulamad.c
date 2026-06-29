#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define KEEPALIVE_HZ          150U
#define KEEPALIVE_INTERVAL_NS (1000000000LL / (int64_t)KEEPALIVE_HZ)

#include "ulama/crsf.h"
#include "ulama/msp.h"
#include "ulama/serial_uart.h"
#include "ulama/transport.h"
#include "ulama/ulama_frame.h"
#include "ulama/ulama_version.h"

#define MSP_POLL_INTERVAL_MS 500
#define MSP_POLL_CODES_COUNT 6
static const uint16_t MSP_POLL_CODES[MSP_POLL_CODES_COUNT] = {
	MSP_ATTITUDE, MSP_ALTITUDE, MSP_ANALOG, MSP_RAW_GPS, MSP_STATUS, MSP_BATTERY_STATE,
};

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
	unsigned int keepalive_timeout_s;
	bool verbose;
	const char *msp_uart_path;
	uint32_t msp_uart_baud;
	const char *tx_peer;
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
	cfg->keepalive_timeout_s = 2U;
	cfg->msp_uart_path = NULL;
	cfg->msp_uart_baud = 115200U;
	cfg->tx_peer = NULL;
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
		"  --keepalive-timeout SECS   stop keepalive if no WiFi frame for N seconds (default: 2, 0=forever)\n"
		"  --ready-file PATH          create marker file after transport/output init\n"
		"  --verbose                  print decoded channel summary\n"
		"  --msp-uart PATH            UART to read MSP telemetry from FC\n"
		"  --msp-baud RATE            MSP UART baud rate (default: 115200)\n"
		"  --tx-peer IP:PORT          send ULAMA TELEMETRY frames to (UDP)\n");
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
	if (strcmp(key, "keepalive_timeout") == 0 || strcmp(key, "keepalive-timeout") == 0) {
		return parse_uint(value, &cfg->keepalive_timeout_s);
	}
	if (strcmp(key, "ready_file") == 0 || strcmp(key, "ready-path") == 0) {
		cfg->ready_path = strdup(value);
		return cfg->ready_path == NULL ? -1 : 0;
	}
	if (strcmp(key, "verbose") == 0) {
		return parse_bool_text(value, &cfg->verbose);
	}
	if (strcmp(key, "msp_uart") == 0 || strcmp(key, "msp-uart") == 0) {
		cfg->msp_uart_path = strdup(value);
		return cfg->msp_uart_path == NULL ? -1 : 0;
	}
	if (strcmp(key, "msp_baud") == 0 || strcmp(key, "msp-baud") == 0) {
		return parse_u32(value, &cfg->msp_uart_baud);
	}
	if (strcmp(key, "tx_peer") == 0 || strcmp(key, "tx-peer") == 0) {
		cfg->tx_peer = strdup(value);
		return cfg->tx_peer == NULL ? -1 : 0;
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
		{"keepalive-timeout", required_argument, NULL, 'k'},
		{"ready-file", required_argument, NULL, 'r'},
		{"verbose", no_argument, NULL, 'v'},
		{"help", no_argument, NULL, 'h'},
		{"msp-uart", required_argument, NULL, 'M'},
		{"msp-baud", required_argument, NULL, 'B'},
		{"tx-peer", required_argument, NULL, 'T'},
		{"version", no_argument, NULL, 'V'},
		{0, 0, 0, 0},
	};
	int opt;

	while ((opt = getopt_long(argc, argv, "t:f:i:l:n:u:b:o:sc:k:r:VvhM:B:T:", options, NULL)) != -1) {
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
		case 'k':
			if (parse_uint(optarg, &cfg->keepalive_timeout_s) != 0) {
				return -1;
			}
			break;
		case 'r':
			cfg->ready_path = optarg;
			break;
		case 'v':
			cfg->verbose = true;
			break;
		case 'M':
			cfg->msp_uart_path = optarg;
			break;
		case 'B':
			if (parse_u32(optarg, &cfg->msp_uart_baud) != 0) {
				return -1;
			}
			break;
		case 'T':
			cfg->tx_peer = optarg;
			break;
		case 'V':
			fprintf(stdout, "ulamad: build #%d (%s@%s) %s\n",
				ULAMA_BUILD_NUMBER, ULAMA_GIT_BRANCH, ULAMA_GIT_HASH, ULAMA_BUILD_DATE);
			exit(0);
		case 'h':
			usage(stdout);
			exit(0);
		default:
			return -1;
		}
	}
	return 0;
}

static int64_t timespec_diff_ns(const struct timespec *a, const struct timespec *b)
{
	return ((int64_t)(a->tv_sec - b->tv_sec) * 1000000000LL) + (int64_t)(a->tv_nsec - b->tv_nsec);
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
	ulama_tx_transport_t tx_transport;
	ulama_serial_port_t uart = {.fd = -1, .baud = 0};
	ulama_serial_port_t msp_uart = {.fd = -1, .baud = 0};
	msp_parser_t msp_parser;
	crsf_telem_parser_t crsf_telem_parser;
	FILE *out_file = NULL;
	uint8_t raw_frame[256];
	unsigned int accepted = 0;
	int exit_code = 1;
	bool has_tx = false;
	bool has_msp = false;
	uint16_t msp_poll_idx = 0;
	uint16_t telem_seq = 0;
	struct timespec last_telem_tx = {0, 0};

	uint8_t last_crsf_frame[ULAMA_CRSF_RC_FRAME_SIZE];
	size_t last_crsf_len = 0;
	bool has_last_frame = false;
	struct timespec last_tx = {0, 0};
	struct timespec last_rx = {0, 0};
	unsigned int keepalive_count = 0;
	bool keepalive_expired_logged = false;

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

	/* Log version information */
	fprintf(stderr, "[ulamad] Build: #%d (%s@%s) %s\n",
		ULAMA_BUILD_NUMBER, ULAMA_GIT_BRANCH, ULAMA_GIT_HASH, ULAMA_BUILD_DATE);

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

	if (cfg.transport_kind == ULAMA_TRANSPORT_KIND_RADIOD) {
		if (ulama_transport_rx_init_radiod(&transport, cfg.node_id, NULL, "ulamad_rx") != 0) {
			fprintf(stderr, "failed to connect to radiod: %s\n", strerror(errno));
			goto cleanup;
		}
	} else if (cfg.transport_kind == ULAMA_TRANSPORT_KIND_UDP) {
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

	memset(&tx_transport, 0, sizeof(tx_transport));
	tx_transport.fd = -1;

	if (cfg.msp_uart_path != NULL) {
		if (ulama_serial_open(&msp_uart, cfg.msp_uart_path, cfg.msp_uart_baud) != 0) {
			fprintf(stderr, "failed to open msp uart %s: %s\n", cfg.msp_uart_path, strerror(errno));
			goto cleanup;
		}
		/* MSP reads must be non-blocking to avoid stalling the main loop */
		{
			int flags = fcntl(msp_uart.fd, F_GETFL, 0);
			if (flags >= 0)
				fcntl(msp_uart.fd, F_SETFL, flags | O_NONBLOCK);
		}
		msp_parser_init(&msp_parser);
		crsf_telem_parser_init(&crsf_telem_parser);
		has_msp = true;
		fprintf(stderr, "ulamad msp: uart=%s baud=%u\n", cfg.msp_uart_path, (unsigned)cfg.msp_uart_baud);

		if (cfg.transport_kind == ULAMA_TRANSPORT_KIND_RADIOD) {
			if (ulama_transport_tx_init_radiod(&tx_transport, cfg.node_id, NULL, "ulamad_tx") != 0) {
				fprintf(stderr, "failed to connect to radiod for tx: %s\n", strerror(errno));
				goto cleanup;
			}
			has_tx = true;
		} else if (cfg.tx_peer != NULL) {
			if (cfg.transport_kind == ULAMA_TRANSPORT_KIND_UDP) {
				if (ulama_transport_tx_init_udp(&tx_transport, cfg.tx_peer) != 0) {
					fprintf(stderr, "failed to init tx peer %s: %s\n", cfg.tx_peer, strerror(errno));
					goto cleanup;
				}
			} else {
				if (ulama_transport_tx_init_unow(&tx_transport, cfg.node_id, cfg.iface, NULL) != 0) {
					fprintf(stderr, "failed to init unow tx: %s\n", strerror(errno));
					goto cleanup;
				}
			}
			has_tx = true;
		} else if (cfg.transport_kind == ULAMA_TRANSPORT_KIND_UNOW) {
			if (ulama_transport_tx_init_unow(&tx_transport, cfg.node_id, cfg.iface, NULL) != 0) {
				fprintf(stderr, "failed to init unow tx for telemetry: %s\n", strerror(errno));
				goto cleanup;
			}
			has_tx = true;
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

	time_t last_stats = time(NULL);
	struct timespec last_msp_poll = {0, 0};

	while (!g_stop) {
		ulama_frame_view_t view;
		uint16_t channels[ULAMA_CRSF_NUM_CHANNELS];
		uint8_t address = 0;
		int8_t rssi = 0;

		/* Wake up early enough to honour the 150 Hz keepalive deadline. */
		int recv_timeout_ms = 250;
		if (has_last_frame && cfg.output_mode == OUTPUT_MODE_UART) {
			struct timespec ts_now;
			clock_gettime(CLOCK_MONOTONIC, &ts_now);
			bool within_timeout = (cfg.keepalive_timeout_s == 0U) ||
				(timespec_diff_ns(&ts_now, &last_rx) < (int64_t)cfg.keepalive_timeout_s * 1000000000LL);
			if (within_timeout) {
				int64_t remaining_ns = KEEPALIVE_INTERVAL_NS - timespec_diff_ns(&ts_now, &last_tx);
				if (remaining_ns <= 0) {
					recv_timeout_ms = 0;
				} else {
					int ms = (int)(remaining_ns / 1000000LL);
					recv_timeout_ms = (ms < 1) ? 1 : (ms < 250 ? ms : 250);
				}
			}
		}

		ssize_t received = ulama_transport_rx_recv(&transport, raw_frame, sizeof(raw_frame), recv_timeout_ms, NULL, &rssi);

		/* Periodically log statistics */
		time_t stats_now = time(NULL);
		if (stats_now - last_stats >= 5) {
			if (cfg.transport_kind == ULAMA_TRANSPORT_KIND_UNOW) {
				fprintf(stderr, "[stats] accepted=%u keepalive=%u (uptime=%lds)\n",
					(unsigned int)accepted,
					(unsigned int)keepalive_count,
					(long)(stats_now - (last_stats - 5)));
			}
			last_stats = stats_now;
		}

		if (received < 0) {
			/* WiFi adapter disappeared (USB hub reset). UART to FC is still
			 * alive — continue keepalive so FC does not enter failsafe.
			 * Try to re-initialize transport after adapter re-enumerates. */
			fprintf(stderr, "[transport] lost: %s — entering recovery mode\n", strerror(errno));
			ulama_transport_rx_close(&transport);
			memset(&transport, 0, sizeof(transport));
			transport.fd = -1;

			/* Recovery loop: keepalive to FC + retry transport init */
			while (!g_stop) {
				/* Keepalive to FC while WiFi is down */
				if (has_last_frame && cfg.output_mode == OUTPUT_MODE_UART) {
					struct timespec ts_ka;
					clock_gettime(CLOCK_MONOTONIC, &ts_ka);
					bool within_timeout = (cfg.keepalive_timeout_s == 0U) ||
						(timespec_diff_ns(&ts_ka, &last_rx) < (int64_t)cfg.keepalive_timeout_s * 1000000000LL);
					if (within_timeout && timespec_diff_ns(&ts_ka, &last_tx) >= KEEPALIVE_INTERVAL_NS) {
						ulama_serial_write_all(&uart, last_crsf_frame, last_crsf_len);
						last_tx = ts_ka;
						keepalive_count++;
					}
				}

				/* Try to re-init transport every ~3 seconds */
				usleep(20000); /* 20ms — maintain keepalive cadence */
				static unsigned int retry_counter = 0;
				retry_counter++;
				if (retry_counter % 150 != 0) { /* ~3 seconds at 20ms */
					continue;
				}

				fprintf(stderr, "[transport] attempting reconnect (iface=%s)...\n", cfg.iface);

				/* Restore monitor mode before re-opening pcap.
				 * After USB re-enumeration the interface appears
				 * as a NEW phy in managed mode. Full sequence:
				 *   1. Wait for interface to exist
				 *   2. ip link set down
				 *   3. iw set type monitor (critical — driver defaults to managed)
				 *   4. ip link set up
				 *   5. iw set channel
				 * Timing: USB re-enum takes ~4s (hub + device + driver). */
				if (cfg.transport_kind == ULAMA_TRANSPORT_KIND_UNOW) {
					char cmd[512];
					snprintf(cmd, sizeof(cmd),
						"for i in 1 2 3 4 5 6 7 8 9 10; do "
						"  ip link show %s >/dev/null 2>&1 && break; "
						"  sleep 1; "
						"done; "
						"ip link set %s down 2>/dev/null; "
						"iw dev %s set type monitor 2>/dev/null; "
						"ip link set %s up 2>/dev/null; "
						"iw dev %s set channel 6 2>/dev/null",
						cfg.iface,
						cfg.iface, cfg.iface, cfg.iface, cfg.iface);
					(void)system(cmd);
					usleep(1000000); /* 1s settle after mode change */
				}

				int rc;
				if (cfg.transport_kind == ULAMA_TRANSPORT_KIND_RADIOD) {
					rc = ulama_transport_rx_init_radiod(&transport, cfg.node_id, NULL, "ulamad_rx");
				} else if (cfg.transport_kind == ULAMA_TRANSPORT_KIND_UDP) {
					rc = ulama_transport_rx_init_udp(&transport, cfg.listen_addr);
				} else {
					rc = ulama_transport_rx_init_unow(&transport, cfg.node_id, cfg.iface);
				}
				if (rc == 0) {
					/* Verify interface is ready: try a short recv.
					 * If it fails immediately, the adapter is not
					 * stable yet (monitor mode not set, etc). */
					uint8_t probe[256];
					ssize_t probe_rc = ulama_transport_rx_recv(&transport, probe, sizeof(probe), 2000, NULL, NULL);
					if (probe_rc < 0) {
						fprintf(stderr, "[transport] reconnect unstable, retrying...\n");
						ulama_transport_rx_close(&transport);
						memset(&transport, 0, sizeof(transport));
						transport.fd = -1;
						continue;
					}
					fprintf(stderr, "[transport] reconnected and stable\n");
					/* Also reinit TX if needed */
					if (has_tx) {
						ulama_transport_tx_close(&tx_transport);
						if (cfg.transport_kind == ULAMA_TRANSPORT_KIND_RADIOD) {
							ulama_transport_tx_init_radiod(&tx_transport, cfg.node_id, NULL, "ulamad_tx");
						} else if (cfg.transport_kind == ULAMA_TRANSPORT_KIND_UNOW) {
							ulama_transport_tx_init_unow(&tx_transport, cfg.node_id, cfg.iface, NULL);
						}
					}
					break; /* Resume main loop */
				}
			}
			continue; /* Back to main recv loop */
		}
		if (received == 0) {
			goto keepalive;
		}
		if (!ulama_frame_unpack(raw_frame, (size_t)received, &view)) {
			fprintf(stderr, "drop: bad ulama frame len=%zd\n", received);
			goto keepalive;
		}
		if (view.dst_node != cfg.node_id && view.dst_node != 0xFFU) {
			goto keepalive;
		}
		if (view.traffic_class != ULAMA_CLASS_CTRL) {
			goto keepalive;
		}

		/* Handle DISCOVER broadcast: reply with ANNOUNCE */
		if (view.payload_len == 8 && memcmp(view.payload, "DISCOVER", 8) == 0) {
			if (has_tx) {
				char announce[32];
				int alen = snprintf(announce, sizeof(announce), "ANNOUNCE:C-%03d", cfg.node_id);
				ulama_frame_view_t af = {
					.src_node = cfg.node_id,
					.dst_node = 0xFF,
					.flags = ULAMA_FLAG_DUP_ALLOWED,
					.traffic_class = ULAMA_CLASS_CTRL,
					.seq = telem_seq++,
					.frag_idx = 0,
					.frag_total = 1,
					.ttl = ULAMA_FRAME_DEFAULT_TTL,
					.payload = (const uint8_t *)announce,
					.payload_len = (size_t)alen,
				};
				uint8_t abuf[ULAMA_FRAME_HEADER_SIZE + 64];
				size_t abuflen = 0;
				if (ulama_frame_pack(&af, abuf, sizeof(abuf), &abuflen))
					ulama_transport_tx_send(&tx_transport, abuf, abuflen);
				fprintf(stderr, "[discover] replied: %s\n", announce);
			}
			goto keepalive;
		}

		/* LTS NACK from ulama-gw: magic 0x4C 0x4E ("LN"), not a CRSF frame */
		if (view.payload_len >= 2 &&
		    view.payload[0] == 0x4C && view.payload[1] == 0x4E) {
			goto keepalive;
		}

		if (!ulama_crsf_parse_rc_channels_frame(view.payload, view.payload_len, &address, channels)) {
			fprintf(stderr, "drop: invalid crsf payload len=%zu hex=", view.payload_len);
			for (size_t di = 0; di < view.payload_len && di < 16; di++)
				fprintf(stderr, "%02x", view.payload[di]);
			fprintf(stderr, "\n");
			goto keepalive;
		}
		if (address != ULAMA_CRSF_ADDRESS_FLIGHT_CONTROLLER) {
			fprintf(stderr, "drop: unexpected crsf address 0x%02x\n", address);
			goto keepalive;
		}
		{
			/* payload[1] is the CRSF length field (bytes after it), so the total
			 * CRSF frame is payload[1]+2 bytes. This trims any trailing bytes
			 * that 802.11 capture drivers (e.g. 8192eu) append to pcap frames,
			 * such as the 4-byte 802.11 FCS. */
			size_t crsf_len = (view.payload_len >= 2U)
				? ((size_t)view.payload[1] + 2U)
				: view.payload_len;
			if (crsf_len > view.payload_len) {
				crsf_len = view.payload_len;
			}
			if (cfg.output_mode == OUTPUT_MODE_UART) {
				if (ulama_serial_write_all(&uart, view.payload, crsf_len) < 0) {
					fprintf(stderr, "uart write failed: %s\n", strerror(errno));
					goto cleanup;
				}
			} else {
				if (fwrite(view.payload, 1, crsf_len, out_file) != crsf_len) {
					fprintf(stderr, "output write failed\n");
					goto cleanup;
				}
				fflush(out_file);
			}
			/* Save frame for keepalive retransmit. last_tx/last_rx are set here
			 * so the keepalive check below does not double-send on the same
			 * iteration, and the timeout window is reset. */
			memcpy(last_crsf_frame, view.payload, crsf_len);
			last_crsf_len = crsf_len;
			clock_gettime(CLOCK_MONOTONIC, &last_tx);
			last_rx = last_tx;
			has_last_frame = true;
			keepalive_expired_logged = false;

			accepted++;
			if (cfg.verbose) {
				print_channel_summary(channels, view.seq, rssi);
			}
			if (cfg.frame_limit != 0U && accepted >= cfg.frame_limit) {
				break;
			}
		}

		/* MSP telemetry polling: send request to FC, read & forward responses */
		if (has_msp) {
			struct timespec ts_msp;
			clock_gettime(CLOCK_MONOTONIC, &ts_msp);
			int64_t msp_elapsed_ms = (timespec_diff_ns(&ts_msp, &last_msp_poll) / 1000000LL);
			if (msp_elapsed_ms >= MSP_POLL_INTERVAL_MS || last_msp_poll.tv_sec == 0) {
				uint8_t req_buf[16];
				size_t req_len = msp_v1_build_request(MSP_POLL_CODES[msp_poll_idx], req_buf, sizeof(req_buf));
				if (req_len > 0)
					ulama_serial_write_all(&msp_uart, req_buf, req_len);
				msp_poll_idx = (msp_poll_idx + 1) % MSP_POLL_CODES_COUNT;
				last_msp_poll = ts_msp;
			}

			uint8_t msp_byte;
			while (read(msp_uart.fd, &msp_byte, 1) == 1) {
				/* Try MSP parser */
				msp_message_t msp_msg;
				if (msp_parser_feed(&msp_parser, msp_byte, &msp_msg)) {
					if (has_tx && msp_msg.direction == MSP_DIR_RESPONSE) {
						struct timespec ts_telem;
						clock_gettime(CLOCK_MONOTONIC, &ts_telem);
						if (timespec_diff_ns(&ts_telem, &last_telem_tx) >= MSP_POLL_INTERVAL_MS * 1000000LL) {
							uint8_t msp_wire[MSP_V2_OVERHEAD + MSP_MAX_PAYLOAD];
							size_t wire_len = msp_v1_build_response(msp_msg.code, msp_msg.payload,
									msp_msg.payload_len, msp_wire, sizeof(msp_wire));
							if (wire_len > 0 && wire_len <= ULAMA_FRAME_MAX_PAYLOAD) {
								ulama_frame_view_t tf = {
									.src_node = cfg.node_id,
									.dst_node = 0xFF,
									.flags = ULAMA_FLAG_DUP_ALLOWED,
									.traffic_class = ULAMA_CLASS_TELEMETRY,
									.seq = telem_seq++,
									.frag_idx = 0,
									.frag_total = 1,
									.ttl = ULAMA_FRAME_DEFAULT_TTL,
									.payload = msp_wire,
									.payload_len = wire_len,
								};
								uint8_t telem_frame[ULAMA_FRAME_HEADER_SIZE + ULAMA_FRAME_MAX_PAYLOAD];
								size_t telem_len = 0;
								if (ulama_frame_pack(&tf, telem_frame, sizeof(telem_frame), &telem_len)) {
									ulama_transport_tx_send(&tx_transport, telem_frame, telem_len);
									last_telem_tx = ts_telem;
								}
							}
						}
					}
					if (cfg.verbose) {
						fprintf(stderr, "[msp] v%d code=%u len=%u\n",
							msp_msg.version, msp_msg.code, msp_msg.payload_len);
					}
				}
				/* Try CRSF telemetry parser (battery, GPS, attitude, etc.) */
				uint8_t crsf_frame[ULAMA_CRSF_MAX_FRAME_SIZE];
				uint8_t crsf_len = 0;
				if (crsf_telem_parser_feed(&crsf_telem_parser, msp_byte, crsf_frame, &crsf_len)) {
					if (has_tx && crsf_len <= ULAMA_FRAME_MAX_PAYLOAD) {
						struct timespec ts_telem;
						clock_gettime(CLOCK_MONOTONIC, &ts_telem);
						if (timespec_diff_ns(&ts_telem, &last_telem_tx) >= MSP_POLL_INTERVAL_MS * 1000000LL) {
							ulama_frame_view_t tf = {
								.src_node = cfg.node_id,
								.dst_node = 0xFF,
								.flags = ULAMA_FLAG_DUP_ALLOWED,
								.traffic_class = ULAMA_CLASS_TELEMETRY,
								.seq = telem_seq++,
								.frag_idx = 0,
								.frag_total = 1,
								.ttl = ULAMA_FRAME_DEFAULT_TTL,
								.payload = crsf_frame,
								.payload_len = crsf_len,
							};
							uint8_t telem_frame[ULAMA_FRAME_HEADER_SIZE + ULAMA_FRAME_MAX_PAYLOAD];
							size_t telem_len = 0;
							if (ulama_frame_pack(&tf, telem_frame, sizeof(telem_frame), &telem_len)) {
								ulama_transport_tx_send(&tx_transport, telem_frame, telem_len);
								last_telem_tx = ts_telem;
							}
						}
					}
					if (cfg.verbose) {
						fprintf(stderr, "[crsf-telem] type=0x%02x len=%u\n",
							crsf_frame[2], crsf_len);
					}
				}
			}
		}

keepalive:
		/* Retransmit the last CRSF frame at 150 Hz so the flight controller
		 * does not trigger failsafe when incoming ULAMA frames are absent.
		 * Stops retransmitting after keepalive_timeout_s of no new WiFi frames
		 * (0 = transmit forever). */
		if (has_last_frame && cfg.output_mode == OUTPUT_MODE_UART) {
			struct timespec ts_now;
			clock_gettime(CLOCK_MONOTONIC, &ts_now);
			bool within_timeout = (cfg.keepalive_timeout_s == 0U) ||
				(timespec_diff_ns(&ts_now, &last_rx) < (int64_t)cfg.keepalive_timeout_s * 1000000000LL);
			if (!within_timeout) {
				if (!keepalive_expired_logged) {
					fprintf(stderr, "[keepalive] timeout: no WiFi frame for %u s, stopping CRSF output\n",
						cfg.keepalive_timeout_s);
					keepalive_expired_logged = true;
				}
			} else if (timespec_diff_ns(&ts_now, &last_tx) >= KEEPALIVE_INTERVAL_NS) {
				if (ulama_serial_write_all(&uart, last_crsf_frame, last_crsf_len) < 0) {
					fprintf(stderr, "uart write failed (keepalive): %s\n", strerror(errno));
					goto cleanup;
				}
				last_tx = ts_now;
				keepalive_count++;
			}
		}
	}

	exit_code = 0;

cleanup:
	if (out_file != NULL && out_file != stdout) {
		fclose(out_file);
	}
	ulama_serial_close(&uart);
	ulama_serial_close(&msp_uart);
	ulama_transport_rx_close(&transport);
	if (has_tx)
		ulama_transport_tx_close(&tx_transport);
	return exit_code;
}