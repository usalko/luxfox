#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ulama/transport.h"

static int expect_true(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}
	return 0;
}

int main(void)
{
	ulama_rx_transport_t rx;
	ulama_tx_transport_t tx;
	uint8_t payload[] = {0xAA, 0x55, 0x01, 0x02, 0x03};
	uint8_t received[32] = {0};
	char endpoint[64];
	ssize_t rc;
	int status = 0;

	memset(&rx, 0, sizeof(rx));
	memset(&tx, 0, sizeof(tx));
	rx.fd = -1;
	tx.fd = -1;

	status |= expect_true(ulama_transport_rx_init_udp(&rx, "127.0.0.1:0") == 0, "rx udp init should succeed");
	if (status != 0) {
		goto cleanup;
	}
	snprintf(endpoint, sizeof(endpoint), "127.0.0.1:%u", (unsigned int)ulama_transport_rx_udp_port(&rx));
	status |= expect_true(ulama_transport_tx_init_udp(&tx, endpoint) == 0, "tx udp init should succeed");
	if (status != 0) {
		goto cleanup;
	}
	status |= expect_true(ulama_transport_tx_send(&tx, payload, sizeof(payload)) == (ssize_t)sizeof(payload), "udp send should succeed");
	rc = ulama_transport_rx_recv(&rx, received, sizeof(received), 1000, NULL, NULL);
	status |= expect_true(rc == (ssize_t)sizeof(payload), "udp recv should return full payload");
	status |= expect_true(memcmp(payload, received, sizeof(payload)) == 0, "udp recv payload should match");

cleanup:
	ulama_transport_tx_close(&tx);
	ulama_transport_rx_close(&rx);
	if (status != 0) {
		return 1;
	}
	printf("test_transport_udp: ok\n");
	return 0;
}