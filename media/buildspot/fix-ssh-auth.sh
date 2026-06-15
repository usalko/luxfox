#!/bin/sh
#
# fix-ssh-auth.sh — Исправить SSH аутентификацию на LuckFox
#
# Увеличивает MaxAuthTries для поддержки множества SSH ключей на хосте
#
# Использование: /oem/usr/bin/fix-ssh-auth.sh
#

set -e

echo "╔════════════════════════════════════════════════════════════════╗"
echo "║       Fixing SSH Authentication on LuckFox                    ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""

# Determine which SSH server is running
if ps aux | grep -q "sshd" | grep -v grep; then
    SSHD_TYPE="openssh"
    echo "✓ OpenSSH detected"
elif ps aux | grep -q "dropbear" | grep -v grep; then
    SSHD_TYPE="dropbear"
    echo "✓ Dropbear SSH detected"
else
    echo "✗ No SSH server found running"
    exit 1
fi

echo ""

# Ensure authorized_keys exists and is accessible
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "1️⃣ ENSURING SSH DIRECTORY STRUCTURE"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# CRITICAL: Fix /root directory permissions
# SSH will refuse to read authorized_keys if /root has wrong permissions
echo "Fixing /root permissions (CRITICAL for SSH public key auth)..."
if [ -d /root ]; then
    chmod 700 /root
    chown root:root /root 2>/dev/null || true
    echo "  ✓ /root: 700 (root:root)"
fi

# Ensure /root/.ssh exists with correct permissions
echo "Fixing /root/.ssh permissions..."
mkdir -p /root/.ssh
chmod 700 /root/.ssh
chown root:root /root/.ssh 2>/dev/null || true
echo "  ✓ /root/.ssh: 700 (root:root)"

# Copy authorized_keys from /oem/.ssh if needed
if [ -f /oem/.ssh/authorized_keys ] && [ ! -f /root/.ssh/authorized_keys ]; then
    echo "Copying authorized_keys from /oem/.ssh to /root/.ssh..."
    cp /oem/.ssh/authorized_keys /root/.ssh/authorized_keys
    chmod 600 /root/.ssh/authorized_keys
    chown root:root /root/.ssh/authorized_keys 2>/dev/null || true
fi

# Verify authorized_keys exists and has correct permissions
if [ ! -f /root/.ssh/authorized_keys ]; then
    echo "⚠️  Creating empty authorized_keys..."
    touch /root/.ssh/authorized_keys
    chmod 600 /root/.ssh/authorized_keys
    chown root:root /root/.ssh/authorized_keys 2>/dev/null || true
else
    # Fix permissions even if file exists
    chmod 600 /root/.ssh/authorized_keys
    chown root:root /root/.ssh/authorized_keys 2>/dev/null || true
fi

echo "✓ SSH directory structure ready"
echo "  - /root: $(ls -ld /root 2>/dev/null || echo 'ERROR')"
echo "  - /root/.ssh: $(ls -ld /root/.ssh 2>/dev/null || echo 'ERROR')"
echo "  - /root/.ssh/authorized_keys: $(ls -l /root/.ssh/authorized_keys 2>/dev/null || echo 'ERROR')"

echo ""

# Fix SSH server configuration
if [ "$SSHD_TYPE" = "openssh" ]; then
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "2️⃣ FIXING OPENSSH CONFIGURATION"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo ""
    
    SSHD_CONFIG="/etc/ssh/sshd_config"
    
    if [ ! -f "$SSHD_CONFIG" ]; then
        echo "Creating $SSHD_CONFIG..."
        mkdir -p /etc/ssh
        touch "$SSHD_CONFIG"
    fi
    
    # Check if MaxAuthTries already set
    if grep -q "^MaxAuthTries" "$SSHD_CONFIG"; then
        echo "Updating existing MaxAuthTries..."
        sed -i 's/^MaxAuthTries.*/MaxAuthTries 10/' "$SSHD_CONFIG"
    else
        echo "Adding MaxAuthTries to $SSHD_CONFIG..."
        echo "MaxAuthTries 10" >> "$SSHD_CONFIG"
    fi
    
    # CRITICAL: Ensure PubkeyAuthentication is ENABLED
    # Some builds have it disabled by default or commented out
    if grep -q "^PubkeyAuthentication no" "$SSHD_CONFIG"; then
        echo "CRITICAL FIX: PubkeyAuthentication was disabled! Enabling..."
        sed -i 's/^PubkeyAuthentication no/PubkeyAuthentication yes/' "$SSHD_CONFIG"
    elif ! grep -q "^PubkeyAuthentication yes" "$SSHD_CONFIG"; then
        echo "Enabling PubkeyAuthentication (not found or commented)..."
        echo "PubkeyAuthentication yes" >> "$SSHD_CONFIG"
    fi
    
    echo "✓ Updated MaxAuthTries to 10"
    echo "✓ Ensured PubkeyAuthentication is enabled"
    echo ""
    echo "Verifying OpenSSH config..."
    grep -E "MaxAuthTries|PubkeyAuthentication|PasswordAuthentication" "$SSHD_CONFIG" || true
    
    echo ""
    echo "Restarting OpenSSH..."
    if [ -x /etc/init.d/S50sshd ]; then
        /etc/init.d/S50sshd restart
    else
        killall sshd 2>/dev/null || true
        sleep 1
        sshd -D &
        sleep 1
        echo "✓ OpenSSH restarted"
    fi

elif [ "$SSHD_TYPE" = "dropbear" ]; then
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "2️⃣ FIXING DROPBEAR CONFIGURATION"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo ""
    
    # Dropbear doesn't have a MaxAuthTries config file, but we can restart it with different settings
    # The default limit is 3, but restarting should work
    
    echo "Dropbear default MaxAuthTries is typically 3"
    echo "Restarting Dropbear to apply SSH key fix..."
    
    killall dropbear 2>/dev/null || true
    sleep 1
    
    # Start Dropbear in background with standard options
    dropbear -B 2>/dev/null || {
        # If -B fails, try without it
        /etc/init.d/S22dropbear restart 2>/dev/null || {
            # Last resort: start manually
            dropbear &
        }
    }
    
    sleep 1
    
    if ps aux | grep -q dropbear | grep -v grep; then
        echo "✓ Dropbear restarted"
    else
        echo "✗ Failed to restart Dropbear"
        exit 1
    fi
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "3️⃣ VERIFICATION"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

sleep 1

if netstat -tln | grep -q :22 || ss -tln | grep -q :22; then
    echo "✓ SSH is listening on port 22"
else
    echo "⚠️  SSH may not be listening on port 22"
fi

echo ""
echo "✓ SSH authentication fix complete!"
echo ""
echo "On host, try:"
if [ "$SSHD_TYPE" = "dropbear" ]; then
    echo "  ssh -o PreferredAuthentications=publickey -i ~/.ssh/id_rsa root@192.168.100.1"
else
    echo "  ssh root@192.168.100.1"
fi
echo ""
