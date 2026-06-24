# Интеграция прикладных протоколов VCP-LTS и CRSF поверх ULAMA

> Аналитический документ и план действий по финальной интеграции протоколов
> прикладного уровня с транспортной сетью ULAMA и операторской станцией
> Cascade (НСУ Каскад).

Дата: 2026-06-17

---

## 0. Исполнительное резюме

### Текущая архитектура (стек протоколов)

```
           cascade-core (НСУ)
                │
                │  cascade-frame (6B hdr, ≤1400B payload, u16 addr)
                │  UDP 127.0.0.1:5600/5601
                ▼
        ┌──── ulamad (gateway) ────┐
        │   cascade-frame ↔ ULAMA  │     <── ОТСУТСТВУЕТ
        │   фрагментация >220B    │
        │   u16 → u8 адресация    │
        └──────────┬───────────────┘
                   │  ULAMA L1 frame (14B hdr, ≤220B, u8 addr)
                   │  + L2/L3 (link_manager, mesh)     <── НЕ ПОРТИРОВАНО
                   ▼
                  UNOW
                   │  802.11 vendor action frame
                   ▼
              MW300UH ◄──── радиоканал ────► MW300UH
                                                │
                                         ulamad (drone)
                                                │
                                    ┌───────────┼───────────┐
                                    │           │           │
                               CRSF→UART   Видео←ISP   MSP←FC
                               (ЕСТЬ)      (НЕТ)       (НЕТ)
```

### Три ключевых ответа

**1. Нужно ли выносить VCP-LTS и CRSF выше ULAMA?**

**ДА.** Оба протокола — прикладной уровень. ULAMA — транспорт. Текущее
смешивание CRSF внутри `media/ulama/` — компромисс вертикального MVP. Для
финальной интеграции чёткое разделение обязательно:

- **CRSF** уже корректно передаётся как payload внутри ULAMA CTRL-кадров.
  Кодек (`crsf.c/h`) может оставаться библиотекой в `ulama/`, но прикладная
  логика (CRSF→UART bridging, keepalive 150 Hz) должна быть отдельным агентом.
- **VCP-LTS** — протокол видеотранспорта с собственной логикой фрагментации,
  нумерацией пакетов, NACK-ретрансмиссией и reorder-буфером. Он работает
  ПОВЕРХ ULAMA VIDEO-класса и НЕ ДОЛЖЕН быть частью сетевого стека.

**2. Какие новые артефакты нужны в luxfox?**

| # | Артефакт | Назначение | Язык |
|---|----------|-----------|------|
| A | `media/ulama-gw/` | Шлюз оператора: cascade-frame ↔ ULAMA frame | C |
| B | `media/vcpd/` | Видеодемон LuckFox: камера → LTS → ULAMA VIDEO | C |
| C | `media/ulama/` расширение | MSP-телеметрия: FC UART → ULAMA TELEMETRY | C |

**3. План действий** — ниже, в разделах 1–6.

---

## 1. Архитектурный анализ: где что живёт

### 1.1 Два формата кадров — ключевое различие

В системе сосуществуют **два** формата кадров:

| Свойство | `ulama_frame` | `cascade-frame` |
|----------|---------------|-----------------|
| Заголовок | 14 байт | 6 байт |
| Max payload | 220 байт | 1400 байт |
| Адресация | u8 (1–253) | u16 (1–65535) |
| CRC | CRC16-CCITT | нет (localhost UDP) |
| Где живёт | радиоканал (UNOW) | localhost UDP |
| Кто создаёт | `ulamad` | `cascade-core` |

Между ними нужен **шлюз** (gateway), который:
- Конвертирует адресацию u16 → u8 (таблица маппинга или простое усечение)
- Фрагментирует payload > 220 байт (cascade-frame до 1400B → ULAMA fragments)
- Маппит traffic class (cascade 0–4 → ULAMA 0–3)
- Скрывает от cascade-core детали mesh/link/ARQ

### 1.2 Протокольный стек (целевой)

```
┌─────────────────────────────────────────────────────────────────┐
│                    ПРИКЛАДНОЙ УРОВЕНЬ                           │
│                                                                 │
│  CRSF                    VCP-LTS                   MSP          │
│  (RC каналы              (видеопоток               (телеметрия   │
│   джойстик→FC)            камера→оператор)          FC→оператор) │
│  payload в               payload в                 payload в     │
│  CLASS_CTRL               CLASS_VIDEO               CLASS_TELEM  │
└───────┬──────────────────────┬───────────────────────┬──────────┘
        │                      │                       │
┌───────┴──────────────────────┴───────────────────────┴──────────┐
│              cascade-frame (host wrapper)                        │
│              6B hdr + payload ≤ 1400B                            │
│              UDP localhost, cascade-core ↔ ulama-gw              │
└───────────────────────────┬─────────────────────────────────────┘
                            │
┌───────────────────────────┴─────────────────────────────────────┐
│              ulama-gw (шлюз)                                     │
│              cascade-frame ↔ ulama_frame                         │
│              фрагментация, адресация, маршрутизация              │
└───────────────────────────┬─────────────────────────────────────┘
                            │
┌───────────────────────────┴─────────────────────────────────────┐
│              ULAMA L1/L2/L3                                      │
│              ulama_frame (14B hdr + ≤220B)                       │
│              link_manager, mesh, ARQ, fragmentation              │
└───────────────────────────┬─────────────────────────────────────┘
                            │
┌───────────────────────────┴─────────────────────────────────────┐
│              UNOW (L2 radio)                                     │
│              802.11 vendor action frames, ≤250B                  │
└─────────────────────────────────────────────────────────────────┘
```

### 1.3 Анализ CRSF

**Что есть сейчас:**

- `media/ulama/src/common/crsf.c` — кодек CRSF RC_CHANNELS_PACKED (pack/unpack
  16 каналов по 11 бит, CRC8-DVB-S2). Чистый C, без зависимостей.
- `media/ulama/tools/ulama_js_tx.c` — джойстик → CRSF → ULAMA L1 → UDP/UNOW.
- `media/ulama/tools/ulamad.c` — приём ULAMA L1 → валидация CRSF → запись в
  UART/файл/stdout. Keepalive 150 Hz. Работает.
- `cascade/core/internal/api/control_sender.go` — cascade-core формирует CRSF
  payload (0x16 + 22B packed channels) и шлёт как cascade-frame CLASS_CONTROL.

**Проблема:** `cascade-core` пакует CRSF в своём формате (23 байта: type + 22B
data), а `ulamad` на дроне ожидает полный CRSF serial frame (26 байт:
`0xC8 + len + type + 22B + CRC8`). Между ними НУЖЕН шлюз, который:
1. Принимает cascade-frame CTRL с CRSF payload от cascade-core
2. Конвертирует в ULAMA frame с traffic_class=CTRL
3. Отправляет через UNOW

На дроне `ulamad` уже принимает ULAMA frame, но ожидает внутри полный CRSF
serial frame с заголовком `0xC8`. Нужно определиться с «точкой сборки» CRSF
serial frame:
- **Вариант A**: cascade-core собирает полный CRSF frame (0xC8/len/type/data/CRC)
  → шлюз просто транслирует
- **Вариант B**: шлюз (ulama-gw) добавляет CRSF serial обёртку
- **Вариант C**: drone-side ulamad принимает «голый» CRSF payload и сам добавляет
  serial обёртку перед записью в UART

**Рекомендация:** Вариант A — cascade-core собирает полный CRSF frame, шлюз
прозрачен. Это позволяет тестировать CRSF encoding на host без железа.

### 1.4 Анализ VCP-LTS

**Что есть сейчас:**

- `ULAMA/host/vcpd/` — Go-имплементация UVCP/1 + LTS для ESP32-системы
  - LTS: 4-байтовый заголовок (`stream_id:1 + pkt_seq:2 + flags:1`)
  - Reorder-буфер, NACK-ретрансмиссия, ring-buffer (256 пакетов)
  - UVCP/1: текстовый control protocol (READY/START/STOP/PING/PONG)
- `cascade/tests/ulama-simulator/cmd/drone-sim/video.go` — ffmpeg → MPEG-TS →
  cascade-frame VIDEO (без LTS, raw TS chunks ≤1100B)
- `cascade/core/internal/api/video_ws.go` — cascade-core пробрасывает VIDEO
  payload по WebSocket в браузер

**Проблема:** на реальном радиоканале потери и переупорядочивание неизбежны.
Без LTS видеопоток разваливается. Нужна C-имплементация LTS для LuckFox
(Go-версия не годится для embedded ARM uclibc).

**Целевой видеопоток на дроне:**
```
Камера ISP → H.264 HW encoder → MPEG-TS → LTS encoder → ULAMA VIDEO frames → UNOW
```

**Целевой приём на операторе:**
```
UNOW → ULAMA VIDEO → cascade-frame → cascade-core → WebSocket → браузер (mpegts.js)
```

LTS-декодирование (reorder, dedup, NACK) можно делать в трёх местах:
1. **В ulama-gw** — до передачи cascade-core
2. **В cascade-core** — перед отправкой в WebSocket
3. **В браузере** — JavaScript LTS decoder перед mpegts.js

**Рекомендация:** Вариант 1 — LTS-декодирование в ulama-gw на операторской
стороне. Cascade-core получает уже чистый MPEG-TS поток. Это:
- Не усложняет cascade-core
- Не требует JavaScript-декодера
- Позволяет ulama-gw отправлять NACK обратно по радио

### 1.5 Анализ MSP-телеметрии

**Что есть сейчас:**
- Симулятор: текстовый формат `sim:key=value;...`
- cascade-core: парсит только `sim:` формат
- Реальный FC: говорит MSP v1/v2 по UART

**Нужен агент на дроне:** читает MSP с FC по UART, упаковывает в ULAMA
TELEMETRY frames. Это прикладной агент, не часть сетевого стека.

---

## 2. Необходимые артефакты (новые проекты)

### 2.1 `media/ulama-gw/` — Шлюз cascade↔ULAMA (ПРИОРИТЕТ 1)

**Назначение:** Работает на стороне оператора (ground station). Принимает
cascade-frame от cascade-core по UDP, конвертирует в ULAMA frame и отправляет
по UNOW. В обратном направлении принимает ULAMA frame от дронов, конвертирует
в cascade-frame и отправляет cascade-core.

**Почему отдельный проект, а не часть `media/ulama/`:**
- `media/ulama/` — это порт сетевого стека (L1/L2/L3) + вертикальный MVP
  (CRSF→UART). Шлюз — другая роль.
- Шлюз зависит от `libulama.a` (L1 frame pack/unpack) и `libunow`
  (радио-транспорт), но добавляет свою логику (cascade-frame парсинг,
  фрагментация >220B, LTS-декодирование).
- Разделение ролей = независимые обновления.

**Ключевая функциональность:**

```
cascade-core:5601 ──(cascade-frame)──► ulama-gw ──(ulama_frame)──► UNOW air
cascade-core:5600 ◄──(cascade-frame)── ulama-gw ◄──(ulama_frame)── UNOW air
```

| Функция | Описание |
|---------|----------|
| Downlink (оператор→дрон) | Приём cascade-frame CTRL/MGMT → ULAMA frame → UNOW TX |
| Uplink (дрон→оператор) | UNOW RX → ULAMA frame → cascade-frame → cascade-core |
| Фрагментация downlink | cascade payload > 220B → ULAMA fragments |
| Дефрагментация uplink | ULAMA fragments → сборка → cascade payload |
| Адресация | cascade u16 → ULAMA u8 (таблица / truncate) |
| LTS-декодирование (video uplink) | Reorder-буфер, dedup, NACK → чистый MPEG-TS |
| LTS-кодирование (при необходимости) | Для OSD_IMAGE downlink (фаза 2) |
| Статистика | Per-class counters, link quality, throughput |

**Зависимости:**
- `media/ulama/out/lib/libulama.a` — ulama_frame pack/unpack, CRC
- `media/ulama/out/include/ulama/` — заголовки
- `media/unow/` — радио-транспорт (или через `ulama/src/common/transport.c`)
- `libpcap-dev` — для UNOW

**Целевая структура:**
```
media/ulama-gw/
  Makefile
  include/ulama_gw/
    gateway.h           # cascade-frame ↔ ULAMA conversion API
    lts_decoder.h       # LTS reorder/dedup/NACK для video uplink
    fragmentation.h     # cascade payload → ULAMA fragments
  src/
    gateway.c           # основной event loop
    cascade_frame.c     # парсер cascade-frame формата
    lts_decoder.c       # порт из ULAMA/host/vcpd/internal/lts/
    fragmentation.c     # fragmentation/reassembly
  tools/
    ulama_gw.c          # main() — демон шлюза
  tests/
    test_cascade_frame.c
    test_lts_decoder.c
    test_fragmentation.c
  defaults/
    ulama-gw.conf       # конфиг (listen, peer, iface, node_id)
  scripts/
    S98ulama-gw         # init-скрипт
```

### 2.2 `media/vcpd/` — Видеодемон для LuckFox (ПРИОРИТЕТ 2)

**Назначение:** Работает на дроне. Захватывает видео с камеры ISP,
кодирует в H.264/MPEG-TS, фрагментирует через LTS, упаковывает в ULAMA
VIDEO frames и отправляет через UNOW.

**Почему отдельный проект:**
- Видеозахват — это отдельная подсистема со своими зависимостями
  (V4L2/ISP, аппаратный кодер RV1106, ffmpeg subprocess)
- Не должен утяжелять сетевой стек `media/ulama/`
- Может развиваться независимо (добавление OSD, adaptive bitrate, snapshot)

**Ключевая функциональность:**

```
Camera ISP → [HW Encoder / ffmpeg] → MPEG-TS → vcpd → LTS encoder → ULAMA VIDEO → UNOW
```

| Функция | Описание |
|---------|----------|
| Видеозахват | V4L2 или ffmpeg subprocess |
| H.264 кодирование | HW кодер RV1106 (mpp) или ffmpeg libx264 |
| LTS-кодирование | stream_id, pkt_seq, flags, группировка TS пакетов |
| ULAMA упаковка | LTS пакет → ULAMA frame VIDEO class |
| Отправка | Через UNOW TX (или UDP к локальному ulamad) |
| UVCP/1 | Обработка READY/STOP от оператора (управление потоком) |
| Adaptive bitrate | Реакция на DEGRADED/RECOVERED события ULAMA |
| OSD snapshot | Периодический JPEG → ULAMA OSD_IMAGE class |

**Зависимости:**
- `media/ulama/out/lib/libulama.a` — frame pack
- `media/unow/` или UDP к локальному `ulamad`
- `media/mpp/` — hardware encoder (RV1106 MPP)
- `ffmpeg` — fallback software encoder

**Целевая структура:**
```
media/vcpd/
  Makefile
  include/vcpd/
    lts_encoder.h       # LTS пакетизация
    uvcp.h              # UVCP/1 state machine
    video_source.h      # абстракция источника видео
  src/
    lts_encoder.c       # порт из ULAMA/host/vcpd/internal/lts/
    uvcp.c              # порт из ULAMA/host/vcpd/internal/proto/
    video_ffmpeg.c      # бэкенд: ffmpeg subprocess → pipe MPEG-TS
    video_mpp.c         # бэкенд: RV1106 hardware encoder (фаза 2)
    video_source.c      # диспатчер бэкендов
  tools/
    vcpd.c              # main() — видеодемон
  tests/
    test_lts_encoder.c
    e2e_video_loopback.sh
  defaults/
    vcpd.conf
  scripts/
    S97vcpd             # init-скрипт (до ulamad)
```

### 2.3 Расширение `media/ulama/` — MSP-телеметрия (ПРИОРИТЕТ 3)

**Назначение:** Добавить в `ulamad` на дроне возможность читать MSP-телеметрию
с полётного контроллера по UART и отправлять как ULAMA TELEMETRY frames.

**Почему в `media/ulama/`, а не отдельный проект:**
- `ulamad` уже владеет UART-выводом (CRSF→UART). MSP-чтение — обратное
  направление по тому же UART или второму UART.
- Минимальный MSP-парсер (< 200 строк C) не оправдывает отдельный проект.
- Телеметрия тесно связана с состоянием link (RSSI, health) из ULAMA L2.

**Необходимые изменения:**

| Файл | Что добавить |
|------|-------------|
| `include/ulama/msp.h` | MSP v1/v2 кодек (parse/build) |
| `src/common/msp.c` | Реализация MSP кодека |
| `tools/ulamad.c` | MSP read thread: UART RX → MSP parse → ULAMA TELEMETRY TX |
| `tests/test_msp.c` | Unit-test MSP golden vectors |

**MSP сообщения для телеметрии (минимум):**
- `MSP_ATTITUDE (108)` — roll, pitch, yaw
- `MSP_ALTITUDE (109)` — altitude
- `MSP_ANALOG (110)` — battery voltage, RSSI
- `MSP_RAW_GPS (106)` — lat, lon, speed, altitude
- `MSP_STATUS (101)` — armed, flight mode
- `MSP_BATTERY_STATE (130)` — detailed battery

---

## 3. Порядок работ (рекомендуемая последовательность)

### Фаза I — Шлюз (ulama-gw) — фундамент интеграции

Без шлюза cascade-core не может общаться с реальными дронами. Это
критический блокер всей финальной интеграции.

```
[cascade-core] ←──UDP──→ [ulama-gw] ←──UNOW──→ [дрон ulamad]
```

**Предусловия:** UNOW работает (radio inject/receive), `libulama.a` собирается.

### Фаза II — Видео (vcpd) на дроне

После шлюза видеопоток — следующий по приоритету. Без видео система
нефункциональна для FPV-оператора.

```
[камера] → [vcpd] → [ULAMA VIDEO] → [UNOW] → [ulama-gw] → [cascade-core] → [UI]
```

### Фаза III — MSP-телеметрия на дроне

Замена текстового `sim:` формата на реальный MSP от полётного контроллера.

```
[Betaflight FC] → [UART] → [ulamad MSP reader] → [ULAMA TELEMETRY] → ... → [cascade-core]
```

### Фаза IV — Порт L2/L3 (link_manager, mesh)

Полный порт ULAMA L2/L3 для надёжности, ретрансмиссий и multi-hop.
Описан в `media/ulama/TODO.md` §2–3.

### Фаза V — Полная интеграция и стресс-тесты

End-to-end с реальным железом: джойстик → cascade → ULAMA → дрон → видео → UI.

---

## 4. Детальный TODO

Статусы: `[ ]` не начато · `[~]` в работе · `[x]` готово · `[!]` риск.

### Фаза I: `media/ulama-gw/` — Шлюз оператора

#### [x] 1.1 Bootstrap проекта

Действия:
1. Создать структуру каталогов `media/ulama-gw/` согласно §2.1
2. Создать `Makefile` по образцу `media/ulama/Makefile` (host + target сборка)
3. Реализовать парсер `cascade-frame` в `src/cascade_frame.c`:
   - `cascade_frame_pack()`/`cascade_frame_unpack()` — 6-байтовый заголовок
   - Golden-test вектор из `docs/PROTOCOL.md`:
     `01 00 2A 00 07 01 73 69 6D 3A 73 65 71 3D 31`
4. Unit-test `tests/test_cascade_frame.c`

Готово, когда:
- `make -C media/ulama-gw host` собирается
- `test_cascade_frame` зелёный, golden vector совпадает

#### [x] 1.2 Базовый шлюз: cascade-frame → ULAMA → UNOW (downlink)

Действия:
1. В `tools/ulama_gw.c` реализовать демон:
   - CLI: `--cascade-in 127.0.0.1:5601 --iface mon0 --node <gateway_id>`
   - Слушает UDP от cascade-core
   - Парсит cascade-frame
   - Конвертирует: `src=u16→u8`, `dst=u16→u8`, `class` mapping
   - Пакует в `ulama_frame` через `ulama_frame_pack()`
   - Отправляет через UNOW TX
2. Конфиг маппинга адресов: `--addr-map 1=1,2=2,...` или auto (truncate u16→u8)

Проверка:
```bash
# cascade-core шлёт CTRL → ulama-gw → UNOW → ulamad на дроне получает
```

Готово, когда:
- CTRL-кадр от cascade-core доходит до `ulamad` на реальном LuckFox через UNOW

#### [x] 1.3 Базовый шлюз: UNOW → ULAMA → cascade-frame (uplink)

Действия:
1. Добавить UNOW RX → `ulama_frame_unpack()` → cascade-frame pack → UDP TX
2. CLI: `--cascade-out 127.0.0.1:5600`
3. Обработка: маппинг `src_node` u8 → `src` u16, class mapping

Проверка:
```bash
# ulamad на дроне шлёт TELEMETRY → UNOW → ulama-gw → cascade-core видит борт
```

Готово, когда:
- cascade-core видит телеметрию дрона через реальный радиоканал

#### [x] 1.4 Фрагментация downlink (cascade payload > 220B)

Действия:
1. Реализовать `fragmentation.c`: разбиение payload на фрагменты ≤ 220B
   с заполнением `frag_idx`, `frag_total`, `ULAMA_FLAG_FRAGMENT`,
   `ULAMA_FLAG_LAST_FRAGMENT`
2. Для CTRL (CRSF ~26B) фрагментация не нужна — проходит одним кадром
3. Для VIDEO/OSD_IMAGE — обязательна

Готово, когда:
- Cascade payload 1400B → 7 ULAMA-фрагментов → отправлены по UNOW

#### [x] 1.5 Дефрагментация uplink (ULAMA fragments → cascade payload)

Действия:
1. Реализовать reassembly в `fragmentation.c`: сборка фрагментов по
   `(src_node, seq)` ключу, таймаут 200 мс, 4–8 слотов
2. Собранный payload → cascade-frame → cascade-core

Готово, когда:
- Видеопакет с дрона (>220B после LTS) приходит как один cascade-frame

#### [x] 1.6 LTS-декодирование видео (uplink)

Действия:
1. Портировать `ULAMA/host/vcpd/internal/lts/` в C:
   - `lts_decoder.c`: reorder-буфер, dedup по `pkt_seq`, anti-stall emit
   - Окно reorder: 64 пакета (настраиваемо)
   - Emit deadline: 80 мс (настраиваемо)
2. NACK-генератор: при обнаружении gap отправить NACK обратно дрону
   через ULAMA CTRL frame
3. На выходе: чистый MPEG-TS поток → cascade-frame VIDEO → cascade-core

Готово, когда:
- Видео с 5% packet loss на канале проигрывается в UI без freeze >200 мс

#### [x] 1.7 Конфиг, init-скрипт, deploy

Действия:
1. `defaults/ulama-gw.conf` — cascade-in, cascade-out, iface, node, log level
2. `scripts/S98ulama-gw` — Buildroot init
3. `deploy-to-ground.sh` — аналог `deploy-to-luckfox.sh`

Готово, когда:
- Шлюз автозапускается на ground-station LuckFox/Pi

#### `[!]` Риск I.R1: CRSF формат payload

Текущий `control_sender.go` в cascade-core формирует CRSF payload как
`[0x16][22B packed]` (23 байта), а `ulamad` на дроне ожидает полный serial
frame `[0xC8][0x18][0x16][22B][CRC8]` (26 байт). Варианты решения:

- (A) cascade-core формирует полный serial frame — РЕКОМЕНДУЕТСЯ
- (B) ulama-gw добавляет serial обёртку
- (C) drone-side ulamad добавляет обёртку

Решение: согласовать с владельцем cascade-core и зафиксировать.

---

### Фаза II: `media/vcpd/` — Видеодемон для дрона

#### [x] 2.1 Bootstrap проекта

Действия:
1. Создать `media/vcpd/` по структуре из §2.2
2. Makefile с host + target сборкой
3. Портировать LTS-кодер из Go в C:
   - `lts_encoder.c`: заголовок 4B, pkt_seq инкремент, группировка TS пакетов
   - Максимум 1 LTS пакет = N×188 байт TS (N подбирается под ULAMA payload limit)
4. Unit-test `test_lts_encoder.c`

Готово, когда:
- LTS encode → decode round-trip совпадает бит-в-бит
- Golden vector совпадает с Go-версией

#### [x] 2.2 Видеозахват через ffmpeg subprocess

Действия:
1. `video_ffmpeg.c`: запуск ffmpeg → pipe stdout → MPEG-TS
2. CLI: `--source /dev/video0 --bitrate 512k --codec h264`
3. Чтение 188-байтных TS пакетов из pipe
4. Группировка в LTS пакеты (7×188 = 1316 ≤ ULAMA max payload)

**Примечание:** на LuckFox RV1106 есть аппаратный H.264 кодер через MPP
(media process platform). Фаза 2 — интеграция с `media/mpp/` напрямую,
минуя ffmpeg. MVP начинаем с ffmpeg.

Готово, когда:
- `vcpd` стабильно читает TS от ffmpeg и логирует пакеты

#### [x] 2.3 Отправка через ULAMA VIDEO

Действия:
1. LTS пакет → `ulama_frame_pack()` с traffic_class=VIDEO
2. Отправка через UNOW TX (или через UDP к локальному `ulamad`)
3. Rate control: не превышать канальную ёмкость (~1 Мбит/с для UNOW)

Два варианта доставки:
- **Вариант A**: `vcpd` сам шлёт через UNOW (прямой доступ к радио)
- **Вариант B**: `vcpd` шлёт UDP к `ulamad`, который ретранслирует

Рекомендация: Вариант A для MVP (проще, меньше задержка). `ulamad` на
дроне можно расширить позже для мультиплексирования VIDEO+CTRL+TELEM.

Готово, когда:
- Видео с LuckFox-камеры через UNOW доходит до ground ulama-gw

#### [x] 2.4 UVCP/1 управление потоком

Действия:
1. Портировать UVCP/1 state machine из Go в C
2. Обработка READY от оператора → начать/возобновить стрим
3. Lease mechanism: остановить стрим если нет READY/PONG > 6с
4. Реакция на ULAMA DEGRADED события: снизить битрейт

Готово, когда:
- Видео стартует по READY, останавливается по таймауту lease

#### [x] 2.5 Аппаратный кодер RV1106 MPP (фаза 2)

Действия:
1. `video_mpp.c`: прямой доступ к ISP + MPP кодеру (минуя ffmpeg)
2. Зависимость: `media/mpp/`, `media/isp/`
3. Значительно ниже CPU-нагрузка и задержка

Готово, когда:
- Видео кодируется аппаратно, CPU-нагрузка < 10%

---

### Фаза III: MSP-телеметрия в `media/ulama/`

#### [x] 3.1 MSP-кодек

Действия:
1. `include/ulama/msp.h` + `src/common/msp.c`
2. MSP v1: `$M>` + len + code + payload + XOR checksum
3. MSP v2: `$X>` + flag + code:u16 + len:u16 + payload + CRC8 DVB-S2
4. Golden vectors из Betaflight/INAV спецификации

Готово, когда:
- `test_msp.c` зелёный, v1 и v2 golden vectors совпадают

#### [x] 3.2 MSP read thread в ulamad

Действия:
1. Добавить в `ulamad` второй UART (или RX-направление основного):
   `--msp-uart /dev/ttyS4` или `--msp-rx` на том же UART (half-duplex CRSF)
2. Thread: читает MSP responses, формирует ULAMA frame TELEMETRY, отправляет TX
3. Частота: 5–10 Гц (по таймеру отправлять MSP-запросы, парсить ответы)
4. MSP polling: отправлять запросы ATTITUDE/ALTITUDE/ANALOG/GPS/STATUS

`[!]` **Риск III.R1**: CRSF и MSP на одном UART. Betaflight поддерживает
half-duplex CRSF (RX+TX на одном UART). MSP-over-CRSF (device ping/response)
позволяет читать телеметрию через тот же CRSF-канал. Альтернатива: второй UART.

Готово, когда:
- cascade-core получает MSP-телеметрию от реального FC через ULAMA

#### [x] 3.3 Обновление cascade-core для MSP

Действия:
1. В `core/internal/api/telemetry.go`: добавить ветку парсинга MSP
   (если payload начинается с `$M` или `$X` — парсить MSP, иначе `sim:`)
2. Маппинг MSP → `DroneSnapshot` (attitude, battery, GPS, armed, mode)
3. Unit-test на MSP frame → snapshot conversion

Готово, когда:
- Реальный дрон виден в cascade-core UI с MSP-телеметрией

---

### Фаза IV: Порт L2/L3 ULAMA (параллельно)

Описана в `media/ulama/TODO.md` §2–3. Ключевые задачи:

- [ ] **4.1** Compat-шим ESP-IDF → POSIX (§2 в TODO.md)
- [ ] **4.2** L2 link_manager порт (ACK/ARQ, reassembly, health)
- [ ] **4.3** L3 mesh порт (OGM, routing, relay, dedup)
- [ ] **4.4** Интеграция L2/L3 в ulama-gw и drone-side ulamad

`[!]` **Решение:** L2/L3 нужен для надёжности и multi-hop, но НЕ блокирует
фазы I–III. Фазы I–III работают на одном хопе с L1-only (что уже делает
вертикальный MVP CRSF).

---

### Фаза V: End-to-end интеграция

#### [ ] 5.1 Smoke-тест: джойстик → видео

Полный путь:
```
USB джойстик
  → cascade-core (CRSF pack, 50 Hz)
  → cascade-frame CTRL
  → ulama-gw (→ ULAMA frame → UNOW TX)
  → air
  → drone ulamad (UNOW RX → CRSF → UART3 → Betaflight)

LuckFox камера
  → vcpd (H.264 → MPEG-TS → LTS → ULAMA VIDEO)
  → UNOW TX → air
  → ulama-gw (UNOW RX → LTS decode → cascade-frame VIDEO)
  → cascade-core → WebSocket → UI видеоплеер

Betaflight FC
  → ulamad MSP reader (UART → MSP → ULAMA TELEMETRY)
  → UNOW TX → air
  → ulama-gw (→ cascade-frame TELEMETRY)
  → cascade-core → WebSocket → UI телеметрия
```

Проверка:
- В UI видно видео с дрона
- Джойстик управляет дроном (attitude меняется в телеметрии)
- Телеметрия живая (battery, GPS, armed status)

#### [ ] 5.2 Стресс-тест деградации

- Видео на максимуме + команды одновременно
- CTRL-кадры доходят с ≥99% reliability
- VIDEO деградирует gracefully (артефакты, но не crash)
- Keepalive ulamad предотвращает failsafe FC

#### [ ] 5.3 Совместимость с симулятором

- ulama-gw должен уметь работать с `ulama-netsim` (UDP вместо UNOW)
  для тестирования без железа
- CLI: `--transport udp --peer 127.0.0.1:7500` (как в ulamad)

---

## 5. Зависимости между артефактами

```
                                ┌─────────────┐
                                │ cascade-core │
                                │ (существует) │
                                └──────┬───────┘
                                       │ UDP
                                ┌──────▼───────┐
                     ┌──────────│  ulama-gw    │──────────┐
                     │          │  (НОВЫЙ)      │          │
                     │          └──────┬───────┘          │
                     │                 │ UNOW              │
                     │          ┌──────▼───────┐          │
                     │          │  libulama.a  │          │
                     │          │ (существует)  │          │
                     │          └──────┬───────┘          │
                     │                 │                   │
              ┌──────▼───────┐  ┌─────▼──────┐  ┌────────▼───────┐
              │    vcpd      │  │   ulamad    │  │    libunow     │
              │ (НОВЫЙ)      │  │ (расширить) │  │  (существует)  │
              └──────────────┘  └────────────┘  └────────────────┘
```

**Порядок сборки:**
1. `media/unow/` → `media/ulama/` → `libulama.a` (уже работает)
2. `media/ulama-gw/` (зависит от libulama + libunow)
3. `media/vcpd/` (зависит от libulama, опционально libunow)
4. Расширение `media/ulama/` (MSP) — параллельно с 2–3

---

## 6. Риски и решения

| # | Риск | Влияние | Решение |
|---|------|---------|---------|
| R1 | CRSF payload format mismatch | CTRL не доходит до FC | Согласовать формат (§1.3, рекомендация: полный serial frame в cascade-core) |
| R2 | LTS C-порт расходится с Go | Видео не собирается на приёме | Golden vectors + bit-exact тесты между Go и C |
| R3 | UNOW throughput < 1 Мбит/с | Видео фризит | Adaptive bitrate в vcpd, emergency снижение до 256 кбит/с |
| R4 | Один UART для CRSF TX + MSP RX | Half-duplex конфликт | MSP-over-CRSF device protocol ИЛИ второй UART |
| R5 | uclibc threading/timing | Keepalive 150 Hz джиттерит | Замер jitter на реальном железе; при необходимости RT-приоритет |
| R6 | Шлюз как single point of failure | Потеря связи | Watchdog, auto-restart, graceful degradation |

---

## Приложение: Карта существующего кода

| Путь | Что | Статус |
|------|-----|--------|
| `luxfox/media/ulama/` | ULAMA L1 frame + CRSF MVP | Работает (host + target) |
| `luxfox/media/ulama/tools/ulamad.c` | Drone-side: ULAMA RX → CRSF → UART | Работает |
| `luxfox/media/ulama/tools/ulama_js_tx.c` | Host-side: joystick → CRSF → ULAMA TX | Работает |
| `luxfox/media/ulama/src/common/crsf.c` | CRSF RC_CHANNELS_PACKED кодек | Работает |
| `luxfox/media/ulama/include/ulama/*.h` | Публичные заголовки ULAMA | Стабильные |
| `luxfox/media/unow/` | UNOW радио (monitor mode, inject) | Работает |
| `ULAMA/host/vcpd/` | VCP-LTS видеотранспорт (Go) | Работает (Go, не C) |
| `ULAMA/host/vcpd/internal/lts/` | LTS кодек (Go) | Работает (reference для C-порта) |
| `ULAMA/host/vcpd/internal/proto/` | UVCP/1 кодек (Go) | Работает (reference для C-порта) |
| `cascade/core/` | cascade-core НСУ бэкенд | Работает |
| `cascade/core/internal/api/control_sender.go` | CRSF packing + cascade-frame CTRL | Работает |
| `cascade/core/internal/api/video_ws.go` | VIDEO → WebSocket relay | Работает |
| `cascade/tests/ulama-simulator/` | Симулятор сети ULAMA | Работает (фазы 0–2) |
