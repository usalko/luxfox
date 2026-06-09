#!/bin/bash

set -euo pipefail

IFACE="${1:-${IFACE:-wlx088af1287d57}}"
MON_IFACE="${2:-${MON_IFACE:-mon0}}"

echo "[unow-down] base iface=${IFACE} monitor=${MON_IFACE}"

if iw dev "${MON_IFACE}" info >/dev/null 2>&1; then
	ip link set "${MON_IFACE}" down || true
	if [[ "${MON_IFACE}" != "${IFACE}" ]]; then
		iw dev "${MON_IFACE}" del || true
	fi
fi

if iw dev "${IFACE}" info >/dev/null 2>&1; then
	ip link set "${IFACE}" down || true
	iw dev "${IFACE}" set type managed || true
	ip link set "${IFACE}" up || true
fi

if command -v systemctl >/dev/null 2>&1 && systemctl list-unit-files NetworkManager.service >/dev/null 2>&1; then
	systemctl restart NetworkManager || true
fi

echo "[unow-down] interface restored"