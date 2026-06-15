# buildspot — Fast OEM Sync via SSH + rsync

**Быстрая доставка изменений на LuckFox Pico Ultra через Ethernet (RJ45) + SSH + rsync**

---

## 🚀 Быстрый старт за 10 минут

### Шаг 1: Подготовка ПК

1. **Установить rsync:**
   ```bash
   sudo apt install -y rsync openssh-client
   ```

2. **Сгенерировать SSH-ключи (если их ещё нет):**
   ```bash
   ssh-keygen -t rsa -b 2048 -f ~/.ssh/id_rsa -N ""
   ```

3. **Получить публичный ключ:**
   ```bash
   cat ~/.ssh/id_rsa.pub
   ```

### Шаг 2: Подключение LuckFox

1. **Подключить RJ45 кабель от LuckFox к сетевому адаптеру ПК или маршрутизатору**

2. **Собрать и прошить firmware с buildspot:**
   ```bash
   cd /home/pascale/projects/63411/luxfox
   bash media/buildspot/build.sh
   ./build.sh firmware
   ./flash.sh
   ```

3. **После загрузки устройства дождитесь, пока SSH-сервис запустится**

### Шаг 3: Добавить ваш публичный ключ на устройство

**Вариант 1: через SSH (если есть доступ):**
```bash
ssh root@192.168.1.100 "mkdir -p /oem/.ssh && \
  echo '$(cat ~/.ssh/id_rsa.pub)' >> /oem/.ssh/authorized_keys && \
  chmod 600 /oem/.ssh/authorized_keys"
```

**Вариант 2: скопировать ключ в staging перед сборкой:**
```bash
mkdir -p output/out/oem/.ssh
cat ~/.ssh/id_rsa.pub >> output/out/oem/.ssh/authorized_keys
chmod 600 output/out/oem/.ssh/authorized_keys
```

Затем пересобрать: `./build.sh firmware && ./flash.sh`

### Шаг 4: Синхронизация с ПК

```bash
cd /home/pascale/projects/63411/luxfox
./build.sh sync
```

**Или вручную:**
```bash
./media/buildspot/buildspot-sync-ssh.sh \
  --host 192.168.1.100 \
  --user root \
  --port 22 \
  --source output/out/oem \
  --target /oem
```

Синхронизация завершена! Файлы доставлены через SSH + rsync.

---

## 📖 Детальное руководство

### Проблема

- Wi-Fi/Ethernet может быть недоступны через обычные интерфейсы
- SSH требует предварительного конфигурирования
- Нужна надёжная доставка изменений в `/oem` через Ethernet

### Решение

Синхронизация через **Ethernet (RJ45)** с использованием **SSH + rsync** для надёжной доставки с дельта-синхронизацией.

### Характеристики

| Параметр | Значение |
|----------|---------|
| **Скорость** | ~50+ МБ/с (зависит от Ethernet) |
| **1 МБ** | ~0.02 сек |
| **Надёжность** | Очень высокая (SSH + rsync) |
| **Требует сеть** | Да (Ethernet RJ45) |
| **Индекс** | rsync вычисляет дельту автоматически |
| **Дельта** | Только изменённые файлы |

---

## 🔧 Сборка и установка

### На ПК

```bash
# Установить зависимости
sudo apt install -y rsync openssh-client

# Сгенерировать SSH-ключи
ssh-keygen -t rsa -b 2048 -f ~/.ssh/id_rsa -N ""

# Подготовить LuckFox
cd /home/pascale/projects/63411/luxfox
bash media/buildspot/build.sh
./build.sh firmware
./flash.sh
```

### На LuckFox (автоматически при загрузке)

Сервис `buildspot-ssh-daemon` запускается автоматически:

1. Конфигурирует сетевой интерфейс (eth0 или wlan0)
2. Генерирует SSH host key (если не существует)
3. Запускает Dropbear или OpenSSH сервер
4. Выводит IP адрес и порт в логи

**Проверить статус сервиса:**
```bash
ssh root@192.168.1.100 'ps aux | grep -i ssh'
```

---

## Использование

### Запуск синхронизации

```bash
# Использовать стандартные параметры
./build.sh sync

# Или вручную с параметрами
./media/buildspot/buildspot-sync-ssh.sh \
  --host 192.168.1.100 \
  --user root \
  --port 22 \
  --key ~/.ssh/id_rsa \
  --source output/out/oem \
  --target /oem \
  --timeout 30
```

### Опции

```
./media/buildspot/buildspot-sync-ssh.sh [options]

--host HOST         Device hostname/IP (по умолчанию: 192.168.1.100)
--user USER         SSH user (по умолчанию: root)
--port PORT         SSH port (по умолчанию: 22)
--key FILE          SSH private key (по умолчанию: ~/.ssh/id_rsa)
--source DIR        Local source directory (по умолчанию: output/out/oem)
--target DIR        Remote target directory (по умолчанию: /oem)
--timeout SEC       Connection timeout (по умолчанию: 30)
--help              Show this help
```

---

## Как это работает

### Алгоритм синхронизации

```
┌─────────────────────────────────────────────────────────────┐
│                  PC (buildspot-sync-ssh.sh)                 │
├─────────────────────────────────────────────────────────────┤
│ 1. Проверить SSH-ключ (~/.ssh/id_rsa)                      │
│                                                              │
│ 2. Подключиться к устройству (SSH)                         │
│    ssh root@192.168.1.100                                   │
│                                                              │
│ 3. Запустить rsync:                                        │
│    - Вычислить контрольные суммы файлов                   │
│    - Отправить только изменённые файлы                    │
│    - Сохранить права доступа и timestamp                  │
│                                                              │
│ 4. Проверить результат:                                    │
│    Сравнить количество файлов на обеих сторонах           │
└─────────────────────────────────────────────────────────────┘
                        ↕ SSH ↕ (Encrypted)
┌─────────────────────────────────────────────────────────────┐
│               LuckFox (buildspot-ssh-daemon)                │
├─────────────────────────────────────────────────────────────┤
│ 1. Конфигурировать Ethernet (DHCP или static)             │
│    ifconfig eth0 up                                         │
│    udhcpc -i eth0 (или IP 192.168.1.100/24)               │
│                                                              │
│ 2. Запустить SSH-сервер (Dropbear или OpenSSH)           │
│    dropbear -p 22                                           │
│                                                              │
│ 3. Ожидать входящих SSH-команд (rsync)                    │
│    rsync получает файлы в /oem/                           │
│                                                              │
│ 4. Сохранять файлы с проверкой целостности               │
└─────────────────────────────────────────────────────────────┘
```

### Формат синхронизации

**rsync работает с:**
- Проверкой размера файла
- Контрольными суммами (checksums)
- Сжатием при передаче
- Сохранением прав доступа и mode

**На устройстве хранятся:**
- `/oem/*` — синхронизированные файлы
- `/oem/.ssh/authorized_keys` — публичные ключи доступа
- `/var/log/buildspot-ssh.log` — логи сервиса

---

## Проблемы и решения

### "SSH connection refused"

```bash
# Проверить, запущен ли SSH-сервис на устройстве
ssh root@192.168.1.100 'ps aux | grep ssh'

# Если не запущен, запустить вручную
ssh root@192.168.1.100 '/etc/init.d/S99buildspot-ssh start'

# Проверить логи
ssh root@192.168.1.100 'cat /var/log/buildspot-ssh.log'
```

### "Permission denied (publickey)"

**See detailed troubleshooting guide: [SSH-AUTH-TROUBLESHOOTING.md](../../SSH-AUTH-TROUBLESHOOTING.md)**

Quick fixes:

```bash
# Add your public key to device
ssh root@192.168.1.100 "mkdir -p /oem/.ssh && \
  echo '$(cat ~/.ssh/id_rsa.pub)' >> /oem/.ssh/authorized_keys && \
  chmod 600 /oem/.ssh/authorized_keys && \
  chmod 700 /oem/.ssh"

# Or if device permissions are wrong, apply fix:
ssh -o PreferredAuthentications=password root@192.168.1.100  # password: luckfox
# On device:
/oem/usr/bin/fix-ssh-auth.sh
exit
reboot
```

**Known Issue:** Some firmware versions have `PubkeyAuthentication` disabled in `/etc/ssh/sshd_config` or incorrect `/root` permissions. The new build automatically fixes this on boot.

### "Device not reachable"

```bash
# Проверить сетевое подключение
ping 192.168.1.100

# Проверить Ethernet на устройстве
ssh root@192.168.1.100 'ip link show eth0'
ssh root@192.168.1.100 'ip addr show eth0'

# Если нет IP, попробовать DHCP
ssh root@192.168.1.100 'udhcpc -i eth0 -t 5'

# Или установить статический IP
ssh root@192.168.1.100 'ip addr add 192.168.1.100/24 dev eth0'
```

### "rsync not found on host"

```bash
sudo apt install -y rsync
```

### "Permission denied for /oem"

```bash
# Проверить права на /oem
ssh root@192.168.1.100 'ls -ld /oem'

# Создать/исправить права
ssh root@192.168.1.100 'mkdir -p /oem && chmod 755 /oem'
```

---

## ✅ Итоговая проверка

Если видите это, всё работает:

```bash
# На ПК:
$ ./build.sh sync
[buildspot-sync] Host: root@192.168.1.100:22
[buildspot-sync] Source: output/out/oem (42 files)
[buildspot-sync] SSH connection verified
[buildspot-sync] Starting rsync (delta sync with progress)...
[buildspot-sync] ✓ Verification successful
[buildspot-sync] All operations completed successfully!

# На устройстве:
$ ps aux | grep ssh
root        XXX  0.0  0.1   1234   567 ?  S 10:30 dropbear -r /oem/.ssh/id_rsa -p 22

$ ls -la /oem/
total 128
drwxr-xr-x  3 root root  4096 Jun 15 10:35 .
drwxr-xr-x 13 root root  4096 Jun 15 10:20 ..
-rw-r--r--  1 root root   512 Jun 15 10:35 .ssh
-rw-r--r--  1 root root  1024 Jun 15 10:35 config.json
drwxr-xr-x  2 root root  4096 Jun 15 10:35 bin
drwxr-xr-x  3 root root  4096 Jun 15 10:35 etc
drwxr-xr-x  3 root root  4096 Jun 15 10:35 usr
```

---

## 🚀 Дополнительные команды

```bash
# Просмотреть логи SSH сервиса
ssh root@192.168.1.100 'tail -f /var/log/buildspot-ssh.log'

# Перезапустить сервис
ssh root@192.168.1.100 '/etc/init.d/S99buildspot-ssh restart'

# Остановить сервис
ssh root@192.168.1.100 '/etc/init.d/S99buildspot-ssh stop'

# Проверить размер синхронизированных файлов
ssh root@192.168.1.100 'du -sh /oem/*'

# Сравнить локальные и удалённые файлы
ssh root@192.168.1.100 'find /oem -type f | sort' > /tmp/remote.txt
find output/out/oem -type f | sort > /tmp/local.txt
diff /tmp/local.txt /tmp/remote.txt
```

---

## 📝 Структура staging в build.sh

**Когда вы запускаете `bash media/buildspot/build.sh`:**

```
media/buildspot/
├── buildspot-ssh-daemon.sh
└── S99buildspot-ssh
    ↓
    (staging)
    ↓
output/out/oem/usr/bin/buildspot-ssh-daemon.sh
output/out/media_out/root/etc/init.d/S99buildspot-ssh
output/out/oem/.ssh/ (created, empty)
    ↓
    (packaging during ./build.sh firmware)
    ↓
output/image/rootfs.img  (содержит /etc/init.d/S99buildspot-ssh)
output/image/oem.img     (содержит /usr/bin/buildspot-ssh-daemon.sh, /.ssh/)
    ↓
    (flashing with ./flash.sh)
    ↓
LuckFox device (/etc/init.d/S99buildspot-ssh) → auto-start on boot
```

