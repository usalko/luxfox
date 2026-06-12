#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
	int fd;
	uint32_t baud;
} ulama_serial_port_t;

int ulama_serial_open(ulama_serial_port_t *port, const char *path, uint32_t baud);
ssize_t ulama_serial_write_all(ulama_serial_port_t *port, const uint8_t *data, size_t len);
void ulama_serial_close(ulama_serial_port_t *port);