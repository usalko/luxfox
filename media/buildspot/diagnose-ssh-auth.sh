#!/bin/sh
#
# diagnose-ssh-auth.sh — Диагностика SSH аутентификации на LuckFox
#
# Проверяет:
# 1. Какой SSH сервер запущен (sshd, dropbear)
# 2. Права доступа на ~/.ssh/ и authorized_keys
# 3. Конфигурацию SSH сервера (MaxAuthTries и т.д.)
# 4. Логи попыток аутентификации
#
# Использование: /oem/usr/bin/diagnose-ssh-auth.sh
#

set -u

echo "╔════════════════════════════════════════════════════════════════╗"
echo "║          SSH Authentication Diagnostics on LuckFox            ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""

# Проверка какой SSH сервер запущен
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "1️⃣ SSH SERVER STATUS"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

if ps aux | grep -q "sshd" | grep -v grep; then
    echo "✓ OpenSSH (sshd) is running"
    ps aux | grep "sshd" | grep -v grep
    SSHD_TYPE="openssh"
elif ps aux | grep -q "dropbear" | grep -v grep; then
    echo "✓ Dropbear SSH is running"
    ps aux | grep "dropbear" | grep -v grep
    SSHD_TYPE="dropbear"
else
    echo "✗ No SSH server found running!"
    SSHD_TYPE="none"
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "2️⃣ AUTHORIZED KEYS STATUS"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Проверить в /root/.ssh/
if [ -f /root/.ssh/authorized_keys ]; then
    echo "✓ /root/.ssh/authorized_keys exists"
    echo "  Permissions:"
    ls -la /root/.ssh/authorized_keys
    echo ""
    echo "  First line (fingerprint):"
    head -1 /root/.ssh/authorized_keys | awk '{print substr($0, 1, 80)}...'
    echo ""
    echo "  Total keys: $(wc -l < /root/.ssh/authorized_keys)"
else
    echo "✗ /root/.ssh/authorized_keys NOT FOUND"
fi

echo ""

# Проверить в /oem/.ssh/
if [ -f /oem/.ssh/authorized_keys ]; then
    echo "✓ /oem/.ssh/authorized_keys exists"
    echo "  Permissions:"
    ls -la /oem/.ssh/authorized_keys
    echo ""
    echo "  First line (fingerprint):"
    head -1 /oem/.ssh/authorized_keys | awk '{print substr($0, 1, 80)}...'
    echo ""
    echo "  Total keys: $(wc -l < /oem/.ssh/authorized_keys)"
else
    echo "✗ /oem/.ssh/authorized_keys NOT FOUND"
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "3️⃣ SSH DIRECTORY PERMISSIONS"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

echo "/root/.ssh directory:"
if [ -d /root/.ssh ]; then
    ls -lad /root/.ssh
else
    echo "✗ /root/.ssh directory NOT FOUND"
fi

echo ""
echo "/oem/.ssh directory:"
if [ -d /oem/.ssh ]; then
    ls -lad /oem/.ssh
else
    echo "✗ /oem/.ssh directory NOT FOUND"
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "4️⃣ SSH SERVER CONFIGURATION"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

if [ "$SSHD_TYPE" = "openssh" ]; then
    echo "OpenSSH configuration (/etc/ssh/sshd_config):"
    echo ""
    if [ -f /etc/ssh/sshd_config ]; then
        echo "Key settings:"
        grep -E "MaxAuthTries|PubkeyAuthentication|PasswordAuthentication|PermitRootLogin" /etc/ssh/sshd_config || echo "(using defaults)"
    else
        echo "✗ /etc/ssh/sshd_config not found (using compile-time defaults)"
    fi
elif [ "$SSHD_TYPE" = "dropbear" ]; then
    echo "Dropbear SSH configuration:"
    echo ""
    if dropbear -h 2>&1 | grep -q "\-w"; then
        echo "Dropbear options:"
        dropbear -h 2>&1 | head -20
    fi
    echo ""
    echo "⚠️  Dropbear typically has MaxAuthTries=3 by default"
    echo "   This causes 'Too many authentication failures' after 3 bad attempts"
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "5️⃣ SSH AUTHENTICATION LOGS"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

if [ -f /var/log/auth.log ]; then
    echo "Recent auth.log (last 15 lines):"
    echo ""
    tail -15 /var/log/auth.log
elif [ -f /var/log/dropbear*.log ]; then
    echo "Dropbear logs:"
    echo ""
    tail -15 /var/log/dropbear*.log 2>/dev/null || echo "(no dropbear logs found)"
else
    echo "✗ No auth logs found"
    echo "  Try: ssh -vvv root@192.168.100.1 to see verbose output"
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "6️⃣ SOLUTIONS"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

if [ "$SSHD_TYPE" = "dropbear" ]; then
    echo "🔧 DROPBEAR FIX: MaxAuthTries limit (default 3)"
    echo ""
    echo "Option 1: Restart Dropbear with higher limit"
    echo "  killall dropbear"
    echo "  dropbear -B"  
    echo ""
    echo "Option 2: On host, use -o PreferredAuthentications=publickey"
    echo "  ssh -o PreferredAuthentications=publickey root@192.168.100.1"
    echo ""
    echo "Option 3: On host, limit SSH keys attempted"
    echo "  ssh -i ~/.ssh/id_rsa -o IdentitiesOnly=yes root@192.168.100.1"
fi

if [ "$SSHD_TYPE" = "openssh" ]; then
    echo "🔧 OPENSSH FIX: MaxAuthTries setting"
    echo ""
    echo "Option 1: Edit /etc/ssh/sshd_config"
    echo "  echo 'MaxAuthTries 10' >> /etc/ssh/sshd_config"
    echo "  /etc/init.d/S50sshd restart"
    echo ""
    echo "Option 2: On host, use -o PreferredAuthentications=publickey"
    echo "  ssh -o PreferredAuthentications=publickey root@192.168.100.1"
fi

echo ""
echo "🎯 IMMEDIATE FIX (on host): Use this command"
echo "   ssh -o PreferredAuthentications=publickey -i ~/.ssh/id_rsa root@192.168.100.1"
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
