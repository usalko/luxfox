#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/joystick.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ulama/crsf.h"
#include "ulama/transport.h"
#include "ulama/ulama_frame.h"

static volatile sig_atomic_t g_stop;

typedef struct {
	ulama_transport_kind_t transport_kind;
	const char *iface;
	const char *peer;
	const char *dst_mac_text;
	const char *joystick_path;
	const char *channels_text;
	const char *crsf_out_path;
	uint8_t src_node;
	uint8_t dst_node;
	uint8_t ttl;
	unsigned int rate_hz;
	unsigned int frame_count;
	bool arm;
	bool verbose;
} app_config_t;

static void usage(FILE *stream)
{
	fprintf(stream,
		"usage: ulama_js_tx [options]\n"
		"  --transport udp|unow       transport backend (default: udp)\n"
		"  --peer IP:PORT             udp destination (default: 127.0.0.1:5000)\n"
		"  --iface IFACE              monitor interface for unow (default: mon0)\n"
		"  --dst-mac MAC              unow destination MAC\n"
		"  --src-node ID              source ULAMA node id (default: 10)\n"
		"  --dst-node ID              destination ULAMA node id (default: 1)\n"
		"  --ttl N                    ULAMA TTL (default: 1)\n"
		"  --joystick PATH            read live joystick events from /dev/input/js0\n"
		"  --channels CSV             one-shot CRSF channel values\n"
		"  --arm                      force AUX1 high\n"
		"  --rate-hz N                send rate for live joystick mode (default: 50)\n"
		"  --count N                  number of frames to send (default: 1, joystick mode: 0 = forever)\n"
		"  --crsf-out PATH            dump emitted CRSF bytes to file\n"
		"  --verbose                  print channel summary on each send\n");
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

static int parse_u8_range(const char *text, uint8_t min_value, uint8_t max_value, uint8_t *out)
{
	char *endptr;
	long value;

	if (text == NULL || out == NULL) {
		return -1;
	}
	value = strtol(text, &endptr, 10);
	if (endptr == text || *endptr != '\0' || value < min_value || value > max_value) {
		return -1;
	}
	*out = (uint8_t)value;
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

static int parse_channels_csv(const char *text, uint16_t channels[ULAMA_CRSF_NUM_CHANNELS])
{
	char *buffer;
	char *cursor;
	char *token;
	size_t index = 0;

	if (text == NULL || channels == NULL) {
		return -1;
	}
	buffer = strdup(text);
	if (buffer == NULL) {
		return -1;
	}
	cursor = buffer;
	while ((token = strsep(&cursor, ",")) != NULL && index < ULAMA_CRSF_NUM_CHANNELS) {
		char *endptr;
		unsigned long value;

		if (*token == '\0') {
			index++;
			continue;
		}
		value = strtoul(token, &endptr, 10);
		if (endptr == token || *endptr != '\0' || value > 0xFFFFUL) {
			free(buffer);
			return -1;
		}
		channels[index++] = ulama_crsf_clamp((uint16_t)value);
	}
	free(buffer);
	return 0;
}

static uint64_t monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void print_channel_summary(const uint16_t channels[ULAMA_CRSF_NUM_CHANNELS], uint16_t seq)
{
	fprintf(stderr,
		"tx seq=%u ch1=%u ch2=%u ch3=%u ch4=%u aux1=%u aux2=%u\n",
		(unsigned int)seq,
		(unsigned int)channels[0],
		(unsigned int)channels[1],
		(unsigned int)channels[2],
		(unsigned int)channels[3],
		(unsigned int)channels[4],
		(unsigned int)channels[5]);
}

static int write_blob(FILE *file, const uint8_t *data, size_t len)
{
	return fwrite(data, 1, len, file) == len ? 0 : -1;
}

static void apply_joystick_event(struct js_event event,
	uint16_t channels[ULAMA_CRSF_NUM_CHANNELS],
	int button_state[ULAMA_CRSF_NUM_CHANNELS],
	bool *arm_state)
{
	event.type &= (uint8_t)~JS_EVENT_INIT;
	if (event.type == JS_EVENT_AXIS) {
		switch (event.number) {
		case 0:
			channels[0] = ulama_crsf_axis_to_bipolar(event.value, false);
			break;
		case 1:
			channels[1] = ulama_crsf_axis_to_bipolar(event.value, true);
			break;
		case 2:
			channels[2] = ulama_crsf_axis_to_throttle(event.value, true);
			break;
		case 3:
			channels[3] = ulama_crsf_axis_to_bipolar(event.value, false);
			break;
		default:
			break;
		}
		return;
	}
	if (event.type == JS_EVENT_BUTTON && event.number < ULAMA_CRSF_NUM_CHANNELS) {
		if (event.number == 0 && event.value != 0 && button_state[0] == 0) {
			*arm_state = !*arm_state;
			channels[4] = *arm_state ? ULAMA_CRSF_VALUE_MAX : ULAMA_CRSF_VALUE_MIN;
		}
		button_state[event.number] = event.value;
		if (event.number > 0 && (size_t)(4 + event.number) < ULAMA_CRSF_NUM_CHANNELS) {
			channels[4 + event.number] = event.value != 0 ? ULAMA_CRSF_VALUE_MAX : ULAMA_CRSF_VALUE_MIN;
		}
	}
}

static int parse_args(int argc, char **argv, app_config_t *cfg)
{
	static const struct option options[] = {
		{"transport", required_argument, NULL, 't'},
		{"peer", required_argument, NULL, 'p'},
		{"iface", required_argument, NULL, 'i'},
		{"dst-mac", required_argument, NULL, 'm'},
		{"src-node", required_argument, NULL, 's'},
		{"dst-node", required_argument, NULL, 'd'},
		{"ttl", required_argument, NULL, 'l'},
		{"joystick", required_argument, NULL, 'j'},
		{"channels", required_argument, NULL, 'c'},
		{"arm", no_argument, NULL, 'a'},
		{"rate-hz", required_argument, NULL, 'r'},
		{"count", required_argument, NULL, 'n'},
		{"crsf-out", required_argument, NULL, 'o'},
		{"verbose", no_argument, NULL, 'v'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int opt;

	memset(cfg, 0, sizeof(*cfg));
	cfg->transport_kind = ULAMA_TRANSPORT_KIND_UDP;
	cfg->peer = "127.0.0.1:5000";
	cfg->iface = "mon0";
	cfg->src_node = 10;
	cfg->dst_node = 1;
	cfg->ttl = ULAMA_FRAME_ONE_HOP_TTL;
	cfg->rate_hz = 50U;
	cfg->frame_count = 1U;

	while ((opt = getopt_long(argc, argv, "t:p:i:m:s:d:l:j:c:ar:n:o:vh", options, NULL)) != -1) {
		switch (opt) {
		case 't':
			cfg->transport_kind = ulama_transport_parse_kind(optarg);
			if (cfg->transport_kind == ULAMA_TRANSPORT_KIND_UNSPEC) {
				return -1;
			}
			break;
		case 'p':
			cfg->peer = optarg;
			break;
		case 'i':
			cfg->iface = optarg;
			break;
		case 'm':
			cfg->dst_mac_text = optarg;
			break;
		case 's':
			if (parse_u8_range(optarg, 1U, 255U, &cfg->src_node) != 0) {
				return -1;
			}
			break;
		case 'd':
			if (parse_u8_range(optarg, 1U, 255U, &cfg->dst_node) != 0) {
				return -1;
			}
			break;
		case 'l':
			if (parse_u8_range(optarg, 1U, 32U, &cfg->ttl) != 0) {
				return -1;
			}
			break;
		case 'j':
			cfg->joystick_path = optarg;
			cfg->frame_count = 0U;
			break;
		case 'c':
			cfg->channels_text = optarg;
			break;
		case 'a':
			cfg->arm = true;
			break;
		case 'r':
			if (parse_uint(optarg, &cfg->rate_hz) != 0 || cfg->rate_hz == 0U) {
				return -1;
			}
			break;
		case 'n':
			if (parse_uint(optarg, &cfg->frame_count) != 0) {
				return -1;
			}
			break;
		case 'o':
			cfg->crsf_out_path = optarg;
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

int main(int argc, char **argv)
{
	app_config_t cfg;
	ulama_tx_transport_t transport;
	uint16_t channels[ULAMA_CRSF_NUM_CHANNELS];
	int button_state[ULAMA_CRSF_NUM_CHANNELS] = {0};
	bool arm_state = false;
	uint16_t seq = 0;
	FILE *crsf_out = NULL;
	int joystick_fd = -1;
	unsigned int sent = 0;
	int exit_code = 1;

	if (parse_args(argc, argv, &cfg) != 0) {
		usage(stderr);
		return 2;
	}
	install_signals();
	memset(&transport, 0, sizeof(transport));
	transport.fd = -1;
	ulama_crsf_channels_init(channels);
	if (cfg.channels_text != NULL && parse_channels_csv(cfg.channels_text, channels) != 0) {
		fprintf(stderr, "failed to parse channels csv\n");
		goto cleanup;
	}
	if (cfg.arm) {
		arm_state = true;
		channels[4] = ULAMA_CRSF_VALUE_MAX;
	}

	if (cfg.crsf_out_path != NULL) {
		crsf_out = fopen(cfg.crsf_out_path, "wb");
		if (crsf_out == NULL) {
			fprintf(stderr, "failed to open %s: %s\n", cfg.crsf_out_path, strerror(errno));
			goto cleanup;
		}
	}

	if (cfg.joystick_path != NULL) {
		joystick_fd = open(cfg.joystick_path, O_RDONLY | O_NONBLOCK);
		if (joystick_fd < 0) {
			fprintf(stderr, "failed to open joystick %s: %s\n", cfg.joystick_path, strerror(errno));
			goto cleanup;
		}
	}

	if (cfg.transport_kind == ULAMA_TRANSPORT_KIND_UDP) {
		if (ulama_transport_tx_init_udp(&transport, cfg.peer) != 0) {
			fprintf(stderr, "failed to init udp %s: %s\n", cfg.peer, strerror(errno));
			goto cleanup;
		}
	} else {
		uint8_t dst_mac[6];
		const uint8_t *dst_mac_ptr = NULL;

		if (cfg.dst_mac_text != NULL) {
			if (!ulama_transport_parse_mac(cfg.dst_mac_text, dst_mac)) {
				fprintf(stderr, "invalid dst mac %s\n", cfg.dst_mac_text);
				goto cleanup;
			}
			dst_mac_ptr = dst_mac;
		}
		if (ulama_transport_tx_init_unow(&transport, cfg.src_node, cfg.iface, dst_mac_ptr) != 0) {
			fprintf(stderr, "failed to init unow %s: %s\n", cfg.iface, strerror(errno));
			goto cleanup;
		}
	}

	fprintf(stderr,
		"ulama_js_tx sending transport=%s src=%u dst=%u mode=%s\n",
		ulama_transport_kind_name(cfg.transport_kind),
		(unsigned int)cfg.src_node,
		(unsigned int)cfg.dst_node,
		cfg.joystick_path != NULL ? cfg.joystick_path : "channels");

	while (!g_stop) {
		uint64_t start_ms = monotonic_ms();
		uint8_t crsf_frame[ULAMA_CRSF_RC_FRAME_SIZE];
		uint8_t ulama_frame[ULAMA_FRAME_HEADER_SIZE + ULAMA_CRSF_RC_FRAME_SIZE];
		size_t crsf_len;
		size_t ulama_len = 0;
		ulama_frame_view_t view;

		if (cfg.joystick_path != NULL) {
			struct pollfd pfd = {
				.fd = joystick_fd,
				.events = POLLIN,
			};
			int timeout_ms = (int)(1000U / cfg.rate_hz);
			int poll_rc = poll(&pfd, 1, timeout_ms);

			if (poll_rc < 0) {
				if (errno == EINTR) {
					continue;
				}
				fprintf(stderr, "joystick poll failed: %s\n", strerror(errno));
				goto cleanup;
			}
			if (poll_rc > 0 && (pfd.revents & POLLIN) != 0) {
				for (;;) {
					struct js_event event;
					ssize_t rc = read(joystick_fd, &event, sizeof(event));
					if (rc == (ssize_t)sizeof(event)) {
						apply_joystick_event(event, channels, button_state, &arm_state);
						continue;
					}
					if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
						break;
					}
					if (rc < 0 && errno == EINTR) {
						continue;
					}
					if (rc < 0) {
						fprintf(stderr, "joystick read failed: %s\n", strerror(errno));
						goto cleanup;
					}
					break;
				}
			}
		}

		crsf_len = ulama_crsf_build_rc_channels_frame(ULAMA_CRSF_ADDRESS_FLIGHT_CONTROLLER, channels, crsf_frame);
		if (crsf_len == 0U) {
			fprintf(stderr, "failed to build crsf frame\n");
			goto cleanup;
		}
		if (crsf_out != NULL && write_blob(crsf_out, crsf_frame, crsf_len) != 0) {
			fprintf(stderr, "failed to write crsf dump\n");
			goto cleanup;
		}
		view.src_node = cfg.src_node;
		view.dst_node = cfg.dst_node;
		view.flags = 0;
		view.traffic_class = ULAMA_CLASS_CTRL;
		view.seq = seq++;
		view.frag_idx = 0;
		view.frag_total = 1;
		view.ttl = cfg.ttl;
		view.payload = crsf_frame;
		view.payload_len = crsf_len;
		if (!ulama_frame_pack(&view, ulama_frame, sizeof(ulama_frame), &ulama_len)) {
			fprintf(stderr, "failed to pack ulama frame\n");
			goto cleanup;
		}
		if (ulama_transport_tx_send(&transport, ulama_frame, ulama_len) < 0) {
			fprintf(stderr, "send failed: %s\n", strerror(errno));
			goto cleanup;
		}
		sent++;
		if (cfg.verbose) {
			print_channel_summary(channels, (uint16_t)(seq - 1U));
		}
		if (cfg.frame_count != 0U && sent >= cfg.frame_count) {
			break;
		}
		if (cfg.joystick_path == NULL && cfg.frame_count > 1U) {
			uint64_t elapsed = monotonic_ms() - start_ms;
			unsigned int period_ms = 1000U / cfg.rate_hz;
			if (elapsed < period_ms) {
				struct timespec delay = {
					.tv_sec = 0,
					.tv_nsec = (long)(period_ms - elapsed) * 1000000L,
				};
				nanosleep(&delay, NULL);
			}
		}
	}

	exit_code = 0;

cleanup:
	if (joystick_fd >= 0) {
		close(joystick_fd);
	}
	if (crsf_out != NULL) {
		fclose(crsf_out);
	}
	ulama_transport_tx_close(&transport);
	return exit_code;
}