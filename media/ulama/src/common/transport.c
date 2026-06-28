#include "ulama/transport.h"
#include "ulama/ulama_frame.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
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
	if (strcasecmp(text, "radiod") == 0) {
		return ULAMA_TRANSPORT_KIND_RADIOD;
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
	case ULAMA_TRANSPORT_KIND_RADIOD:
		return "radiod";
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

/* ================================================================
 * radiod IPC transport backend
 *
 * Wire protocol (Unix SOCK_DGRAM):
 *   TX request:  [03] [priority] [reliability] [00] [len_lo] [len_hi] [payload...]
 *   RX frame:    [04] [rssi]     [mac×6]             [len_lo] [len_hi] [payload...]
 *   Register:    [01] [name×16]
 *   Unregister:  [02] ...
 * ================================================================ */

#define RADIOD_DEFAULT_SOCK   "/var/run/radiod.sock"
#define RADIOD_MSG_REGISTER   0x01
#define RADIOD_MSG_UNREGISTER 0x02
#define RADIOD_MSG_TX_REQUEST 0x03
#define RADIOD_MSG_RX_FRAME   0x04

static uint8_t ulama_class_to_radio_prio(uint8_t traffic_class)
{
	switch (traffic_class) {
	case 0: return 0; /* CTRL  → P0 */
	case 1: return 1; /* TELEM → P1 */
	case 3: return 2; /* VIDEO → P2 */
	case 2: return 3; /* BULK  → P3 */
	default: return 3;
	}
}

static uint8_t frame_traffic_class(const uint8_t *data, size_t len)
{
	if (data == NULL || len < ULAMA_FRAME_HEADER_SIZE)
		return 3;
	return data[5];
}

static struct sockaddr_un g_radiod_addr;
static bool g_radiod_addr_init = false;

/* Shared per-process connection to radiod.
 * Both TX and RX transports reuse the same socket so radiod
 * sees one client per process, not two. Without this, the TX-only
 * socket never calls recv() → its buffer fills → radiod sendto
 * fails with EAGAIN on half the broadcasts. */
static int g_radiod_fd = -1;
static int g_radiod_refcount = 0;

static void radiod_ensure_addr(const char *sock_path)
{
	if (g_radiod_addr_init)
		return;
	memset(&g_radiod_addr, 0, sizeof(g_radiod_addr));
	g_radiod_addr.sun_family = AF_UNIX;
	strncpy(g_radiod_addr.sun_path,
		sock_path != NULL ? sock_path : RADIOD_DEFAULT_SOCK,
		sizeof(g_radiod_addr.sun_path) - 1);
	g_radiod_addr_init = true;
}

static int radiod_acquire_fd(const char *client_name)
{
	struct sockaddr_un addr;

	if (g_radiod_fd >= 0) {
		g_radiod_refcount++;
		return g_radiod_fd;
	}

	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	addr.sun_path[0] = '\0';
	snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 2,
		 "radiod_%s_%d", client_name ? client_name : "client", getpid());
	socklen_t bind_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
			      + 1 + strlen(addr.sun_path + 1));
	if (bind(fd, (struct sockaddr *)&addr, bind_len) < 0) {
		close(fd);
		return -1;
	}

	g_radiod_fd = fd;
	g_radiod_refcount = 1;

	/* Register once */
	uint8_t msg[1 + 16];
	memset(msg, 0, sizeof(msg));
	msg[0] = RADIOD_MSG_REGISTER;
	if (client_name != NULL)
		strncpy((char *)msg + 1, client_name, 15);
	sendto(fd, msg, sizeof(msg), MSG_DONTWAIT,
	       (struct sockaddr *)&g_radiod_addr, sizeof(g_radiod_addr));

	return fd;
}

static void radiod_release_fd(void)
{
	if (g_radiod_fd < 0)
		return;
	g_radiod_refcount--;
	if (g_radiod_refcount <= 0) {
		uint8_t msg[1 + 16];
		memset(msg, 0, sizeof(msg));
		msg[0] = RADIOD_MSG_UNREGISTER;
		sendto(g_radiod_fd, msg, sizeof(msg), MSG_DONTWAIT,
		       (struct sockaddr *)&g_radiod_addr, sizeof(g_radiod_addr));
		close(g_radiod_fd);
		g_radiod_fd = -1;
		g_radiod_refcount = 0;
	}
}

static ssize_t radiod_tx_send(int fd, const uint8_t *data, size_t len,
			      uint8_t reliability)
{
	uint8_t buf[6 + 2400];
	uint8_t prio;
	size_t total;

	if (len > 2400) {
		errno = EMSGSIZE;
		return -1;
	}

	prio = ulama_class_to_radio_prio(frame_traffic_class(data, len));

	buf[0] = RADIOD_MSG_TX_REQUEST;
	buf[1] = prio;
	buf[2] = reliability;
	buf[3] = 0;
	buf[4] = (uint8_t)(len & 0xFF);
	buf[5] = (uint8_t)((len >> 8) & 0xFF);
	memcpy(buf + 6, data, len);
	total = 6 + len;

	ssize_t n = sendto(fd, buf, total, MSG_DONTWAIT,
			   (struct sockaddr *)&g_radiod_addr,
			   sizeof(g_radiod_addr));
	if (n < 0)
		return -1;
	return (ssize_t)len;
}

static ssize_t radiod_rx_recv(int fd, uint8_t *data, size_t capacity,
			      int timeout_ms, uint8_t src_mac[6], int8_t *rssi)
{
	uint8_t buf[10 + 2400];
	ssize_t n;

	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int rc = poll(&pfd, 1, timeout_ms);
	if (rc < 0)
		return -1;
	if (rc == 0)
		return 0;

	n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
	if (n < 10)
		return 0;
	if (buf[0] != RADIOD_MSG_RX_FRAME)
		return 0;

	uint16_t payload_len = (uint16_t)buf[8] | ((uint16_t)buf[9] << 8);
	if ((size_t)n < 10U + payload_len)
		return 0;
	if (payload_len > capacity) {
		errno = EMSGSIZE;
		return -1;
	}

	if (rssi != NULL)
		*rssi = (int8_t)buf[1];
	if (src_mac != NULL)
		memcpy(src_mac, buf + 2, 6);
	memcpy(data, buf + 10, payload_len);
	return (ssize_t)payload_len;
}

int ulama_transport_tx_init_radiod(ulama_tx_transport_t *transport,
				   uint8_t node_id,
				   const char *sock_path,
				   const char *client_name)
{
	int fd;

	if (transport == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(transport, 0, sizeof(*transport));
	transport->fd = -1;
	radiod_ensure_addr(sock_path);

	fd = radiod_acquire_fd(client_name);
	if (fd < 0)
		return -1;

	transport->kind = ULAMA_TRANSPORT_KIND_RADIOD;
	transport->node_id = node_id;
	transport->fd = fd;
	return 0;
}

int ulama_transport_rx_init_radiod(ulama_rx_transport_t *transport,
				   uint8_t node_id,
				   const char *sock_path,
				   const char *client_name)
{
	int fd;

	if (transport == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(transport, 0, sizeof(*transport));
	transport->fd = -1;
	radiod_ensure_addr(sock_path);

	fd = radiod_acquire_fd(client_name);
	if (fd < 0)
		return -1;

	transport->kind = ULAMA_TRANSPORT_KIND_RADIOD;
	transport->node_id = node_id;
	transport->fd = fd;
	return 0;
}

/* ================================================================ */

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
	case ULAMA_TRANSPORT_KIND_RADIOD:
		return radiod_tx_send(transport->fd, data, len, 0);
	default:
		errno = EINVAL;
		return -1;
	}
}

ssize_t ulama_transport_tx_send_reliable(ulama_tx_transport_t *transport, const uint8_t *data, size_t len)
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
		esp_err_t err = radio_espnow_send_reliable(transport->has_dst_mac ? transport->dst_mac : NULL, data, len);
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
	case ULAMA_TRANSPORT_KIND_RADIOD:
		return radiod_tx_send(transport->fd, data, len, 1);
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
	if (transport->kind == ULAMA_TRANSPORT_KIND_RADIOD && transport->fd >= 0) {
		radiod_release_fd();
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
	case ULAMA_TRANSPORT_KIND_RADIOD:
		return radiod_rx_recv(transport->fd, data, capacity,
				      timeout_ms, src_mac, rssi);
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
	if (transport->kind == ULAMA_TRANSPORT_KIND_RADIOD && transport->fd >= 0) {
		radiod_release_fd();
	}
	#if ULAMA_WITH_UNOW
	if (transport->kind == ULAMA_TRANSPORT_KIND_UNOW) {
		unow_deinit();
	}
	#endif
	memset(transport, 0, sizeof(*transport));
	transport->fd = -1;
}