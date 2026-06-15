#!/bin/bash

set -e

# buildspot build script for Luckfox Pico Ultra
# Stages SSH sync service into the same directories that `./build.sh firmware`
# packages into rootfs.img and oem.img.
#
# Current approach:
# - SSH service (Dropbear/OpenSSH) for fast Ethernet-based file sync
# - Auto-configures network and runs SSH daemon on boot
# - Uses rsync for efficient delta sync from host
#

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../" && pwd)"
OUTPUT_ROOT="$PROJECT_ROOT/output/out"

# Firmware staging paths used by the main SDK build:
# - output/out/media_out/root -> copied into rootfs during build_firmware
# - output/out/oem            -> packed into oem.img (or copied into /oem)
MEDIA_ROOT_STAGING="$OUTPUT_ROOT/media_out/root"
OEM_STAGING="$OUTPUT_ROOT/oem"

ROOTFS_INIT_DIR="$MEDIA_ROOT_STAGING/etc/init.d"
OEM_BIN_DIR="$OEM_STAGING/usr/bin"
OEM_SSH_DIR="$OEM_STAGING/.ssh"

# Paths to remove from previous UART-based approach
LEGACY_UART_PATHS=(
	"$OUTPUT_ROOT/etc/init.d/S99buildspot-recv"
	"$ROOTFS_INIT_DIR/S99buildspot-recv"
	"$OEM_BIN_DIR/buildspot-recv.sh"
	"$OEM_BIN_DIR/buildspot-agent.sh"
	"$OEM_STAGING/etc/init.d/S99buildspot-recv"
)

install_executable() {
	local src="$1"
	local dst="$2"

	mkdir -p "$(dirname "$dst")"
	cp -f "$src" "$dst"
	chmod +x "$dst"
}

install_file() {
	local src="$1"
	local dst="$2"

	mkdir -p "$(dirname "$dst")"
	cp -f "$src" "$dst"
	chmod 644 "$dst"
}

echo "=== Buildspot: staging SSH sync service for firmware packaging ==="
echo "Rootfs staging: $MEDIA_ROOT_STAGING"
echo "OEM staging:    $OEM_STAGING"

# Stage SSH daemon service
echo "Staging buildspot-ssh-daemon.sh -> $OEM_BIN_DIR/"
install_executable "$SCRIPT_DIR/buildspot-ssh-daemon.sh" "$OEM_BIN_DIR/buildspot-ssh-daemon.sh"

# Stage diagnostic tools
echo "Staging diagnose-ssh-dhcp.sh -> $OEM_BIN_DIR/"
install_executable "$SCRIPT_DIR/diagnose-ssh-dhcp.sh" "$OEM_BIN_DIR/diagnose-ssh-dhcp.sh"

echo "Staging diagnose-ssh-auth.sh -> $OEM_BIN_DIR/"
install_executable "$SCRIPT_DIR/diagnose-ssh-auth.sh" "$OEM_BIN_DIR/diagnose-ssh-auth.sh"

echo "Staging fix-ssh-auth.sh -> $OEM_BIN_DIR/"
install_executable "$SCRIPT_DIR/fix-ssh-auth.sh" "$OEM_BIN_DIR/fix-ssh-auth.sh"

# Stage SSH init script
echo "Staging S99buildspot-ssh -> $ROOTFS_INIT_DIR/"
install_executable "$SCRIPT_DIR/S99buildspot-ssh" "$ROOTFS_INIT_DIR/S99buildspot-ssh"

# Stage DHCP+DNS server configuration (device acts as gateway)
ROOTFS_ETC_DIR="$MEDIA_ROOT_STAGING/etc"
echo "Staging dnsmasq.conf -> $ROOTFS_ETC_DIR/"
install_file "$SCRIPT_DIR/dnsmasq.conf" "$ROOTFS_ETC_DIR/dnsmasq.conf"

# Create .ssh directory for later population by user/deployment
mkdir -p "$OEM_SSH_DIR"
chmod 700 "$OEM_SSH_DIR"

# Cleanup legacy UART-based files
for legacy_path in "${LEGACY_UART_PATHS[@]}"; do
	if [ -f "$legacy_path" ]; then
		echo "Removing legacy UART file: $legacy_path"
		rm -f "$legacy_path"
	fi
done

echo "✓ Buildspot SSH staging complete"
echo "  - SSH daemon:     $OEM_BIN_DIR/buildspot-ssh-daemon.sh"
echo "  - Init script:    $ROOTFS_INIT_DIR/S99buildspot-ssh"
echo "  - DHCP+DNS:       $ROOTFS_ETC_DIR/dnsmasq.conf"
echo "  - SSH home:       $OEM_SSH_DIR"
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  📋 ПОЛНЫЙ ЧЕК-ЛИСТ: SSH + DHCP + DNS на LuckFox"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "СЕТЕВАЯ КОНФИГУРАЦИЯ:"
echo "  🖥️  Device IP (шлюз):      192.168.100.1 (static на eth0)"
echo "  💻 DHCP диапазон:          192.168.100.100 - 192.168.100.254"
echo "  🔌 DNS сервер (dnsmasq):   192.168.100.1:53"
echo "  📡 Имена хоста:            luxfox, luckfox"
echo ""
echo "┌─ ШАГ 1️⃣: Пересобрать прошивку с SSH сервисом"
echo "│"
echo "│  \$ ./flash.sh"
echo "│"
echo "│  ⏱️  Ожидание: 3-5 минут (перестроение rootfs.img и oem.img)"
echo "│"
echo "└─ ✓ Прошивка готова"
echo ""
echo "┌─ ШАГ 2️⃣: Загрузить на device"
echo "│"
echo "│  1. Подключить USB кабель (UART или Maskrom mode)"
echo "│  2. Запустить ./flash.sh (если еще не сделано)"
echo "│  3. Device перезагрузится с новой прошивкой"
echo "│"
echo "└─ ✓ Device готов"
echo ""
echo "┌─ ШАГ 3️⃣: Подключить по Ethernet (RJ45)"
echo "│"
echo "│  1. Подключить RJ45 кабель от LuckFox к Ubuntu хосту"
echo "│     (или через коммутатор, если нет прямого подключения)"
echo "│"
echo "│  2. Подождать 10-30 секунд для DHCP (eth0 должна поднять IP)"
echo "│"
echo "│  3. Проверить что хост получил IP:"
echo "│     \$ ip addr | grep 192.168.100"
echo "│     inet 192.168.100.x/24 brd 192.168.100.255 dev eth0"
echo "│"
echo "└─ ✓ Сеть готова"
echo ""
echo "┌─ ШАГ 4️⃣: Настроить DNS на хосте (выбрать ОДИН вариант)"
echo "│"
echo "│  Вариант A: Быстро - добавить в /etc/hosts (30 сек)"
echo "│    \$ sudo bash -c 'echo \"192.168.100.1 luxfox luckfox\" >> /etc/hosts'"
echo "│    \$ ssh -o PreferredAuthentications=password root@luxfox  # Пароль: luckfox"
echo "│"
echo "│  Вариант B: Правильно - автоконфигурация systemd-resolved"
echo "│    \$ sudo ./dns-setup-ubuntu.sh"
echo "│    (Скрипт автоматически настроит systemd-resolved или NetworkManager)"
echo "│"
echo "│  Вариант C: IP адрес всегда работает"
echo "│    \$ ssh -o PreferredAuthentications=password root@192.168.100.1  # Пароль: luckfox"
echo "│"
echo "└─ ✓ DNS готов"
echo ""
echo "┌─ ШАГ 5️⃣: Первое подключение SSH (с пароль)"
echo "│"
echo "│  \$ ssh -o PreferredAuthentications=password root@192.168.100.1"
echo "│  root@192.168.100.1's password: luckfox"
echo "│"
echo "│  Команды для проверки на device:"
echo "│    # Проверить IP device"
echo "│    \$ ip addr show eth0"
echo "│"
echo "│    # Проверить dnsmasq (DHCP+DNS)"
echo "│    \$ /oem/usr/bin/diagnose-ssh-dhcp.sh"
echo "│"
echo "│    # Проверить SSH ключи"
echo "│    \$ ls -la /oem/.ssh/"
echo "│    \$ cat /oem/.ssh/authorized_keys"
echo "│"
echo "│  \$ exit  # Выход из device"
echo "│"
echo "└─ ✓ SSH работает"
echo ""
echo "┌─ ШАГ 6️⃣: Настроить SSH ключи (без пароля)"
echo "│"
echo "│  1. На хосте создать ключ (если его нет):"
echo "│     \$ ssh-keygen -t rsa -b 4096 -f ~/.ssh/id_rsa -N \\\"\\\""
echo "│"
echo "│  2. Копировать публичный ключ на device:"
echo "│     \$ ssh-copy-id -i ~/.ssh/id_rsa -o PreferredAuthentications=password root@192.168.100.1"
echo "│     root@192.168.100.1's password: luckfox"
echo "│"
echo "│  3. Проверить что успешно скопирован:"
echo "│     \$ ssh root@192.168.100.1 \"cat ~/.ssh/authorized_keys\""
echo "│"
echo "│  4. Теперь подключение БЕЗ пароля:"
echo "│     \$ ssh root@luxfox  # (если DNS настроен)"
echo "│     \$ ssh root@192.168.100.1  # Всегда работает"
echo "│"
echo "└─ ✓ SSH ключи готовы"
echo ""
echo "┌─ ШАГ 7️⃣: Синхронизация файлов (rsync)"
echo "│"
echo "│  Использовать встроенный скрипт:"
echo "│    \$ ./build.sh sync --host luxfox --user root --port 22"
echo "│    \$ ./build.sh sync --host 192.168.100.1 --user root"
echo "│"
echo "│  Или вручную rsync:"
echo "│    \$ rsync -av --progress --delete output/out/oem/ root@luxfox:/oem/"
echo "│"
echo "│  Проверить что синхронизировано:"
echo "│    \$ ssh root@luxfox 'ls -la /oem/'"
echo "│"
echo "└─ ✓ Файлы синхронизированы"
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  ❓ ПРОБЛЕМЫ? Смотри подробное руководство:"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "  📚 Полное руководство (русский): ../SSH-DNS-SETUP.md"
echo "  🔍 На device: /oem/usr/bin/diagnose-ssh-dhcp.sh"
echo ""
echo "  Типичные проблемы:"
echo "    ❌ 'Could not resolve hostname luxfox'"
echo "       → Запусти: sudo ./dns-setup-ubuntu.sh"
echo "       → ИЛИ: sudo bash -c 'echo \\\"192.168.100.1 luxfox\\\" >> /etc/hosts'"
echo ""
echo "    ❌ 'Permission denied (publickey,password)'"
echo "       → Используй флаг: ssh -o PreferredAuthentications=password root@192.168.100.1"
echo "       → Пароль: luckfox"
echo "       → ИЛИ Device может еще загружаться (подожди 30-60 сек)"
echo ""
echo "    ❌ 'Connection refused'"
echo "       → Проверь IP: ip addr | grep 192.168.100"
echo "       → ИЛИ: на device запусти: /etc/init.d/S99buildspot-ssh restart"
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  🚀 Готово! Начни с ШАГ 1️⃣"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
