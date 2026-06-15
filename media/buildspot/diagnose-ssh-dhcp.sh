#!/bin/sh
#
# diagnose-ssh-dhcp.sh — Diagnostic tool for SSH/DHCP+DNS gateway
#
# Run on device to check: network, DHCP, DNS, SSH, logs
#

set -u

echo "╔════════════════════════════════════════════════════════════════╗"
echo "║      Buildspot SSH+DHCP+DNS Gateway Diagnostics              ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""

# === Network Interfaces ===
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "1. NETWORK INTERFACES"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "Available interfaces:"
ip link show 2>&1 || ifconfig -a
echo ""

echo "eth0 IP configuration:"
ip addr show eth0 2>&1 || ifconfig eth0
echo ""

echo "eth0 link status:"
ip link show eth0 | grep -E "state|mtu" || ifconfig eth0 | grep -E "UP|RUNNING"
echo ""

# === DHCP+DNS Server ===
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "2. DHCP+DNS SERVER (dnsmasq)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

echo "DHCP+DNS server running?"
if ps aux | grep -v grep | grep -q dnsmasq; then
    echo "✓ dnsmasq is running:"
    ps aux | grep -v grep | grep dnsmasq
else
    echo "✗ dnsmasq is NOT running"
fi
echo ""

echo "DHCP+DNS config file:"
if [ -f /etc/dnsmasq.conf ]; then
    echo "✓ Found: /etc/dnsmasq.conf"
    head -20 /etc/dnsmasq.conf
else
    echo "✗ Not found: /etc/dnsmasq.conf"
fi
echo ""

echo "DHCP port (67) listening?"
if netstat -tuln 2>/dev/null | grep -q ":67 "; then
    echo "✓ DHCP (port 67) is listening"
    netstat -tuln | grep ":67 "
elif ss -tuln 2>/dev/null | grep -q ":67 "; then
    echo "✓ DHCP (port 67) is listening"
    ss -tuln | grep ":67 "
else
    echo "✗ DHCP (port 67) is NOT listening"
fi
echo ""

echo "DNS port (53) listening?"
if netstat -tuln 2>/dev/null | grep -q ":53 "; then
    echo "✓ DNS (port 53) is listening"
    netstat -tuln | grep ":53 "
elif ss -tuln 2>/dev/null | grep -q ":53 "; then
    echo "✓ DNS (port 53) is listening"
    ss -tuln | grep ":53 "
else
    echo "✗ DNS (port 53) is NOT listening"
fi
echo ""

echo "DHCP+DNS logs (last 15 lines):"
if [ -f /var/log/dnsmasq.log ]; then
    echo "✓ Found: /var/log/dnsmasq.log"
    tail -15 /var/log/dnsmasq.log
else
    echo "✗ Not found: /var/log/dnsmasq.log"
fi
echo ""

# === SSH Server ===
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "3. SSH SERVER"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

echo "SSH server running?"
if ps aux | grep -v grep | grep -qE "sshd|dropbear"; then
    echo "✓ SSH server is running:"
    ps aux | grep -v grep | grep -E "sshd|dropbear"
else
    echo "✗ SSH server is NOT running"
fi
echo ""

echo "SSH port 22 listening?"
if netstat -tuln 2>/dev/null | grep -q ":22 "; then
    echo "✓ Port 22 is listening:"
    netstat -tuln | grep ":22 "
elif ss -tuln 2>/dev/null | grep -q ":22 "; then
    echo "✓ Port 22 is listening:"
    ss -tuln | grep ":22 "
else
    echo "✗ Port 22 is NOT listening"
fi
echo ""

echo "SSH host keys:"
if [ -d /oem/.ssh ]; then
    echo "✓ SSH directory exists: /oem/.ssh"
    ls -la /oem/.ssh/
else
    echo "✗ SSH directory missing: /oem/.ssh"
fi
echo ""

# === Buildspot Logs ===
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "4. BUILDSPOT SSH DAEMON LOGS"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

if [ -f /var/log/buildspot-ssh.log ]; then
    echo "✓ Found: /var/log/buildspot-ssh.log"
    echo "Last 30 lines:"
    tail -30 /var/log/buildspot-ssh.log
else
    echo "✗ Not found: /var/log/buildspot-ssh.log"
fi
echo ""

# === Init Script ===
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "5. INIT SCRIPT STATUS"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

if [ -f /etc/init.d/S99buildspot-ssh ]; then
    echo "✓ Init script found: /etc/init.d/S99buildspot-ssh"
    echo "Status:"
    /etc/init.d/S99buildspot-ssh status 2>/dev/null || echo "(status command not available)"
else
    echo "✗ Init script missing: /etc/init.d/S99buildspot-ssh"
fi
echo ""

# === Summary ===
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "6. SUMMARY & TROUBLESHOOTING"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

if [ -z "$(ip addr show eth0 2>/dev/null | grep '192.168.100.1')" ]; then
    echo "⚠ eth0 does NOT have IP 192.168.100.1"
    echo "  FIX: Run: ip addr add 192.168.100.1/24 dev eth0"
fi

if ! ps aux | grep -v grep | grep -q dnsmasq; then
    echo "⚠ dnsmasq (DHCP+DNS) is NOT running"
    echo "  FIX: Check if dnsmasq is installed: which dnsmasq"
    echo "  FIX: Start manually: dnsmasq -C /etc/dnsmasq.conf"
    echo "  FIX: Check logs: tail /var/log/dnsmasq.log"
fi

if ! ps aux | grep -v grep | grep -qE "sshd|dropbear"; then
    echo "⚠ SSH server is NOT running"
    echo "  FIX: Check logs in /var/log/buildspot-ssh.log"
fi

echo ""
echo "✓ For hostname-based connections:"
echo "  Host machine can now use: ssh root@luxfox (or ssh root@luckfox)"
echo ""
echo "For more debugging, try:"
echo "  - tail -f /var/log/buildspot-ssh.log"
echo "  - tail -f /var/log/dnsmasq.log"
echo "  - /etc/init.d/S99buildspot-ssh restart"
echo "  - dnsmasq --version"
echo "  - ip addr show eth0"
echo "  - ps aux | grep -E 'dnsmasq|sshd|dropbear'"
echo ""
