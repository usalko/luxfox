# Проект 1 — `ulama` (OEM-порт стека ULAMA на LuckFox Pico Ultra)

> Цель: перенести **всё, что переносимо**, из `../ULAMA` (ESP-IDF/FreeRTOS,
> ESP32 + LilyGo T3) в нативный Linux/Buildroot OEM-демон на LuckFox Pico Ultra.
> Радио-транспорт ESP-NOW заменяется на **UNOW** (см. отдельный проект
> [`media/unow/README.md`](../unow/README.md)). Стек L1/L2/L3 (кадр, link_manager,
> mesh, resilience, фрагментация) переносится с тонким POSIX-шимом.

Статусы: `[ ]` не начато, `[~]` в работе, `[x]` готово, `[!]` риск/требует решения.
Каждый крупный этап завершается проверяемым артефактом (бинарь, лог, тест, замер).

---

## 0. Главный архитектурный вывод (читать первым)

На ESP32 система — это **две коробки**: ноутбук (приложение) ↔ ESP32 (радио),
связанные `USB-CDC/UART + SLIP`. На LuckFox это **одна коробка** — плата сама
исполняет и приложение (ffmpeg/UDP-сервис), и драйвит Wi-Fi-радио. Отсюда три
следствия, определяющие весь порт:

1. **Host-frontend схлопывается в локальный интерфейс.** Слой
   `cdc_tunnel`/`host_frontend` (SLIP-поверх-UART к внешнему ноуту) и Go-демон
   `ulama-tun` (UDP↔SLIP мост) сливаются в **локальный UDP-сокет** (`udp://
   127.0.0.1:5000`) или `TUN`-устройство прямо на LuckFox. SLIP-кадрирование на
   этом стыке больше не нужно (нет физической serial-линии) — кадры ходят через
   in-process очередь.
2. **`radio_espnow` → `radio_unow`.** ESP-NOW на LuckFox отсутствует; его
   заменяет UNOW (сырые 802.11 action-frame через monitor+inject на MW300UH/
   RTL8192EU), реализующий **тот же интерфейс** `radio_espnow.h`. Стек выше не
   меняется.
3. **`radio_lora` опционален.** У LuckFox есть SPI (`spidev`), поэтому
   userspace-драйвер SX1276 реалистичен, но это фаза 2. MVP — single-radio
   (только UNOW), как ESP-NOW-MVP в исходном ULAMA.

Ключевая особенность ULAMA, которую сохраняем дословно: **маленькие пакеты и
адресация одним байтом** (`u8 src_node` / `u8 dst_node`, 14-байтовый заголовок,
payload ≤ 220 Б). Это уже ровно то, что просил заказчик — не изобретаем заново.

```
БЫЛО (ESP32):
  [laptop app] --UDP--> [ulama-tun] --SLIP/UART--> [ESP32: link+mesh+espnow] ))) air

СТАЛО (LuckFox):
  [app on LuckFox] --UDP 127.0.0.1--> [ulamad: host_local + link + mesh + UNOW] ))) air
```

---

## 0.1 Быстрый вертикальный MVP до CRSF/Betaflight

Цель ближайшего среза: не ждать полного порта `link_manager/mesh`, а как можно
быстрее получить проверяемый тракт **"команды стиков → CRSF → UART3"**.

- [x] Собран минимальный userspace-пакет `media/ulama/` с переносом `ulama_frame`
  и Linux-сборкой под host и LuckFox.
- [x] Добавлен `ulama_js_tx`: формирует `CRSF RC_CHANNELS_PACKED`, заворачивает
  его в `ULAMA L1` и отправляет через backend `UDP` или `UNOW`.
- [x] Добавлен `ulamad`: принимает `ULAMA L1`, валидирует `CRSF` и пишет кадр в
  `UART`, `stdout` или файл.
- [x] Добавлен host-only regression-тест `tests/e2e_udp_crsf.sh`:
  `ulama_js_tx -> UDP -> ulamad -> CRSF-bytes`, байты на выходе совпадают бит-в-бит.
- [x] Добавлен аппаратный wrapper `tests/run_unow_crsf_smoke.sh` для двух ролей:
  `host-tx` и `luckfox-rx`, чтобы следующий smoke `UNOW -> UART3` запускался без
  ручной сборки длинных команд.
- [x] `make` в `media/ulama` собирает target-бинарь `ulamad` под
  `ARM EABI uclibc` и кладёт артефакты в `media/out`.
- [x] Добавлены дефолтный `defaults/ulama.conf`, support `--config` в `ulamad` и
  init-скрипт `scripts/S99ulama` для Buildroot/rootfs-интеграции.
- [ ] Следующий аппаратный smoke: `UNOW RX on LuckFox -> /dev/ttyS3 @ 420000 -> Betaflight CRSF RX`.
- [ ] Следующий end-to-end smoke: живой `--joystick /dev/input/js0` на хосте +
  реальный радиоканал `UNOW` между двумя USB-адаптерами.

Промежуточные артефакты уже в репозитории:

- [x] `make -C media/ulama host`
- [x] `make -C media/ulama test`
- [x] `cd media/ulama && ./tests/e2e_udp_crsf.sh`
- [x] `cd media/ulama && ./tests/e2e_udp_crsf_config.sh`

---

## 1. Карта переноса (порт-боундари)

| Компонент ULAMA | Строк | Решение на LuckFox | Риск |
|---|---:|---|---|
| `link/ulama_frame.{c,h}` | 158 | **Порт как есть** (чистый C, без зависимостей) | низкий |
| `cdc_tunnel/slip.{c,h}` | 126 | **Порт как есть** (понадобится только для UART-режима/отладки) | низкий |
| `link/link_manager.{c,h}` | 1593 | **Порт + шим** (esp_log, esp_timer, FreeRTOS task/queue) | средний |
| `mesh/*` (`mesh_router`, `mesh_neighbor`, `mesh_types`) | ~1300 | **Порт + шим** (тот же шим) | средний |
| `link/ulama_frame` CRC/ARQ/reasm (внутри link_manager) | — | Порт как есть (чистая логика) | низкий |
| `radio_espnow.{c,h}` | 464 | **Заменить на UNOW** (см. проект 2), сохранить API `radio_espnow.h` | высокий `[!]` |
| `radio_lora.{c,h}` | 1055 | **Фаза 2**: spidev-драйвер SX1276 | средний |
| `cdc_tunnel` / `host_frontend` | 539 | **Заменить** на локальный UDP/TUN host-iface | низкий |
| `pair_uart` | 552 | **Выбросить** (внутренний T3↔T3 split-radio; на одиночном LuckFox не нужен) | — |
| `board_t3v161` | 595 | **Выбросить** (специфика платы ESP32: OLED/BOOT/LED/pins) | — |
| `main/main.c` | 2354 | **Переписать** как Linux-демон `ulamad` (роль/CLI/конфиг-файл вместо NVS) | средний |
| `host/ulama-tun` (Go) | 445 | **Переиспользовать** на удалённом пире ИЛИ свернуть в демон | низкий |
| `host/vcpd` (Go) | ~1600 | Референс для UVC media-proxy (опционально, видео-стенд) | — |
| `host/tools/udp-relay` (Go) | 147 | Полезно для тестового стенда (петля/трафик-ген) | — |

Итог: из ~11k строк **переносится ядро ~3.2k строк C** (frame+link+mesh+slip),
заменяется ~0.5k (radio→UNOW), переписывается main, выбрасывается специфика T3.

---

## 2. Слой совместимости ESP-IDF → POSIX (`compat/`)

Создать тонкий шим, чтобы перенесённые `.c` собрались без правок (или с
минимальными `#ifdef`). Это самый дешёвый путь: не рефакторим бизнес-логику.

| ESP-IDF API | Что используется | POSIX-замена (`media/ulama/src/compat/`) |
|---|---|---|
| `esp_log.h` `ESP_LOGI/W/E/D` | логирование с тегом | `ulama_log.h`: макросы над `fprintf(stderr,...)` + уровень из env `ULAMA_LOG` / опц. `syslog` |
| `esp_timer_get_time()` (µs, int64) | таймстемпы, окна, RTT | `clock_gettime(CLOCK_MONOTONIC)` → µs |
| `freertos/FreeRTOS.h`,`task.h` (`xTaskCreate`,`vTaskDelay`,`TaskHandle_t`,`tskNO_AFFINITY`) | воркеры TX/RX/resilience/mesh | `pthread_create` + `nanosleep`; `TaskHandle_t`→`pthread_t` |
| `freertos/queue.h` (`xQueueCreate/Send/Receive`,`uxQueueMessagesWaiting`) | TX/RX очереди, depth-stats | `ulama_queue.c`: thread-safe ring (mutex+cond), фикс-размер элемента |
| `esp_err.h` (`esp_err_t`,`ESP_OK`,`ESP_FAIL`) | коды возврата | `ulama_err.h`: `typedef int`, `#define ESP_OK 0` |
| `esp_mac.h`/`esp_wifi` (`esp_read_mac`) | self-MAC | UNOW: `SIOCGIFHWADDR` / `/sys/class/net/<if>/address` |
| `nvs_flash` (`nvs_get_*`/`set_*`) | node_id, role, пороги | конфиг-файл `/etc/ulama.conf` (`ini`) + CLI-флаги |
| `mbedtls` AES-GCM | шифрование L1 (опц.) | фаза 2: `rkcrypto` (HW-AES на RV1106) ИЛИ openssl/mbedtls; MVP = plain+CRC (разрешено арх-доком §6) |
| `__atomic_*` builtins | счётчики статистики | работают в gcc как есть (ничего не нужно) |

`[!]` Решение по конкурентности: на ESP32 — FreeRTOS-задачи + очереди. На Linux
повторяем 1:1 на pthread, **сохраняя ту же модель потоков** (TX-воркер, RX-воркер,
resilience-таймер 200 мс, mesh-OGM-таймер). Не переписываем на epoll-event-loop в
MVP — это исказит тайминги/тесты. Оптимизацию под один event-loop оставить на
потом.

### Задачи
- [ ] **2.1** `compat/ulama_log.h` — уровни ERROR/WARN/INFO/DEBUG, тег, env-фильтр.
- [ ] **2.2** `compat/ulama_time.{c,h}` — `ulama_now_us()`, `ulama_sleep_ms()`.
- [ ] **2.3** `compat/ulama_queue.{c,h}` — ring-queue с API, изоморфным
  `xQueueCreate/Send/Receive/uxQueueMessagesWaiting` (+ timeout, + non-block).
- [ ] **2.4** `compat/ulama_task.{c,h}` — обёртка над pthread c affinity-no-op.
- [ ] **2.5** `compat/esp_compat.h` — агрегирующий заголовок: подменяет
  `esp_err.h`, `esp_log.h`, `esp_timer.h`, `freertos/*` через include-path так,
  чтобы перенесённые исходники не правились (`-I src/compat/shim`).
- [ ] **2.6** Артефакт: `compat`-юниттест собирается под x86 (host) и arm (target),
  очередь проходит multi-producer/consumer стресс без потерь/гонок (tsan на host).

---

## 3. Порт ядра (L1/L2/L3) — `media/ulama/src/core/`

### 3.1 L1 — формат кадра
- [x] **3.1.1** Скопировать `ulama_frame.{c,h}` без изменений; собрать под arm/x86.
- [~] **3.1.2** Юниттест pack/unpack/crc16 (вектора из ESP-версии должны
  совпасть **бит-в-бит** — это гарантия совместимости по эфиру с T3-узлами).
- [~] Артефакт: `test_ulama_frame` зелёный на host; target-сборка проходит; запуск теста
  на железе ещё не прогнан. CRC-вектор совпал с портированной реализацией ESP-кадра.

### 3.2 L2 — link manager + resilience + reassembly
- [ ] **3.2.1** Перенести `link_manager.{c,h}`; заменить include'ы ESP→compat.
- [ ] **3.2.2** Развязать прямые вызовы `cdc_tunnel_send_frame()` →
  `host_iface_deliver()` (см. §4). Это единственная инвазивная правка в L2.
- [ ] **3.2.3** Сохранить per-radio health (`s_radio_health[2]`), resilience-task
  (200 мс), CTRL dup/retry, VIDEO degraded/reduced/recovered как есть.
- [ ] **3.2.4** Reassembly (4 слота, TTL 200 мс) — порт как есть.
- [ ] `[!]` **3.2.5** Индекс радио: на MVP активно только `RADIO_INDEX_ESPNOW`
  (=UNOW). `RADIO_INDEX_LORA` остаётся в структуре, но health=DEAD до фазы 2.
- [ ] Артефакт: host-тест переходов health DEAD/PROBING/DEGRADED/OK по
  decision-matrix (§4.4 ARCHITECTURE.md) зелёный.

### 3.3 L3 — mesh
- [ ] **3.3.1** Перенести `mesh_router.c`, `mesh_neighbor.c`, `mesh_types.h`.
- [ ] **3.3.2** OGM/HELLO сейчас бегут по двум радио; на MVP — только по UNOW
  (LoRa-плоскость выключена), per-radio таблица сохраняется для фазы 2.
- [ ] **3.3.3** Loop-prevention (LRU seen `src,seq`) и TTL — порт как есть.
- [ ] `[!]` **3.3.4** Mesh-relay остаётся **только** в `mesh_router` (НЕ в
  транспорте UNOW — см. проект 2, критический разбор). UNOW = тупой L2 one-hop.
- [ ] Артефакт: host-симуляция 3 узлов (моки радио) — пакет A→C через B,
  TTL-декремент, dedup, route-table заполняется.

---

## 4. Host-интерфейс (локальный) — `media/ulama/src/host/`

Заменяет `cdc_tunnel`+`host_frontend`+`ulama-tun`. Демон сам слушает UDP и
мостит его в link-manager.

- [ ] **4.1** `host_iface.{c,h}` — абстракция стыка приложение↔стек:
  `host_iface_submit(payload,len)` (app→air), `host_iface_deliver(payload,len)`
  (air→app). Внутри link_manager зовёт `host_iface_deliver`.
- [~] **4.2** Backend **UDP** (MVP): `--listen 0.0.0.0:5000 --peer-port 5001`.
  Каждый UDP-датаграм = один payload-кадр ULAMA (как и было, но без SLIP).
  Класс трафика и `dst_node` — из конфигурации/таблицы маршрутов или из
  лёгкого префикса (решить: маппинг `udp_port→{dst_node,class}`). Уже реализован
  минимальный backend `UDP` для `ulama_js_tx/ulamad`, one-datagram = one-ULAMA-frame.
- [ ] **4.3** Backend **TUN** (фаза 2): поднять `tunX`, L3-интерфейс, чтобы
  любое IP-приложение ходило прозрачно. Опционально.
- [ ] **4.4** Backend **UART** (отладка/совместимость): переиспользовать
  перенесённый `slip.c` + `media/luxfox/.../luckfox_uart` для стыка с внешним
  T3/устройством (режим «LuckFox как host для T3», описан в HOST_UART_LUCKYFOX_TODO).
- [ ] **4.5** Служебные кадры `ULAMA_STATS`/`ULAMA_EVT` (degraded/recovered) →
  лог/JSONL/локальный сокет статистики (порт логики из `ulama-tun/main.go`).
- [~] Артефакт: host-only e2e уже закрыт скриптом `tests/e2e_udp_crsf.sh`; полный
  `app↔ulamad↔(mock radio)↔ulamad↔app` поверх нового `host_iface` ещё впереди.

---

## 5. Демон `ulamad` — `media/ulama/src/main/`

Переписать `main.c` как Linux-демон (без OLED/NVS/boot-banner ESP).

- [~] **5.1** CLI: `--node <1..253>`, `--role solo`, `--iface mon0`,
  `--listen`, `--peer`, `--config /etc/ulama.conf`, `--log <level>`.
- [~] Реализован минимальный CLI для MVP: `--transport`, `--iface`, `--listen`,
  `--node`, `--uart`, `--baud`, `--output`, `--stdout`, `--count`, `--ready-file`, `--verbose`.
- [ ] **5.2** Конфиг-файл (ini) ← замена NVS: node_id, пороги health, video
  emergency kbps, ctrl_dup. Дефолты = из `link_manager_default_resilience_config`.
- [~] Базовый ini-парсер уже есть: `transport`, `iface`, `listen`, `node`, `uart`,
  `baud`, `output`, `count`, `ready_file`, `verbose`. Параметры resilience/link_manager
  пока не подключены.
- [~] **5.3** Инициализация в порядке: config → UNOW(radio) → link_manager →
  mesh → host_iface → запуск воркеров. (Граф из ARCHITECTURE.md §5.6 без
  pair_uart/board.) В MVP сейчас поднят вертикальный путь `transport -> ULAMA L1 -> CRSF -> UART`.
- [~] **5.4** Сигналы `SIGINT/SIGTERM` → грациозное завершение (flush очередей,
  закрыть pcap/сокеты). `SIGHUP` → reload конфига.
- [~] `SIGINT/SIGTERM` уже обрабатываются; `SIGHUP/reload` пока нет.
- [~] **5.5** Опц. интеграция с init: S-скрипт/`/etc/init.d/S99ulama` для
  Buildroot-rootfs (автозапуск).
- [~] `scripts/S99ulama` и `config/ulama.conf` уже пакуются в `out/etc`; запуск на
  реальном rootfs ещё не прогнан.
- [~] Артефакт: `ulamad` собирается под ARM и host; host e2e с `UDP` пройден;
  on-device runtime с `UNOW` и `UART3` ещё не прогнан.

---

## 6. Сборка и интеграция в прошивку — `media/ulama/Makefile`

`media/Makefile` авто-подхватывает любой `media/*/Makefile` (кроме `samples`),
так что отдельной регистрации не нужно. Брать за образец
[`media/luxfox/Makefile`](../luxfox/Makefile).

- [x] **6.1** Makefile: `CC := $(RK_MEDIA_CROSS)-gcc`, `-I include`,
  `-I src/compat/shim`, сборка `libulama.a` + бинаря `ulamad`. Линковка с
  `UNOW`-исходниками/`libpcap`, `-lpthread`.
- [x] **6.2** Положить артефакты в `RK_MEDIA_OUTPUT` (правило
  `MAROC_COPY_PKG_TO_MEDIA_OUTPUT`, как в luxfox), бинарь → rootfs `/oem` или
  `/usr/bin`.
- [x] `[!]` **6.3** Toolchain — **uclibc** (`arm-rockchip830-linux-uclibcgnueabihf`).
  Ядро ULAMA — чистый C, проблем не будет. Следить, чтобы compat-шим не тянул
  glibc-специфику. Текущий MVP уже кросс-собирается под uclibc.
- [x] **6.4** Хост-сборка для тестов: тот же код собрать `gcc` под x86
  (Makefile-таргет `host`), чтобы юниттесты L1/L2/mesh гонять без железа.
- [x] Артефакт: `make -C media/ulama` собирает `ulamad`, он появляется в `media/out`;
  `file ulamad` → `ELF 32-bit LSB executable, ARM, EABI5, interpreter /lib/ld-uClibc.so.0`.

---

## 7. Тестовые гейты (проверяемые)

| Гейт | Что проверяет | Критерий прохода |
|---|---|---|
| **G1.5** | host-only вертикальный CRSF-срез без радио | `tests/e2e_udp_crsf.sh` проходит; CRSF-байты на выходе идентичны отправленным |
| **G2** | юниттесты L1/SLIP/mesh на host **и** target | все зелёные; CRC-вектор == ESP |
| **G3** | p2p UDP-echo между двумя LuckFox через UNOW | 1000 кадров, PER < 2% @ ~30 см |
| **G4** | throughput + видео | `iperf3 -u -b 1M -t 30`; `ffmpeg→ffplay` визуально ок |
| **G5** | mesh 3 узла, relay A→B→C | route-table заполнена, TTL/dedup работают |
| **G6** | resilience: глушим UNOW | CTRL уходит в retry-queue, VIDEO → degraded-event |

> G0/G1 (инжект/loopback самого радио) — в проекте 2; **без них к G3 не
> переходить**.

---

## 8. Риски и решения

- `[!]` **R1. rtl8xxxu inject** — критический риск всего предприятия; закрывается
  гейтом G0 проекта 2 **до** начала порта L2/L3.
- `[!]` **R2. uclibc + pthread-тайминги** — FreeRTOS tick (1 мс) vs Linux
  scheduler; resilience-окна (2 с) и HELLO-джиттер устойчивы к этому, но
  smoke-замерить латентность одного хопа (цель < 5 мс на L2, эфир отдельно).
- **R3. Совместимость по эфиру с T3** — если нужна гетерогенная сеть LuckFox↔T3,
  UNOW-кадр должен нести **тот же ULAMA L1** и тот же 802.11-несущий формат, что
  ESP-NOW T3 не выдаёт (ESP-NOW — свой vendor-action). Решение: гетерогенность
  LuckFox↔T3 = **не цель MVP**; цель — LuckFox↔LuckFox. Зафиксировать с
  заказчиком.
- **R4. Видео-битрейт через инжект** — без MAC-ACK и на фикс. legacy-рейте
  пропускная ниже ESP-NOW; адаптивный VIDEO-профиль ULAMA это уже умеет
  (degraded/emergency). Замерить реальный потолок на G4.

---

## 9. Порядок работ (рекомендуемая последовательность)

1. Проект 2 (UNOW) до гейта **G1** — без работающего радио порт бессмыслен.
2. §2 compat-шим → §3.1 L1 + юниттест (G2 частично).
3. §3.2 link_manager + §4 host UDP → §5 `ulamad` solo → **G3**.
4. §6 интеграция в Buildroot-образ.
5. §3.3 mesh → **G5**; resilience → **G6**; throughput/видео → **G4**.
6. Фаза 2: radio_lora (spidev), TUN-backend, crypto (rkcrypto), гетерогенность.

---

## Приложение A. Целевая структура каталога

```
media/ulama/
  Makefile
  include/ulama/            # публичные заголовки (frame, link, mesh, host_iface)
  src/
    compat/                 # шим ESP-IDF → POSIX (§2)
      shim/                 # esp_log.h, esp_err.h, esp_timer.h, freertos/*
    core/                   # ulama_frame, link_manager, mesh_* (порт §3)
    common/                 # crsf, uart, udp/unow transport helpers
    host/                   # host_iface (UDP/TUN/UART) (§4)
    main/                   # ulamad (§5)
  tools/                    # ulamad, ulama_js_tx
  tests/                    # host-юниттесты + host-only e2e CRSF regression
  TODO.md                   # этот файл
```
