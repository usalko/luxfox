#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

/* -------------------------------------------------------------------
 * IPC protocol between applications (vcpd, ulamad) and radiod.
 *
 * Transport: Unix domain socket, SOCK_DGRAM.
 * Server path: /var/run/radiod.sock
 *
 * Client sends TX requests, radiod broadcasts RX frames back.
 * Clients filter incoming frames by traffic_class themselves.
 * ------------------------------------------------------------------- */

#define RADIO_IPC_SOCK_PATH  "/var/run/radiod.sock"
#define RADIO_MAX_CLIENTS    8
#define RADIO_IPC_MAX_DGRAM  2400

/* ---- Priorities (match ulama_class_t ordering) ---- */

#define RADIO_PRIO_CTRL      0  /* P0: RC control — always first       */
#define RADIO_PRIO_TELEM     1  /* P1: telemetry                       */
#define RADIO_PRIO_VIDEO     2  /* P2: video stream                    */
#define RADIO_PRIO_BULK      3  /* P3: OSD, firmware, files            */
#define RADIO_PRIO_COUNT     4

/* ---- IPC message types ---- */

typedef enum {
	RADIO_MSG_REGISTER   = 0x01,
	RADIO_MSG_UNREGISTER = 0x02,
	RADIO_MSG_TX_REQUEST = 0x03,
	RADIO_MSG_RX_FRAME   = 0x04,
} radio_ipc_msg_type_t;

/* ---- TX request: client → radiod ---- */

typedef struct {
	uint8_t  msg_type;     /* RADIO_MSG_TX_REQUEST                  */
	uint8_t  priority;     /* 0=CTRL, 1=TELEM, 2=VIDEO, 3=BULK     */
	uint8_t  reliability;  /* 0=unreliable, 1=reliable (ACK+retry)  */
	uint8_t  reserved;
	uint16_t payload_len;
	uint8_t  payload[];    /* packed ULAMA frame                    */
} __attribute__((packed)) radio_tx_request_t;

/* ---- RX frame: radiod → client ---- */

typedef struct {
	uint8_t  msg_type;     /* RADIO_MSG_RX_FRAME                    */
	int8_t   rssi;
	uint8_t  src_mac[6];
	uint8_t  tx_phase;     /* TX-side slot-phase decile (0-9) or 0xFF=N/A;
				 * see UNOW_TX_PHASE_NA / RADIO_TX_PHASE_NA  */
	uint16_t payload_len;
	uint8_t  payload[];    /* packed ULAMA frame                    */
} __attribute__((packed)) radio_rx_frame_t;

/* ---- Registration: client → radiod ---- */

typedef struct {
	uint8_t  msg_type;     /* RADIO_MSG_REGISTER / RADIO_MSG_UNREGISTER */
	char     name[16];     /* client identifier, e.g. "vcpd"            */
} __attribute__((packed)) radio_ipc_register_t;

/* ---- Connected client descriptor ---- */

typedef struct {
	struct sockaddr_un addr;
	socklen_t          addr_len;
	char               name[16];
	bool               active;
} radio_ipc_client_t;

/* ---- IPC server context ---- */

typedef struct {
	int                 server_fd;
	radio_ipc_client_t  clients[RADIO_MAX_CLIENTS];
	int                 client_count;
	/* Diagnostic counters */
	uint32_t            tx_ok;     /* successful sendto to clients  */
	uint32_t            tx_fail;   /* failed sendto (EAGAIN, etc.)  */
	uint32_t            tx_disc;   /* client disconnected (removed) */
} radio_ipc_server_t;

/* ---- Server API ---- */

int  radio_ipc_server_init(radio_ipc_server_t *srv, const char *sock_path);
void radio_ipc_server_close(radio_ipc_server_t *srv);

/*
 * Drain all pending datagrams from the server socket.
 * TX requests are dispatched via the provided callback.
 * Returns number of messages processed, or -1 on error.
 */
typedef void (*radio_ipc_tx_cb_t)(const radio_tx_request_t *req,
				  size_t total_len, void *user_ctx);

int  radio_ipc_drain(radio_ipc_server_t *srv,
		     radio_ipc_tx_cb_t tx_cb, void *user_ctx);

/* Broadcast an RX frame to all registered clients. */
int  radio_ipc_broadcast_rx(radio_ipc_server_t *srv,
			    int8_t rssi, const uint8_t src_mac[6],
			    uint8_t tx_phase,
			    const uint8_t *payload, size_t payload_len);

/* ---- Client API (for vcpd / ulamad) ---- */

typedef struct {
	int fd;
	struct sockaddr_un server_addr;
} radio_ipc_client_conn_t;

int  radio_ipc_client_connect(radio_ipc_client_conn_t *conn,
			      const char *server_path,
			      const char *client_name);
void radio_ipc_client_close(radio_ipc_client_conn_t *conn);

int  radio_ipc_client_send_tx(radio_ipc_client_conn_t *conn,
			      uint8_t priority, uint8_t reliability,
			      const uint8_t *payload, size_t payload_len);

ssize_t radio_ipc_client_recv_rx(radio_ipc_client_conn_t *conn,
				 uint8_t *buf, size_t buf_capacity,
				 int timeout_ms);
