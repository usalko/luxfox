#include "ulama/serial_uart.h"

#include <asm/ioctls.h>
#include <asm/termbits.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int configure_port(int fd, uint32_t baud)
{
	struct termios2 tio;

	if (ioctl(fd, TCGETS2, &tio) != 0) {
		return -1;
	}

	tio.c_cflag &= ~(CBAUD | CSTOPB | PARENB | PARODD | CRTSCTS | CSIZE);
	tio.c_cflag |= BOTHER | CS8 | CLOCAL | CREAD;
	tio.c_iflag = IGNPAR;
	tio.c_oflag = 0;
	tio.c_lflag = 0;
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;
	tio.c_ispeed = baud;
	tio.c_ospeed = baud;

	if (ioctl(fd, TCSETS2, &tio) != 0) {
		return -1;
	}

	return 0;
}

int ulama_serial_open(ulama_serial_port_t *port, const char *path, uint32_t baud)
{
	int fd;

	if (port == NULL || path == NULL || baud == 0U) {
		errno = EINVAL;
		return -1;
	}
	fd = open(path, O_RDWR | O_NOCTTY | O_SYNC);
	if (fd < 0) {
		return -1;
	}
	if (configure_port(fd, baud) != 0) {
		close(fd);
		return -1;
	}
	port->fd = fd;
	port->baud = baud;
	return 0;
}

ssize_t ulama_serial_write_all(ulama_serial_port_t *port, const uint8_t *data, size_t len)
{
	size_t offset = 0;

	if (port == NULL || port->fd < 0 || data == NULL) {
		errno = EINVAL;
		return -1;
	}
	while (offset < len) {
		ssize_t written = write(port->fd, data + offset, len - offset);
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		offset += (size_t)written;
	}
	return (ssize_t)offset;
}

void ulama_serial_close(ulama_serial_port_t *port)
{
	if (port == NULL || port->fd < 0) {
		return;
	}
	close(port->fd);
	port->fd = -1;
	port->baud = 0;
}