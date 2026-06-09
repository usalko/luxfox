#!/bin/bash

set -euo pipefail

IFACE="${1:-${IFACE:-wlx088af1287d57}}"
MON_IFACE="${2:-${MON_IFACE:-mon0}}"
CHAN="${3:-${CHAN:-6}}"

echo "[unow-mon] base iface=${IFACE} monitor=${MON_IFACE} channel=${CHAN}"

if command -v airmon-ng >/dev/null 2>&1; then
	airmon-ng check kill || true
fi

ip link set "${IFACE}" down || true

if ! iw dev "${MON_IFACE}" info >/dev/null 2>&1; then
	iw dev "${IFACE}" interface add "${MON_IFACE}" type monitor || true
fi

if iw dev "${MON_IFACE}" info >/dev/null 2>&1; then
	ip link set "${MON_IFACE}" up
	iw dev "${MON_IFACE}" set channel "${CHAN}"
	echo "[unow-mon] ready on ${MON_IFACE}"
	exit 0
fi

iw dev "${IFACE}" set monitor control otherbss
ip link set "${IFACE}" up
iw dev "${IFACE}" set channel "${CHAN}"
	echo "[unow-mon] ready on ${IFACE}"