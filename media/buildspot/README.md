# buildspot.sh — Fast OEM Sync via UART + Hash Index

**Быстрая доставка изменений на LuckFox Pico Ultra через UART0 + lrzsz с индексом хешей**

---

## 🚀 Быстрый старт за 5 минут

### Шаг 1: Установка на ПК (один раз)
```bash
# Установить lrzsz
sudo apt install -y lrzsz

# Добавить пользователя в группу для работы с UART
sudo usermod -a -G dialout $USER

# Важно: нужно перелогиниться или выполнить `newgrp dialout`
echo "✓ Требуется переподключиться или: newgrp dialout"
```

### Шаг 2: Установка на LuckFox (через SSH)
```bash
ssh root@192.168.1.100 << 'EOF'
  opkg update
  opkg install lrzsz
  mkdir -p /oem
  echo "✓ LuckFox готов к синхронизации"
EOF
```

### Шаг 3: Запуск сервиса на LuckFox
После прошивки с включенным `buildspot` сервис запустится автоматически. Для ручного управления:
```bash
# Запустить сервис в фоне
/etc/init.d/S99buildspot-recv start

# Остановить сервис
/etc/init.d/S99buildspot-recv stop
```

### Шаг 4: Синхронизация с ПК
Из корня проекта `luxfox`:
```bash
# Запустить синхронизацию с параметрами по умолчанию
./build.sh sync

# Или с указанием порта
./build.sh sync --device /dev/ttyUSB1
```
Синхронизация завершена! Можно проверять файлы на устройстве.

---

## 📖 Детальное руководство

### Проблема

- Wi-Fi/Ethernet может быть недоступны
- SSH требует сетевого подключения  
- Нужна надёжная доставка изменений в `/oem` даже без сети

### Решение

Синхронизация через **UART0** (последовательный порт) с использованием **lrzsz** (ZMODEM протокол) + индекс хешей для отправки только изменённых файлов.

### Характеристики

| Параметр | Значение |
|----------|---------|
| **Скорость** | ~10 КБ/с (115200 бод) |
| **1 МБ** | ~100 сек |
| **Надёжность** | Высокая (ZMODEM с проверкой CRC) |
| **Требует сеть** | Нет |
| **Индекс** | SHA256 хеши всех файлов |
| **Дельта** | Только изменённые файлы |

---

## Установка и сборка

### На ПК

```bash
# Установить lrzsz
sudo apt install lrzsz

# Убедиться, что пользователь может читать/писать в /dev/ttyUSB*
sudo usermod -a -G dialout $USER
# Нужно переподключиться или запустить: newgrp dialout
```

### На LuckFox Pico Ultra

**Вариант 1: Через opkg (рекомендуется)**
```bash
ssh root@192.168.1.100
opkg update
opkg install lrzsz
```

**Вариант 2: Через apt (если используется Debian-like система)**
```bash
ssh root@192.168.1.100
apt update
apt install lrzsz
```

### Автозагрузка (Service)

Теперь `buildspot-recv` работает как системный сервис.

#### Скрипт сборки
В корне `media/buildspot` есть `build.sh`, который кладет файлы в те staging-пути, которые реально подхватывает `./build.sh firmware`:
- `output/out/oem/usr/bin/buildspot-recv.sh` → попадет в `/oem/usr/bin/`
- `output/out/media_out/root/etc/init.d/S99buildspot-recv` → попадет в `/etc/init.d/`

Это важно: `./flash.sh` прошивает не каталоги напрямую, а пересобранные `oem.img` и `rootfs.img`. Поэтому `buildspot/build.sh` готовит именно эти staging-деревья, а затем `./flash.sh` вызывает `./build.sh firmware`, который уже пакует их в образы.

Запуск сборки (из корня проекта):
```bash
bash media/buildspot/build.sh
```
Эта команда также вызывается при общей сборке: `./build.sh media`.

#### Управление сервисом на устройстве:
```bash
/etc/init.d/S99buildspot-recv start    # Запустить
/etc/init.d/S99buildspot-recv stop     # Остановить
/etc/init.d/S99buildspot-recv restart  # Перезапустить
```

---

## Использование

### Запуск синхронизации
Команда `sync` интегрирована в основной `build.sh`.

```bash
# Использовать стандартный порт /dev/ttyUSB0
./build.sh sync

# Или указать другой порт/скорость
./build.sh sync --device /dev/ttyUSB1 --speed 115200 --timeout 60
```

### Опции

```
./build.sh sync [options]

--device DEVICE     UART порт (по умолчанию: /dev/ttyUSB0)
--speed SPEED       Скорость в бодах (по умолчанию: 115200)
--timeout SEC       Таймаут на файл в секундах (по умолчанию: 30)
--help              Показать справку
```

---

## Как это работает

### Алгоритм синхронизации

```
┌─────────────────────────────────────────────────────────────┐
│                      PC (buildspot.sh)                      │
├─────────────────────────────────────────────────────────────┤
│ 1. Прочитать локальный индекс:  ./output/out/oem/*.file    │
│    Вычислить SHA256 хеши всех файлов → index_local.txt     │
│                                                              │
│ 2. Получить индекс с устройства (rz):                       │
│    Принять .buildspot.index.gz с /oem/                      │
│    Распаковать → index_device.txt                           │
│                                                              │
│ 3. Найти изменения (delta):                                 │
│    Сравнить SHA256 в index_local vs index_device            │
│    → список файлов для отправки                             │
│                                                              │
│ 4. Отправить файлы (sz):                                    │
│    Для каждого файла из списка:                             │
│    sz -b file > /dev/ttyUSB0                                │
│                                                              │
│ 5. Обновить индекс на устройстве:                           │
│    Отправить index_local.txt.gz (sz)                        │
└─────────────────────────────────────────────────────────────┘
                            ↕ UART ↕ (ZMODEM)
┌─────────────────────────────────────────────────────────────┐
│               LuckFox (buildspot-recv.sh)                    │
├─────────────────────────────────────────────────────────────┤
│ 1. Ждёт входящих файлов (rz):                               │
│    rz -b < /dev/ttyUSB0 > /dev/ttyUSB0                      │
│                                                              │
│ 2. Сохраняет файлы в /oem/:                                 │
│    Восстанавливает структуру директорий                     │
│                                                              │
│ 3. При получении индекса:                                   │
│    Распаковать .buildspot.index.gz                          │
│    Сохранить как .buildspot.index.txt                       │
│                                                              │
│ 4. Повторно ждёт входящих файлов (loop)                     │
└─────────────────────────────────────────────────────────────┘
```

### Формат индекса

Каждая строка текстовой базы данных:
`filepath;sha256hash;mtime;filesize`

**На устройстве хранится:** `/oem/.buildspot.index.gz` (сжатый gzip)

---

## Проблемы и решения

### "lrzsz not installed"
```bash
# На ПК
sudo apt install lrzsz

# На устройстве
opkg install lrzsz
```

### "UART device not found"
```bash
# Проверить доступные порты
ls -la /dev/tty*

# Если используется /dev/ttyACM0 вместо /dev/ttyUSB0:
./build.sh sync --device /dev/ttyACM0
```

### "No read/write permissions on /dev/ttyUSB0"
```bash
# Добавить пользователя в группу dialout
sudo usermod -a -G dialout $USER

# Проверить группы
groups $USER
```

### "Timeout waiting for files"
Увеличить таймаут:
```bash
./build.sh sync --timeout 120
```

---

## Приложение: Руководство по UART подключению

### Физическое подключение

LuckFox Pico Ultra UART0:

```
LuckFox         USB Serial Adapter
──────          ──────────────────
GND ────────→ GND (чёрный)
RX  ────────→ TX  (зелёный)
TX  ────────→ RX  (белый)
```

### Проверка подключения

```bash
# На ПК, проверить связь
screen /dev/ttyUSB0 115200
```

---

## Альтернативы

| Способ | Скорость | Требует сеть | Сложность |
|--------|----------|---------|-----------|
| **UART + lrzsz (buildspot)** | Медленно (~10 КБ/с) | Нет | Низкая |
| SSH (scp/rsync) | Быстро (~1 МБ/с) | Да | Средняя |
| TFTP | Средняя (~500 КБ/с) | Да | Средняя |
| NFS | Очень быстро | Да | Высокая |

---

## Дополнительная информация

- [lrzsz документация](https://ohse.de/uwe/lrzsz.html)
- [ZMODEM протокол](https://en.wikipedia.org/wiki/ZMODEM)
- [SHA256 хеширование](https://en.wikipedia.org/wiki/SHA-2)
