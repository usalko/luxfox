#include "unow_internal.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static void unow_set_error(char *buffer, size_t buffer_size, const char *fmt, const char *arg)
{
	if (buffer == NULL || buffer_size == 0U) {
		return;
	}
	snprintf(buffer, buffer_size, fmt, arg);
}

static bool unow_copy_iface_name(const char *iface, char *buffer, size_t buffer_size)
{
	size_t len;

	if (iface == NULL || buffer == NULL || buffer_size == 0U) {
		return false;
	}
	len = strlen(iface);
	if (len == 0U || len >= buffer_size) {
		return false;
	}
	memcpy(buffer, iface, len + 1U);
	return true;
}

int unow_iface_query(const char *iface, unow_iface_info_t *out, char *error_buf, size_t error_buf_len)
{
	int sockfd;
	struct ifreq ifr;

	if (iface == NULL || out == NULL) {
		unow_set_error(error_buf, error_buf_len, "%s", "invalid argument");
		return -1;
	}
	memset(out, 0, sizeof(*out));
	if (!unow_copy_iface_name(iface, out->name, sizeof(out->name))) {
		unow_set_error(error_buf, error_buf_len, "%s", "invalid interface name");
		return -1;
	}
	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		unow_set_error(error_buf, error_buf_len, "socket failed: %s", strerror(errno));
		return -1;
	}
	memset(&ifr, 0, sizeof(ifr));
	memcpy(ifr.ifr_name, out->name, strlen(out->name));
	if (ioctl(sockfd, SIOCGIFINDEX, &ifr) < 0) {
		unow_set_error(error_buf, error_buf_len, "SIOCGIFINDEX failed: %s", strerror(errno));
		close(sockfd);
		return -1;
	}
	out->ifindex = ifr.ifr_ifindex;
	if (ioctl(sockfd, SIOCGIFFLAGS, &ifr) < 0) {
		unow_set_error(error_buf, error_buf_len, "SIOCGIFFLAGS failed: %s", strerror(errno));
		close(sockfd);
		return -1;
	}
	out->flags = (unsigned int)ifr.ifr_flags;
	if (ioctl(sockfd, SIOCGIFHWADDR, &ifr) < 0) {
		unow_set_error(error_buf, error_buf_len, "SIOCGIFHWADDR failed: %s", strerror(errno));
		close(sockfd);
		return -1;
	}
	memcpy(out->mac, ifr.ifr_hwaddr.sa_data, sizeof(out->mac));
	close(sockfd);
	return 0;
}

int unow_iface_open_pcap(const char *iface, pcap_t **out_handle, int *out_datalink, char *error_buf, size_t error_buf_len)
{
	char pcap_error[PCAP_ERRBUF_SIZE] = {0};
	pcap_t *handle;
	int status;

	if (iface == NULL || out_handle == NULL) {
		unow_set_error(error_buf, error_buf_len, "%s", "invalid argument");
		return -1;
	}
	handle = pcap_create(iface, pcap_error);
	if (handle == NULL) {
		unow_set_error(error_buf, error_buf_len, "pcap_create failed: %s", pcap_error);
		return -1;
	}
	status = pcap_set_snaplen(handle, 2048);
	if (status != 0) {
		unow_set_error(error_buf, error_buf_len, "pcap_set_snaplen failed: %s", pcap_geterr(handle));
		pcap_close(handle);
		return -1;
	}
	status = pcap_set_promisc(handle, 1);
	if (status != 0) {
		unow_set_error(error_buf, error_buf_len, "pcap_set_promisc failed: %s", pcap_geterr(handle));
		pcap_close(handle);
		return -1;
	}
	status = pcap_set_timeout(handle, 100);
	if (status != 0) {
		unow_set_error(error_buf, error_buf_len, "pcap_set_timeout failed: %s", pcap_geterr(handle));
		pcap_close(handle);
		return -1;
	}
	status = pcap_set_immediate_mode(handle, 1);
	if (status != 0) {
		UNOW_LOGW("pcap_set_immediate_mode failed on %s: %s", iface, pcap_geterr(handle));
	}
	status = pcap_activate(handle);
	if (status < 0) {
		unow_set_error(error_buf, error_buf_len, "pcap_activate failed: %s", pcap_geterr(handle));
		pcap_close(handle);
		return -1;
	}
	if (status > 0) {
		UNOW_LOGW("pcap_activate warning on %s: %s", iface, pcap_statustostr(status));
	}
	*out_handle = handle;
	if (out_datalink != NULL) {
		*out_datalink = pcap_datalink(handle);
	}
	return 0;
}

void unow_iface_close_pcap(pcap_t **handle)
{
	if (handle == NULL || *handle == NULL) {
		return;
	}
	pcap_close(*handle);
	*handle = NULL;
}

bool unow_parse_mac(const char *text, uint8_t mac[6])
{
	unsigned int octets[6];

	if (text == NULL || mac == NULL) {
		return false;
	}
	if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x",
		   &octets[0],
		   &octets[1],
		   &octets[2],
		   &octets[3],
		   &octets[4],
		   &octets[5]) != 6) {
		return false;
	}
	for (size_t i = 0; i < 6U; ++i) {
		mac[i] = (uint8_t)octets[i];
	}
	return true;
}

void unow_format_mac(const uint8_t mac[6], char *buffer, size_t buffer_size)
{
	if (buffer == NULL || buffer_size == 0U) {
		return;
	}
	if (mac == NULL) {
		snprintf(buffer, buffer_size, "<null>");
		return;
	}
	snprintf(buffer, buffer_size, "%02x:%02x:%02x:%02x:%02x:%02x",
		 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool unow_mac_is_broadcast(const uint8_t mac[6])
{
	static const uint8_t broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

	if (mac == NULL) {
		return false;
	}
	return memcmp(mac, broadcast_mac, sizeof(broadcast_mac)) == 0;
}