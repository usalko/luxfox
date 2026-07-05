#include "radiod/ipc.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ================================================================
 * Server side
 * ================================================================ */

int radio_ipc_server_init(radio_ipc_server_t *srv, const char *sock_path)
{
	struct sockaddr_un addr;
	int fd;

	if (srv == NULL || sock_path == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(srv, 0, sizeof(*srv));
	srv->server_fd = -1;

	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

	/* Remove stale socket file */
	unlink(sock_path);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	srv->server_fd = fd;
	return 0;
}

void radio_ipc_server_close(radio_ipc_server_t *srv)
{
	if (srv == NULL)
		return;
	if (srv->server_fd >= 0) {
		close(srv->server_fd);
		srv->server_fd = -1;
	}
	unlink(RADIO_IPC_SOCK_PATH);
}

/* Find or create a client slot by source address. */
static radio_ipc_client_t *find_or_add_client(radio_ipc_server_t *srv,
					      const struct sockaddr_un *addr,
					      socklen_t addr_len)
{
	/* Look for existing client */
	for (int i = 0; i < srv->client_count; i++) {
		radio_ipc_client_t *c = &srv->clients[i];
		if (c->active && c->addr_len == addr_len &&
		    memcmp(&c->addr, addr, addr_len) == 0)
			return c;
	}

	/* Add new client */
	for (int i = 0; i < RADIO_MAX_CLIENTS; i++) {
		radio_ipc_client_t *c = &srv->clients[i];
		if (!c->active) {
			memset(c, 0, sizeof(*c));
			memcpy(&c->addr, addr, addr_len);
			c->addr_len = addr_len;
			c->active = true;
			if (i >= srv->client_count)
				srv->client_count = i + 1;
			return c;
		}
	}
	return NULL;
}

static void remove_client(radio_ipc_server_t *srv,
			  const struct sockaddr_un *addr,
			  socklen_t addr_len)
{
	for (int i = 0; i < srv->client_count; i++) {
		radio_ipc_client_t *c = &srv->clients[i];
		if (c->active && c->addr_len == addr_len &&
		    memcmp(&c->addr, addr, addr_len) == 0) {
			fprintf(stderr, "radiod: client '%s' unregistered\n", c->name);
			c->active = false;
			return;
		}
	}
}

int radio_ipc_drain(radio_ipc_server_t *srv,
		    radio_ipc_tx_cb_t tx_cb, void *user_ctx)
{
	uint8_t buf[RADIO_IPC_MAX_DGRAM];
	struct sockaddr_un src_addr;
	socklen_t src_len;
	ssize_t n;
	int count = 0;

	if (srv == NULL || srv->server_fd < 0)
		return -1;

	for (;;) {
		src_len = sizeof(src_addr);
		n = recvfrom(srv->server_fd, buf, sizeof(buf), MSG_DONTWAIT,
			     (struct sockaddr *)&src_addr, &src_len);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return -1;
		}
		if (n < 1)
			continue;

		uint8_t msg_type = buf[0];

		switch (msg_type) {
		case RADIO_MSG_REGISTER: {
			if ((size_t)n < sizeof(radio_ipc_register_t))
				break;
			radio_ipc_register_t *reg = (radio_ipc_register_t *)buf;
			radio_ipc_client_t *c = find_or_add_client(srv, &src_addr, src_len);
			if (c != NULL) {
				reg->name[sizeof(reg->name) - 1] = '\0';
				strncpy(c->name, reg->name, sizeof(c->name) - 1);
				fprintf(stderr, "radiod: client '%s' registered\n", c->name);
			}
			break;
		}
		case RADIO_MSG_UNREGISTER:
			remove_client(srv, &src_addr, src_len);
			break;

		case RADIO_MSG_TX_REQUEST: {
			if ((size_t)n < sizeof(radio_tx_request_t))
				break;
			radio_tx_request_t *req = (radio_tx_request_t *)buf;
			size_t expected = sizeof(radio_tx_request_t) + req->payload_len;
			if ((size_t)n < expected)
				break;
			/* Auto-register clients that send TX without register */
			find_or_add_client(srv, &src_addr, src_len);
			if (tx_cb != NULL)
				tx_cb(req, expected, user_ctx);
			break;
		}
		default:
			break;
		}
		count++;
	}
	return count;
}

int radio_ipc_broadcast_rx(radio_ipc_server_t *srv,
			   int8_t rssi, const uint8_t src_mac[6],
			   uint8_t tx_phase,
			   const uint8_t *payload, size_t payload_len)
{
	uint8_t buf[sizeof(radio_rx_frame_t) + RADIO_IPC_MAX_DGRAM];
	radio_rx_frame_t *frame = (radio_rx_frame_t *)buf;
	size_t total_len;
	int sent = 0;

	if (srv == NULL || payload == NULL)
		return -1;
	if (payload_len > RADIO_IPC_MAX_DGRAM - sizeof(radio_rx_frame_t))
		return -1;

	frame->msg_type = RADIO_MSG_RX_FRAME;
	frame->rssi = rssi;
	frame->tx_phase = tx_phase;
	if (src_mac != NULL)
		memcpy(frame->src_mac, src_mac, 6);
	else
		memset(frame->src_mac, 0, 6);
	frame->payload_len = (uint16_t)payload_len;
	memcpy(frame->payload, payload, payload_len);
	total_len = sizeof(radio_rx_frame_t) + payload_len;

	for (int i = 0; i < srv->client_count; i++) {
		radio_ipc_client_t *c = &srv->clients[i];
		if (!c->active)
			continue;

		ssize_t n = sendto(srv->server_fd, buf, total_len, MSG_DONTWAIT,
				   (struct sockaddr *)&c->addr, c->addr_len);
		if (n < 0) {
			srv->tx_fail++;
			if (errno == ECONNREFUSED || errno == ENOENT) {
				fprintf(stderr, "radiod: client '%s' disconnected\n", c->name);
				c->active = false;
				srv->tx_disc++;
			}
			continue;
		}
		srv->tx_ok++;
		sent++;
	}
	return sent;
}

/* ================================================================
 * Client side
 * ================================================================ */

int radio_ipc_client_connect(radio_ipc_client_conn_t *conn,
			     const char *server_path,
			     const char *client_name)
{
	struct sockaddr_un client_addr;
	radio_ipc_register_t reg;
	int fd;

	if (conn == NULL || server_path == NULL || client_name == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(conn, 0, sizeof(*conn));
	conn->fd = -1;

	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	/* Bind to abstract socket so we don't need a file on disk */
	memset(&client_addr, 0, sizeof(client_addr));
	client_addr.sun_family = AF_UNIX;
	snprintf(client_addr.sun_path + 1, sizeof(client_addr.sun_path) - 2,
		 "radiod_%s_%d", client_name, getpid());
	client_addr.sun_path[0] = '\0'; /* abstract namespace */
	socklen_t bind_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
				+ 1 + strlen(client_addr.sun_path + 1));

	if (bind(fd, (struct sockaddr *)&client_addr, bind_len) < 0) {
		close(fd);
		return -1;
	}

	/* Set up server address */
	memset(&conn->server_addr, 0, sizeof(conn->server_addr));
	conn->server_addr.sun_family = AF_UNIX;
	strncpy(conn->server_addr.sun_path, server_path,
		sizeof(conn->server_addr.sun_path) - 1);

	conn->fd = fd;

	/* Send registration */
	memset(&reg, 0, sizeof(reg));
	reg.msg_type = RADIO_MSG_REGISTER;
	strncpy(reg.name, client_name, sizeof(reg.name) - 1);
	sendto(fd, &reg, sizeof(reg), 0,
	       (struct sockaddr *)&conn->server_addr, sizeof(conn->server_addr));

	return 0;
}

void radio_ipc_client_close(radio_ipc_client_conn_t *conn)
{
	radio_ipc_register_t unreg;

	if (conn == NULL)
		return;

	if (conn->fd >= 0) {
		memset(&unreg, 0, sizeof(unreg));
		unreg.msg_type = RADIO_MSG_UNREGISTER;
		sendto(conn->fd, &unreg, sizeof(unreg), MSG_DONTWAIT,
		       (struct sockaddr *)&conn->server_addr,
		       sizeof(conn->server_addr));
		close(conn->fd);
		conn->fd = -1;
	}
}

int radio_ipc_client_send_tx(radio_ipc_client_conn_t *conn,
			     uint8_t priority, uint8_t reliability,
			     const uint8_t *payload, size_t payload_len)
{
	uint8_t buf[sizeof(radio_tx_request_t) + RADIO_IPC_MAX_DGRAM];
	radio_tx_request_t *req = (radio_tx_request_t *)buf;
	size_t total_len;

	if (conn == NULL || conn->fd < 0 || payload == NULL)
		return -1;
	if (payload_len > RADIO_IPC_MAX_DGRAM - sizeof(radio_tx_request_t))
		return -1;

	req->msg_type = RADIO_MSG_TX_REQUEST;
	req->priority = priority;
	req->reliability = reliability;
	req->reserved = 0;
	req->payload_len = (uint16_t)payload_len;
	memcpy(req->payload, payload, payload_len);
	total_len = sizeof(radio_tx_request_t) + payload_len;

	ssize_t n = sendto(conn->fd, buf, total_len, 0,
			   (struct sockaddr *)&conn->server_addr,
			   sizeof(conn->server_addr));
	return n < 0 ? -1 : 0;
}

ssize_t radio_ipc_client_recv_rx(radio_ipc_client_conn_t *conn,
				 uint8_t *buf, size_t buf_capacity,
				 int timeout_ms)
{
	struct pollfd pfd;
	int rc;

	if (conn == NULL || conn->fd < 0 || buf == NULL)
		return -1;

	pfd.fd = conn->fd;
	pfd.events = POLLIN;
	rc = poll(&pfd, 1, timeout_ms);
	if (rc < 0)
		return -1;
	if (rc == 0)
		return 0;

	return recv(conn->fd, buf, buf_capacity, MSG_DONTWAIT);
}
