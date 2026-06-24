/*
 * super_ack_test — end-to-end ACK reliability test over two WiFi adapters
 *
 * Forks into sender (TX interface) and receiver (RX interface).
 * Receiver simulates packet loss by randomly dropping N% of frames
 * before auto-ACK processing.  Compares unreliable vs reliable modes.
 *
 * Requires: two WiFi adapters in monitor mode on the same channel.
 * Must run as root (pcap_inject needs it).
 */

#include "unow_internal.h"

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_PAYLOAD_SIZE    100U
#define MAX_PACKETS          100000U
#define RECV_GRACE_MS        2000
#define STARTUP_DELAY_MS     500

typedef struct {
	volatile bool receiver_ready;
	volatile bool sender_done;

	uint32_t tx_total;
	uint32_t tx_ack_ok;
	uint32_t tx_ack_timeout;
	uint32_t tx_retries;
	uint32_t tx_send_fail;
	int64_t  tx_elapsed_ms;

	uint32_t rx_total_frames;
	uint32_t rx_dropped;
	uint32_t rx_processed;
	uint32_t rx_unique;
	uint32_t rx_dedup;
	uint32_t rx_acks_sent;
} super_test_shared_t;

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static int64_t now_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

static int64_t now_ms(void)
{
	return now_us() / 1000LL;
}

static void encode_seq(uint8_t *buf, uint32_t seq)
{
	buf[0] = (uint8_t)(seq >> 24);
	buf[1] = (uint8_t)(seq >> 16);
	buf[2] = (uint8_t)(seq >> 8);
	buf[3] = (uint8_t)(seq);
}

static uint32_t decode_seq(const uint8_t *buf)
{
	return ((uint32_t)buf[0] << 24) |
	       ((uint32_t)buf[1] << 16) |
	       ((uint32_t)buf[2] << 8) |
	       (uint32_t)buf[3];
}

/* ------------------------------------------------------------------ */
/*  Sender                                                             */
/* ------------------------------------------------------------------ */

static int run_sender(const char *iface, const uint8_t *peer_mac,
		      uint32_t count, int rate_pps, bool reliable,
		      super_test_shared_t *shared)
{
	uint8_t payload[TEST_PAYLOAD_SIZE];
	radio_espnow_stats_t stats;
	int64_t start_us;
	int64_t interval_us;
	uint32_t i;

	if (unow_init_iface(1, iface) != ESP_OK) {
		fprintf(stderr, "[TX] unow_init_iface(%s) failed\n", iface);
		return 1;
	}
	radio_espnow_add_peer(peer_mac);

	while (!shared->receiver_ready && !g_stop) {
		usleep(1000);
	}
	usleep(STARTUP_DELAY_MS * 1000);

	memset(payload, 'A', sizeof(payload));
	interval_us = rate_pps > 0 ? 1000000LL / rate_pps : 0;
	start_us = now_us();

	for (i = 0; i < count && !g_stop; i++) {
		esp_err_t err;
		int64_t target_us;
		int64_t cur;

		encode_seq(payload, i);
		if (reliable) {
			err = radio_espnow_send_reliable(peer_mac, payload, TEST_PAYLOAD_SIZE);
		} else {
			err = radio_espnow_send(peer_mac, payload, TEST_PAYLOAD_SIZE);
		}
		if (err != ESP_OK) {
			fprintf(stderr, "[TX] send %u failed: %d\n", i, err);
		}

		if (interval_us > 0) {
			target_us = start_us + (int64_t)(i + 1) * interval_us;
			cur = now_us();
			if (cur < target_us) {
				usleep((useconds_t)(target_us - cur));
			}
		}
	}

	radio_espnow_get_stats(&stats);
	shared->tx_total = i;
	shared->tx_ack_ok = stats.tx_ack_ok;
	shared->tx_ack_timeout = stats.tx_ack_timeout;
	shared->tx_retries = stats.tx_retries;
	shared->tx_send_fail = stats.tx_send_fail;
	shared->tx_elapsed_ms = (now_us() - start_us) / 1000LL;
	shared->sender_done = true;

	unow_deinit();
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Receiver                                                           */
/* ------------------------------------------------------------------ */

static int run_receiver(const char *iface, uint32_t count, int drop_pct,
			super_test_shared_t *shared)
{
	uint8_t received[MAX_PACKETS / 8];
	int64_t grace_deadline = 0;
	bool grace_started = false;

	memset(received, 0, sizeof(received));

	if (unow_init_iface(2, iface) != ESP_OK) {
		fprintf(stderr, "[RX] unow_init_iface(%s) failed\n", iface);
		return 1;
	}

	srand((unsigned int)(time(NULL) ^ getpid()));
	shared->receiver_ready = true;

	while (!g_stop) {
		struct pcap_pkthdr *header;
		const u_char *pkt_data;
		unow_diag_frame_t frame;
		int status;

		if (shared->sender_done && !grace_started) {
			grace_deadline = now_ms() + RECV_GRACE_MS;
			grace_started = true;
		}
		if (grace_started && now_ms() >= grace_deadline) {
			break;
		}
		if (shared->rx_unique >= count) {
			break;
		}

		status = pcap_next_ex(g_unow.pcap, &header, &pkt_data);
		if (status != 1) {
			if (status == 0) {
				usleep(500);
				continue;
			}
			break;
		}
		if (!unow_parse_action_frame(pkt_data, header->caplen, &frame)) {
			continue;
		}
		if (memcmp(frame.src_mac, g_unow.iface.mac, 6) == 0) {
			continue;
		}
		if (frame.subtype == UNOW_VENDOR_SUBTYPE_ACK) {
			continue;
		}

		shared->rx_total_frames++;

		if (drop_pct > 0 && (rand() % 100) < drop_pct) {
			shared->rx_dropped++;
			continue;
		}

		shared->rx_processed++;

		const uint8_t *app_payload;
		size_t app_len;

		if (frame.subtype == UNOW_VENDOR_SUBTYPE_DATA_SEQ && frame.len >= 2U) {
			uint8_t ack_pkt[sizeof(struct unow_radiotap_tx_header) + sizeof(struct unow_dot11_mgmt_header) + sizeof(struct unow_action_vendor_header) + 2U];
			uint8_t seq_bytes[2];
			size_t ack_len;

			seq_bytes[0] = frame.payload[0];
			seq_bytes[1] = frame.payload[1];
			ack_len = unow_build_action_frame_ex(ack_pkt, sizeof(ack_pkt),
				g_unow.iface.mac, frame.src_mac, seq_bytes, 2U,
				UNOW_TX_RATE_1MBPS, UNOW_VENDOR_SUBTYPE_ACK);
			if (ack_len > 0U) {
				pcap_inject(g_unow.pcap, ack_pkt, ack_len);
				shared->rx_acks_sent++;
			}
			app_payload = frame.payload + 2U;
			app_len = frame.len - 2U;
		} else {
			app_payload = frame.payload;
			app_len = frame.len;
		}

		if (app_len >= 4U) {
			uint32_t app_seq = decode_seq(app_payload);

			if (app_seq < MAX_PACKETS) {
				uint32_t byte_idx = app_seq / 8U;
				uint8_t bit_mask = (uint8_t)(1U << (app_seq % 8U));

				if (!(received[byte_idx] & bit_mask)) {
					received[byte_idx] |= bit_mask;
					shared->rx_unique++;
				} else {
					shared->rx_dedup++;
				}
			}
		}
	}

	unow_deinit();
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

static void print_usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s --iface-tx IF --iface-rx IF [OPTIONS]\n"
		"\n"
		"Options:\n"
		"  --iface-tx IF     Sender interface (e.g. wlan0)\n"
		"  --iface-rx IF     Receiver interface (e.g. wlan1)\n"
		"  --count N         Packets to send (default: 1000)\n"
		"  --rate N          Packets/sec (default: 150)\n"
		"  --drop N          Receiver drop percent 0-99 (default: 0)\n"
		"  --reliable        Use radio_espnow_send_reliable\n"
		"  --log-level LVL   UNOW log level (error/warn/info/debug/trace)\n"
		"  -h, --help        Show this help\n",
		prog);
}

int main(int argc, char **argv)
{
	char iface_tx[IFNAMSIZ] = {0};
	char iface_rx[IFNAMSIZ] = {0};
	uint32_t count = 1000;
	int rate_pps = 150;
	int drop_pct = 0;
	bool reliable = false;
	super_test_shared_t *shared;
	unow_iface_info_t tx_info;
	unow_iface_info_t rx_info;
	char error_buf[256];
	char tx_mac_str[18];
	char rx_mac_str[18];
	pid_t pid;
	int child_status = 0;
	double per;
	int i;

	for (i = 1; i < argc; i++) {
		if ((strcmp(argv[i], "--iface-tx") == 0) && i + 1 < argc) {
			snprintf(iface_tx, sizeof(iface_tx), "%s", argv[++i]);
		} else if ((strcmp(argv[i], "--iface-rx") == 0) && i + 1 < argc) {
			snprintf(iface_rx, sizeof(iface_rx), "%s", argv[++i]);
		} else if ((strcmp(argv[i], "--count") == 0) && i + 1 < argc) {
			count = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else if ((strcmp(argv[i], "--rate") == 0) && i + 1 < argc) {
			rate_pps = atoi(argv[++i]);
		} else if ((strcmp(argv[i], "--drop") == 0) && i + 1 < argc) {
			drop_pct = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--reliable") == 0) {
			reliable = true;
		} else if ((strcmp(argv[i], "--log-level") == 0) && i + 1 < argc) {
			setenv("UNOW_LOG_LEVEL", argv[++i], 1);
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			print_usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "unknown argument: %s\n", argv[i]);
			print_usage(argv[0]);
			return 2;
		}
	}

	if (iface_tx[0] == '\0' || iface_rx[0] == '\0') {
		fprintf(stderr, "error: --iface-tx and --iface-rx are required\n");
		print_usage(argv[0]);
		return 2;
	}
	if (count > MAX_PACKETS) {
		fprintf(stderr, "error: --count max is %u\n", MAX_PACKETS);
		return 2;
	}

	if (unow_iface_query(iface_tx, &tx_info, error_buf, sizeof(error_buf)) != 0) {
		fprintf(stderr, "error: cannot query %s: %s\n", iface_tx, error_buf);
		return 1;
	}
	if (unow_iface_query(iface_rx, &rx_info, error_buf, sizeof(error_buf)) != 0) {
		fprintf(stderr, "error: cannot query %s: %s\n", iface_rx, error_buf);
		return 1;
	}

	unow_format_mac(tx_info.mac, tx_mac_str, sizeof(tx_mac_str));
	unow_format_mac(rx_info.mac, rx_mac_str, sizeof(rx_mac_str));

	shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memset(shared, 0, sizeof(*shared));

	fprintf(stdout,
		"[SUPER ACK TEST] tx=%s(%s) rx=%s(%s) count=%u rate=%d drop=%d%% mode=%s\n",
		iface_tx, tx_mac_str, iface_rx, rx_mac_str,
		count, rate_pps, drop_pct,
		reliable ? "reliable" : "unreliable");
	fflush(stdout);

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	pid = fork();
	if (pid < 0) {
		perror("fork");
		munmap(shared, sizeof(*shared));
		return 1;
	}

	if (pid == 0) {
		int rc = run_sender(iface_tx, rx_info.mac, count, rate_pps, reliable, shared);
		_exit(rc);
	}

	run_receiver(iface_rx, count, drop_pct, shared);
	waitpid(pid, &child_status, 0);

	per = count > 0 ? 100.0 * (1.0 - (double)shared->rx_unique / (double)count) : 0.0;

	fprintf(stdout,
		"[RESULT] tx_sent=%u tx_ack_ok=%u tx_ack_timeout=%u tx_retries=%u tx_fail=%u tx_ms=%lld "
		"rx_frames=%u rx_dropped=%u rx_processed=%u rx_unique=%u rx_dedup=%u rx_acks=%u "
		"PER=%.2f%%\n",
		shared->tx_total,
		shared->tx_ack_ok,
		shared->tx_ack_timeout,
		shared->tx_retries,
		shared->tx_send_fail,
		(long long)shared->tx_elapsed_ms,
		shared->rx_total_frames,
		shared->rx_dropped,
		shared->rx_processed,
		shared->rx_unique,
		shared->rx_dedup,
		shared->rx_acks_sent,
		per);

	munmap(shared, sizeof(*shared));
	return WIFEXITED(child_status) ? WEXITSTATUS(child_status) : 1;
}
