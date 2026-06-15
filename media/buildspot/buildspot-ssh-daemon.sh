#!/bin/sh
#
# buildspot-ssh-daemon.sh — SSH service for fast file sync over Ethernet
#
# Auto-starts SSH on boot with pre-configured keys/certs.
# Allows host to connect via SSH and sync files to /oem.
#
# Staging: /oem/usr/bin/buildspot-ssh-daemon.sh
# Init script: /etc/init.d/S99buildspot-ssh
#

set -u

OEM_ROOT="${OEM_ROOT:-/oem}"
SSH_HOST_KEY="$OEM_ROOT/.ssh/id_rsa"
SSH_AUTHORIZED_KEYS="$OEM_ROOT/.ssh/authorized_keys"
SSH_PORT="${SSH_PORT:-22}"
SSH_PID_FILE="/var/run/buildspot-sshd.pid"
HOSTNAME_FILE="/etc/hostname"

log_info() {
    echo "[buildspot-ssh-daemon] $1" | tee -a /var/log/buildspot-ssh.log 2>&1
}

log_error() {
    echo "[buildspot-ssh-daemon] ERROR: $1" | tee -a /var/log/buildspot-ssh.log 2>&1
}

setup_ssh_env() {
    mkdir -p "$OEM_ROOT/.ssh" || {
        log_error "Cannot create $OEM_ROOT/.ssh"
        return 1
    }
    
    chmod 700 "$OEM_ROOT/.ssh"
    
    if [ ! -f "$SSH_HOST_KEY" ]; then
        log_info "Generating SSH host key..."
        ssh-keygen -t rsa -b 2048 -f "$SSH_HOST_KEY" -N "" -C "buildspot@luckfox" >/dev/null 2>&1 || {
            log_error "Failed to generate SSH host key"
            return 1
        }
        log_info "SSH host key generated"
    fi
    
    chmod 600 "$SSH_HOST_KEY"
    
    if [ ! -f "$SSH_AUTHORIZED_KEYS" ]; then
        : > "$SSH_AUTHORIZED_KEYS"
        chmod 600 "$SSH_AUTHORIZED_KEYS"
        log_info "Created empty authorized_keys"
    fi
    
    # CRITICAL: Fix /root permissions for SSH public key auth
    # SSH requires specific permissions or refuses to read authorized_keys
    if [ -d /root ]; then
        chmod 700 /root
        chown root:root /root 2>/dev/null || true
        log_info "Fixed /root permissions: 700, owner root:root"
    fi
    
    # Ensure /root/.ssh directory exists with correct permissions
    mkdir -p /root/.ssh
    chmod 700 /root/.ssh
    chown root:root /root/.ssh 2>/dev/null || true
    log_info "Ensured /root/.ssh permissions: 700, owner root:root"
    
    # Copy authorized_keys to /root/.ssh (SSH looks for keys in /root/.ssh/)
    if [ -f "$SSH_AUTHORIZED_KEYS" ]; then
        cp "$SSH_AUTHORIZED_KEYS" /root/.ssh/authorized_keys 2>/dev/null || true
        chmod 600 /root/.ssh/authorized_keys
        chown root:root /root/.ssh/authorized_keys 2>/dev/null || true
        log_info "Synced authorized_keys to /root/.ssh/ with permissions: 600"
    fi
    
    # Enable PubkeyAuthentication in sshd_config (some builds have it disabled!)
    if [ -f /etc/ssh/sshd_config ]; then
        if grep -q "^#PubkeyAuthentication" /etc/ssh/sshd_config || ! grep -q "^PubkeyAuthentication" /etc/ssh/sshd_config; then
            echo "PubkeyAuthentication yes" >> /etc/ssh/sshd_config
            log_info "Enabled PubkeyAuthentication in /etc/ssh/sshd_config"
        fi
    fi
    
    return 0
}

configure_network() {
    log_info "Configuring eth0 as DHCP gateway (192.168.100.1)..."
    
    if ! ip link show eth0 >/dev/null 2>&1; then
        log_error "eth0 not found"
        available=$(ip link | grep '^[0-9]' | awk '{print $2}' | tr -d ':')
        log_error "Available interfaces: $available"
        return 1
    fi
    
    sleep 1
    
    log_info "Bringing eth0 up..."
    ip link set eth0 up 2>&1 | while read line; do log_info "ip link: $line"; done
    sleep 1
    
    iface_status=$(ip link show eth0 | grep "state")
    log_info "eth0 status: $iface_status"
    
    log_info "Flushing existing IPs from eth0..."
    ip addr flush dev eth0 2>&1 | while read line; do log_info "flush: $line"; done
    sleep 1
    
    log_info "Setting static IP 192.168.100.1/24 on eth0..."
    if ! ip addr add 192.168.100.1/24 dev eth0 2>&1 | while read line; do log_info "addr: $line"; done; then
        log_error "Failed to set eth0 IP address"
        return 1
    fi
    
    sleep 1
    
    ip_set=$(ip addr show eth0 | grep "inet " | awk '{print $2}' | cut -d/ -f1)
    if [ -z "$ip_set" ]; then
        log_error "IP address verification failed - eth0 has no IP"
        log_error "ip addr show eth0 output:"
        ip addr show eth0 2>&1 | while read line; do log_error "  $line"; done
        return 1
    fi
    
    log_info "eth0 configured successfully: $ip_set/24"
    ip addr show eth0 2>&1 | while read line; do log_info "  $line"; done
    return 0
}

start_dhcp_dns_server() {
    log_info "Starting DHCP+DNS gateway with dnsmasq on eth0..."
    
    if ! command -v dnsmasq >/dev/null 2>&1; then
        log_error "DHCP+DNS server (dnsmasq) not found in PATH"
        log_error "This device needs dnsmasq with DHCP support"
        log_error "Add to buildroot config: BR2_PACKAGE_DNSMASQ=y and BR2_PACKAGE_DNSMASQ_DHCP=y"
        return 1
    fi
    
    dnsmasq_path=$(which dnsmasq)
    log_info "Found dnsmasq at: $dnsmasq_path"
    
    dhcp_conf="/etc/dnsmasq.conf"
    
    if [ ! -f "$dhcp_conf" ]; then
        log_error "Config file not found: $dhcp_conf"
        return 1
    fi
    
    log_info "Using dnsmasq config: $dhcp_conf"
    log_info "Config contents (first 15 lines):"
    head -15 "$dhcp_conf" 2>&1 | while read line; do log_info "  $line"; done
    
    # Create required directories
    mkdir -p /var/lib/misc
    mkdir -p /var/log
    
    log_info "Starting dnsmasq (DHCP+DNS server)..."
    if dnsmasq -C "$dhcp_conf" >/var/log/dnsmasq.log 2>&1
    then
        sleep 2
        if ps aux | grep -v grep | grep dnsmasq >/dev/null; then
            log_info "DHCP+DNS server (dnsmasq) is running"
            ps aux | grep dnsmasq | grep -v grep | while read line; do log_info "  $line"; done
            return 0
        else
            log_error "DHCP+DNS server process not found after start"
            log_error "dnsmasq.log contents:"
            cat /var/log/dnsmasq.log 2>&1 | while read line; do log_error "  $line"; done
            return 1
        fi
    else
        log_error "Failed to start DHCP+DNS server"
        log_error "dnsmasq.log contents:"
        cat /var/log/dnsmasq.log 2>&1 | while read line; do log_error "  $line"; done
        return 1
    fi
}

start_ssh_server() {
    log_info "Starting SSH server on port $SSH_PORT..."
    
    if command -v dropbear >/dev/null 2>&1; then
        dropbear -r "$SSH_HOST_KEY" -p "$SSH_PORT" -B || {
            log_error "Failed to start dropbear SSH server"
            return 1
        }
        log_info "Dropbear SSH server started"
    elif command -v sshd >/dev/null 2>&1; then
        mkdir -p /var/run/sshd
        /usr/sbin/sshd -D -p "$SSH_PORT" &
        echo $! > "$SSH_PID_FILE"
        log_info "OpenSSH server started (PID: $!)"
    else
        log_error "No SSH server found (dropbear or openssh required)"
        return 1
    fi
    
    sleep 1
    
    # Fix SSH authentication issues (MaxAuthTries, authorized_keys permissions)
    if [ -x "$OEM_ROOT/usr/bin/fix-ssh-auth.sh" ]; then
        log_info "Applying SSH authentication fixes..."
        "$OEM_ROOT/usr/bin/fix-ssh-auth.sh" 2>&1 | while read line; do log_info "  $line"; done
    fi
    
    return 0
}

print_info() {
    if [ -f "$HOSTNAME_FILE" ]; then
        hostname=$(cat "$HOSTNAME_FILE" 2>/dev/null || echo "luckfox")
    else
        hostname="luckfox"
    fi
    
    device_ip=$(ip addr show eth0 2>/dev/null | grep "inet " | awk '{print $2}' | cut -d/ -f1)
    device_ip="${device_ip:-192.168.100.1}"
    
    log_info "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    log_info "Buildspot SSH + DHCP+DNS Gateway Ready"
    log_info "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    log_info "Device Hostname: $hostname"
    log_info "Device IP (Gateway): $device_ip"
    log_info "DHCP Range: 192.168.100.100 - 192.168.100.254 (12h lease)"
    log_info "DNS Server: 192.168.100.1 (luxfox, luckfox)"
    log_info "SSH Port: $SSH_PORT"
    log_info "Sync Target: $OEM_ROOT"
    log_info "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    log_info "Connection Methods:"
    log_info "1. By hostname (DNS): ssh root@luxfox"
    log_info "2. By IP address:     ssh root@192.168.100.1"
    log_info "3. Sync files:        ./build.sh sync --host luxfox --user root"
    log_info "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
}

main() {
    log_info "Buildspot SSH DHCP Gateway starting..."
    log_info "System time: $(date)"
    tools=$(which ip ifconfig brctl 2>/dev/null | tr '\n' ' ')
    log_info "Available network tools: $tools"
    
    setup_ssh_env || exit 1
    configure_network || exit 1
    
    if ! start_dhcp_dns_server; then
        log_error "DHCP+DNS server failed to start, but continuing with SSH service"
    fi
    
    start_ssh_server || exit 1
    print_info
}

main "$@"
