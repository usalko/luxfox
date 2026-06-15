#!/bin/sh
#
# dns-setup-ubuntu.sh — Configure Ubuntu host to use device DNS after DHCP
#
# After RJ45 connection and DHCP assignment, this script configures
# systemd-resolved to use the device's dnsmasq DNS server (192.168.100.1)
#
# Usage:
#   sudo ./dns-setup-ubuntu.sh
#   ssh root@luxfox
#

set -u

if [ "$(id -u)" != "0" ]; then
    echo "ERROR: This script must be run as root (sudo)"
    exit 1
fi

echo "╔════════════════════════════════════════════════════════════════╗"
echo "║         Ubuntu DNS Configuration for Buildspot Device         ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""

# Check if interface is connected
echo "Checking network connection..."
if ip link show | grep -q "state UP"; then
    active_iface=$(ip link show | grep "state UP" | head -1 | awk '{print $2}' | tr -d ':')
    echo "✓ Active interface: $active_iface"
else
    echo "✗ No active network interface found"
    echo "Please connect RJ45 and wait for DHCP"
    exit 1
fi

# Check if device IP is configured
if ip addr | grep -q "192.168.100"; then
    device_ip=$(ip addr | grep "192.168.100" | awk '{print $2}' | cut -d/ -f1 | head -1)
    echo "✓ Device IP assigned: $device_ip"
else
    echo "✗ Device IP not assigned yet"
    echo "Waiting for DHCP... (this can take 10-30 seconds)"
    sleep 15
    if ! ip addr | grep -q "192.168.100"; then
        echo "✗ DHCP failed. Device may not be reachable."
        exit 1
    fi
    device_ip=$(ip addr | grep "192.168.100" | awk '{print $2}' | cut -d/ -f1 | head -1)
    echo "✓ Device IP assigned: $device_ip"
fi

echo ""
echo "Current DNS configuration:"
resolvectl status | head -20
echo ""

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Configuring systemd-resolved to use device DNS (192.168.100.1)..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Check if systemd-networkd is active
if systemctl is-active --quiet systemd-networkd; then
    echo "ℹ systemd-networkd is active"
    
    # Check if DHCP provided DNS
    echo "Checking DHCP-provided DNS..."
    systemctl restart systemd-resolved
    sleep 2
    
    # Try to resolve device name
    if resolvectl query luxfox >/dev/null 2>&1; then
        echo "✓ DNS resolution for 'luxfox' works!"
        resolvectl query luxfox
    else
        echo "⚠ DNS resolution for 'luxfox' not working yet"
        echo "This may be because systemd-networkd hasn't applied DHCP DNS yet"
        echo ""
        echo "Manual configuration..."
        
        # Create systemd-networkd config to use device DNS
        mkdir -p /etc/systemd/network
        cat > /etc/systemd/network/80-dhcp-dns.network << 'NETWORK_EOF'
[Match]
Type=ether

[Network]
DHCP=yes
UseDNS=yes
NETWORK_EOF
        
        systemctl restart systemd-networkd
        sleep 5
        
        echo "✓ Restarted systemd-networkd with DHCP+DNS"
    fi
elif systemctl is-active --quiet NetworkManager; then
    echo "ℹ NetworkManager is active"
    echo "NetworkManager should automatically use DHCP DNS"
    
    # Restart NetworkManager to apply new DHCP settings
    systemctl restart NetworkManager
    sleep 3
    
    echo "✓ Restarted NetworkManager"
else
    echo "⚠ Neither systemd-networkd nor NetworkManager is active"
    echo "Manual DNS configuration required"
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Testing DNS resolution..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

sleep 2

if nslookup luxfox 192.168.100.1 >/dev/null 2>&1; then
    echo "✓ Direct query: nslookup luxfox 192.168.100.1"
    nslookup luxfox 192.168.100.1
else
    echo "✗ Direct query failed"
fi

echo ""

if ping -c 1 luxfox >/dev/null 2>&1; then
    echo "✓ System resolution: ping luxfox works!"
    ping -c 1 luxfox
else
    echo "⚠ System resolution: ping luxfox doesn't work yet"
    echo "Possible causes:"
    echo "  1. systemd-resolved hasn't applied DNS yet (wait 10-30 sec)"
    echo "  2. Firewall blocking"
    echo "  3. DHCP not applied"
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Current DNS status:"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

resolvectl status

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "MANUAL FIX (if DNS still doesn't work):"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "Option 1: Use IP address directly (always works):"
echo "  ssh root@192.168.100.1"
echo ""
echo "Option 2: Edit /etc/resolv.conf manually:"
echo "  sudo bash -c 'echo \"nameserver 192.168.100.1\" > /etc/resolv.conf'"
echo "  ssh root@luxfox"
echo ""
echo "Option 3: Force systemd-resolved to use device DNS:"
echo "  sudo resolvectl dns default 192.168.100.1"
echo "  ssh root@luxfox"
echo ""
echo "Option 4: Add to /etc/hosts manually (permanent):"
echo "  sudo bash -c 'echo \"192.168.100.1 luxfox luckfox\" >> /etc/hosts'"
echo "  ssh root@luxfox"
echo ""
