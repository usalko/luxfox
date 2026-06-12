#include "ulama/transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef ULAMA_WITH_UNOW
#define ULAMA_WITH_UNOW 0
#endif

#if ULAMA_WITH_UNOW
#include "esp_err.h"
#include "unow/radio_unow.h"
#include "unow/unow_diag.h"
#endif

static int parse_ipv4_endpoint(const char *text, struct sockaddr_in *out)
{
	char host[64];
	char *colon;
	char *endptr;
	long port;
	size_t host_len;

	if (text == NULL || out == NULL) {
		errno = EINVAL;
		return -1;
	}
	colon = strrchr(text, ':');
	if (colon == NULL) {
		errno = EINVAL;
		return -1;
	}
	host_len = (size_t)(colon - text);
	if (host_len == 0U || host_len >= sizeof(host)) {
		errno = EINVAL;
		return -1;
	}
	memcpy(host, text, host_len);
	host[host_len] = '\0';
	port = strtol(colon + 1, &endptr, 10);
	if (endptr == colon + 1 || *endptr != '\0' || port < 0 || port > 65535) {
		errno = EINVAL;
		return -1;
	}
	memset(out, 0, sizeof(*out));
	out->sin_family = AF_INET;
	out->sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &out->sin_addr) != 1) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

ulama_transport_kind_t ulama_transport_parse_kind(const char *text)
{
	if (text == NULL) {
		return ULAMA_TRANSPORT_KIND_UNSPEC;
	}
	if (strcasecmp(text, "udp") == 0) {
		return ULAMA_TRANSPORT_KIND_UDP;
	}
	if (strcasecmp(text, "unow") == 0) {
		return ULAMA_TRANSPORT_KIND_UNOW;
	}
	return ULAMA_TRANSPORT_KIND_UNSPEC;
}

const char *ulama_transport_kind_name(ulama_transport_kind_t kind)
{
	switch (kind) {
	case ULAMA_TRANSPORT_KIND_UDP:
		return "udp";
	case ULAMA_TRANSPORT_KIND_UNOW:
		return "unow";
	default:
		return "unspec";
	}
}

bool ulama_transport_parse_mac(const char *text, uint8_t mac[6])
{
	unsigned int octets[6];

	if (text == NULL || mac == NULL) {
		return false;
	}
	if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x",
		    &octets[0], &octets[1], &octets[2], &octets[3], &octets[4], &octets[5]) != 6) {
		return false;
	}
	for (size_t index = 0; index < 6U; index++) {
		mac[index] = (uint8_t)octets[index];
	}
	return true;
}

int ulama_transport_tx_init_udp(ulama_tx_transport_t *transport, const char *peer)
{
	int fd;

	if (transport == NULL || peer == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(transport, 0, sizeof(*transport));
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return -1;
	}
	if (parse_ipv4_endpoint(peer, &transport->endpoint) != 0) {
		close(fd);
		return -1;
	}
	transport->kind = ULAMA_TRANSPORT_KIND_UDP;
	transport->fd = fd;
	return 0;
}

int ulama_transport_tx_init_unow(ulama_tx_transport_t *transport, uint8_t node_id, const char *iface, const uint8_t *dst_mac)
{
	#if ULAMA_WITH_UNOW
	esp_err_t err;

	if (transport == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(transport, 0, sizeof(*transport));
	err = unow_init_iface(node_id, iface);
	if (err != ESP_OK) {
		errno = EIO;
		return -1;
	}
	transport->kind = ULAMA_TRANSPORT_KIND_UNOW;
	transport->node_id = node_id;
	transport->fd = -1;
	if (dst_mac != NULL) {
		memcpy(transport->dst_mac, dst_mac, sizeof(transport->dst_mac));
		transport->has_dst_mac = true;
		(void)radio_espnow_add_peer(dst_mac);
	}
	return 0;
	#else
	(void)transport;
	(void)node_id;
	(void)iface;
	(void)dst_mac;
	errno = ENOTSUP;
	return -1;
	#endif
}

ssize_t ulama_transport_tx_send(ulama_tx_transport_t *transport, const uint8_t *data, size_t len)
{
	if (transport == NULL || data == NULL || len == 0U) {
		errno = EINVAL;
		return -1;
	}
	switch (transport->kind) {
	case ULAMA_TRANSPORT_KIND_UDP:
		return sendto(transport->fd,
			data,
			len,
			0,
			(const struct sockaddr *)&transport->endpoint,
			sizeof(transport->endpoint));
	case ULAMA_TRANSPORT_KIND_UNOW: {
		#if ULAMA_WITH_UNOW
		esp_err_t err = radio_espnow_send(transport->has_dst_mac ? transport->dst_mac : NULL, data, len);
		if (err != ESP_OK) {
			errno = EIO;
			return -1;
		}
		return (ssize_t)len;
		#else
		errno = ENOTSUP;
		return -1;
		#endif
	}
	default:
		errno = EINVAL;
		return -1;
	}
}

void ulama_transport_tx_close(ulama_tx_transport_t *transport)
{
	if (transport == NULL) {
		return;
	}
	if (transport->kind == ULAMA_TRANSPORT_KIND_UDP && transport->fd >= 0) {
		close(transport->fd);
	}
	memset(transport, 0, sizeof(*transport));
	transport->fd = -1;
}

int ulama_transport_rx_init_udp(ulama_rx_transport_t *transport, const char *listen_addr)
{
	int fd;
	int yes = 1;

	if (transport == NULL || listen_addr == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(transport, 0, sizeof(*transport));
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return -1;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
		close(fd);
		return -1;
	}
	if (parse_ipv4_endpoint(listen_addr, &transport->endpoint) != 0) {
		close(fd);
		return -1;
	}
	if (bind(fd, (const struct sockaddr *)&transport->endpoint, sizeof(transport->endpoint)) != 0) {
		close(fd);
		return -1;
	}
	{
		struct sockaddr_in actual = {0};
		socklen_t actual_len = sizeof(actual);
		if (getsockname(fd, (struct sockaddr *)&actual, &actual_len) == 0) {
			transport->endpoint = actual;
		}
	}
	transport->kind = ULAMA_TRANSPORT_KIND_UDP;
	transport->fd = fd;
	return 0;
}

int ulama_transport_rx_init_unow(ulama_rx_transport_t *transport, uint8_t node_id, const char *iface)
{
	#if ULAMA_WITH_UNOW
	esp_err_t err;

	if (transport == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(transport, 0, sizeof(*transport));
	err = unow_init_iface(node_id, iface);
	if (err != ESP_OK) {
		errno = EIO;
		return -1;
	}
	transport->kind = ULAMA_TRANSPORT_KIND_UNOW;
	transport->node_id = node_id;
	transport->fd = -1;
	return 0;
	#else
	(void)transport;
	(void)node_id;
	(void)iface;
	errno = ENOTSUP;
	return -1;
	#endif
}

ssize_t ulama_transport_rx_recv(ulama_rx_transport_t *transport, uint8_t *data, size_t capacity, int timeout_ms, uint8_t src_mac[6], int8_t *rssi)
{
	if (transport == NULL || data == NULL || capacity == 0U) {
		errno = EINVAL;
		return -1;
	}
	switch (transport->kind) {
	case ULAMA_TRANSPORT_KIND_UDP: {
		struct pollfd pfd = {
			.fd = transport->fd,
			.events = POLLIN,
		};
		int poll_rc = poll(&pfd, 1, timeout_ms);
		if (poll_rc < 0) {
			return -1;
		}
		if (poll_rc == 0) {
			return 0;
		}
		return recvfrom(transport->fd, data, capacity, 0, NULL, NULL);
	}
	case ULAMA_TRANSPORT_KIND_UNOW: {
		#if ULAMA_WITH_UNOW
		unow_diag_frame_t frame;
		esp_err_t err = unow_diag_recv(&frame, timeout_ms);
		if (err == ESP_ERR_NOT_FOUND) {
			return 0;
		}
		if (err != ESP_OK) {
			errno = EIO;
			return -1;
		}
		if (frame.len > capacity) {
			errno = EMSGSIZE;
			return -1;
		}
		memcpy(data, frame.payload, frame.len);
		if (src_mac != NULL) {
			memcpy(src_mac, frame.src_mac, 6U);
		}
		if (rssi != NULL) {
			*rssi = frame.rssi;
		}
		return (ssize_t)frame.len;
		#else
		(void)src_mac;
		(void)rssi;
		errno = ENOTSUP;
		return -1;
		#endif
	}
	default:
		errno = EINVAL;
		return -1;
	}
}

uint16_t ulama_transport_rx_udp_port(const ulama_rx_transport_t *transport)
{
	if (transport == NULL || transport->kind != ULAMA_TRANSPORT_KIND_UDP) {
		return 0;
	}
	return ntohs(transport->endpoint.sin_port);
}

void ulama_transport_rx_close(ulama_rx_transport_t *transport)
{
	if (transport == NULL) {
		return;
	}
	if (transport->kind == ULAMA_TRANSPORT_KIND_UDP && transport->fd >= 0) {
		close(transport->fd);
	}
	memset(transport, 0, sizeof(*transport));
	transport->fd = -1;
}