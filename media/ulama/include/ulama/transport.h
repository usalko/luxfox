#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <netinet/in.h>

typedef enum {
	ULAMA_TRANSPORT_KIND_UNSPEC = 0,
	ULAMA_TRANSPORT_KIND_UDP,
	ULAMA_TRANSPORT_KIND_UNOW,
} ulama_transport_kind_t;

typedef struct {
	ulama_transport_kind_t kind;
	uint8_t node_id;
	int fd;
	struct sockaddr_in endpoint;
	bool has_dst_mac;
	uint8_t dst_mac[6];
} ulama_tx_transport_t;

typedef struct {
	ulama_transport_kind_t kind;
	uint8_t node_id;
	int fd;
	struct sockaddr_in endpoint;
} ulama_rx_transport_t;

ulama_transport_kind_t ulama_transport_parse_kind(const char *text);
const char *ulama_transport_kind_name(ulama_transport_kind_t kind);
bool ulama_transport_parse_mac(const char *text, uint8_t mac[6]);

int ulama_transport_tx_init_udp(ulama_tx_transport_t *transport, const char *peer);
int ulama_transport_tx_init_unow(ulama_tx_transport_t *transport, uint8_t node_id, const char *iface, const uint8_t *dst_mac);
ssize_t ulama_transport_tx_send(ulama_tx_transport_t *transport, const uint8_t *data, size_t len);
void ulama_transport_tx_close(ulama_tx_transport_t *transport);

int ulama_transport_rx_init_udp(ulama_rx_transport_t *transport, const char *listen_addr);
int ulama_transport_rx_init_unow(ulama_rx_transport_t *transport, uint8_t node_id, const char *iface);
ssize_t ulama_transport_rx_recv(ulama_rx_transport_t *transport, uint8_t *data, size_t capacity, int timeout_ms, uint8_t src_mac[6], int8_t *rssi);
uint16_t ulama_transport_rx_udp_port(const ulama_rx_transport_t *transport);
void ulama_transport_rx_close(ulama_rx_transport_t *transport);