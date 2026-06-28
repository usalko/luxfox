# Unified Radio Multiplexer Architecture — TODO

## Problem Statement

На LuckFox (device) два независимых процесса (vcpd, ulamad) конкурируют
за один WiFi adapter (wlan0) в monitor mode. Каждый открывает свой pcap
handle и делает pcap_inject() без координации. При интенсивной передаче
видео (35+ TX/sec с async reliable retries = 210 TX/sec) radio постоянно
в TX mode → ulamad не может принимать control frames → задержка
управления 7-8 секунд. **ОПАСНО ДЛЯ ПОЛЁТА**.

## Current Architecture (broken)

```
LuckFox (device):                    Host (ground station):
┌─────────┐  ┌─────────┐            ┌──────────┐
│  vcpd   │  │ ulamad  │            │ ulama-gw │
│ VIDEO TX│  │ CTRL RX │            │ все классы│
│ ACK RX  │  │TELEM TX │            │ CASCADE  │
└────┬────┘  └────┬────┘            └────┬─────┘
     │ pcap       │ pcap                 │ pcap
     └──────┬─────┘                      │
            ▼                            ▼
        wlan0 (monitor)              wlan0 (monitor)
        ← КОНФЛИКТ! →               ← OK (один процесс) →
```

## Target Architecture

```
LuckFox (device):                    Host (ground station):
┌─────────┐  ┌─────────┐            ┌──────────┐
│  vcpd   │  │ ulamad  │            │ ulama-gw │
└────┬────┘  └────┬────┘            └────┬─────┘
     │ IPC        │ IPC                  │
     └──────┬─────┘                      │
            ▼                            ▼
     ┌─────────────┐              ┌─────────────┐
     │   radiod    │              │   ulama-gw   │
     │ (новый MUX) │              │ (уже MUX)    │
     │             │              │              │
     │ TX scheduler│   ← radio → │ TX scheduler │
     │ RX dispatch │              │ RX dispatch  │
     │ TDMA slots  │              │ TDMA slots   │
     └──────┬──────┘              └──────┬───────┘
            ▼                            ▼
        wlan0 (monitor)              wlan0 (monitor)
```

## Design Principles

1. **ОДИН процесс на radio** — radiod владеет pcap handle, все остальные
   обращаются через IPC (Unix domain socket или shared memory)

2. **Жёсткое чередование TX/RX** — TDMA-like scheduling:
   - TX slot: отправить N пакетов
   - RX slot: принять пакеты (pcap_next_ex в цикле с timeout)
   - Гарантирует что radio не застревает в TX mode

3. **Приоритеты трафика**:
   - P0 (CRITICAL): CTRL — управление, всегда первым
   - P1 (HIGH):     TELEMETRY — телеметрия
   - P2 (NORMAL):   VIDEO — видео, заполняет оставшееся время
   - P3 (LOW):      BULK — OSD, прошивки, файлы

4. **Reliable send на уровне MUX** — не на уровне приложения.
   radiod сам решает когда делать retry, не блокируя приложения.

## Execution Plan

### Phase 1: radiod daemon (device side)

#### ~~Task 1.1: IPC протокол между приложениями и radiod~~ ✅ DONE

**Файлы**: `radiod/include/radiod/ipc.h`, `radiod/src/ipc.c`

IPC через Unix domain socket (SOCK_DGRAM):
- Каждое приложение (vcpd, ulamad) подключается к `/var/run/radiod.sock`
- Отправляет: `radio_tx_request_t { priority, reliability, ulama_frame[] }`
- Получает: `radio_rx_frame_t { ulama_frame, rssi, src_mac }`

```c
typedef struct {
    uint8_t priority;      // 0=CTRL, 1=TELEM, 2=VIDEO, 3=BULK
    uint8_t reliability;   // 0=unreliable, 1=reliable (ACK+retry)
    uint16_t payload_len;
    uint8_t payload[];     // packed ULAMA frame
} radio_tx_request_t;

typedef struct {
    int8_t rssi;
    uint8_t src_mac[6];
    uint16_t payload_len;
    uint8_t payload[];     // packed ULAMA frame
} radio_rx_frame_t;
```

Приложения фильтруют входящие фреймы по traffic_class самостоятельно.

#### ~~Task 1.2: TX Scheduler с приоритетами~~ ✅ DONE

**Файлы**: `radiod/src/tx_scheduler.c`

Три приоритетные очереди (lock-free ring buffers):
```
┌──────────────────────────────────┐
│ TX Scheduler                     │
│                                  │
│  P0 [CTRL] ─────→ ┐             │
│  P1 [TELEM] ────→ ├─→ pcap_inject│
│  P2 [VIDEO] ────→ ┘             │
│                                  │
│  Правило: P0 всегда первым.     │
│  P2 отправляется только если    │
│  P0 и P1 пусты.                 │
└──────────────────────────────────┘
```

Rate limiting:
- Max TX per TDMA slot: 4 пакета
- После TX slot: обязательный RX slot (минимум 2ms)
- CTRL пакеты обходят rate limit (всегда отправляются немедленно)

#### ~~Task 1.3: RX Dispatcher~~ ✅ DONE

**Файлы**: `radiod/src/rx_dispatcher.c`

RX slot: вызывает pcap_next_ex в цикле с коротким timeout (1ms).
Полученные фреймы:
1. ACK → обработать внутренне (async_ack_locked)
2. DATA_SEQ → отправить ACK, передать приложению через IPC
3. DATA → передать приложению через IPC

Dispatch по traffic_class:
- CTRL → ulamad socket
- VIDEO → vcpd socket (NACKs, UVCP)
- TELEMETRY → ulamad socket
- ALL → broadcast (если получатель не указан)

#### ~~Task 1.4: TDMA Scheduling Loop~~ ✅ DONE

**Файлы**: `radiod/src/main.c`

```c
while (running) {
    // 1. ВСЕГДА: отправить все CTRL пакеты (P0)
    flush_priority_queue(P0_CTRL);

    // 2. TX slot: отправить до N пакетов из P1/P2
    for (int i = 0; i < TX_SLOT_SIZE; i++) {
        pkt = dequeue_highest_priority();
        if (!pkt) break;
        pcap_inject(pkt);
        if (pkt.reliability == RELIABLE)
            store_async_slot(pkt);
    }

    // 3. RX slot: принимать пакеты с timeout
    rx_deadline = now_us() + RX_SLOT_US;
    while (now_us() < rx_deadline) {
        frame = pcap_next_ex(timeout=1ms);
        if (frame) {
            process_ack_if_needed(frame);
            dispatch_to_client(frame);
        }
    }

    // 4. Async retry: проверить таймауты, ретрансмиты
    async_tick();

    // 5. IPC: прочитать новые TX запросы от клиентов
    drain_ipc_requests();
}
```

Параметры TDMA:
- TX_SLOT_SIZE = 4 пакета (настраиваемый)
- RX_SLOT_US = 2000 мкс (2 мс приём)
- Цикл: ~3-5 мс (200-330 итераций/сек)

#### ~~Task 1.5: Адаптация vcpd~~ ✅ DONE

**Файл**: `vcpd/tools/vcpd.c`, `ulama/src/common/transport.c`

Новый transport backend: `ULAMA_TRANSPORT_KIND_RADIOD`
- `ulama_transport_tx_send()` → отправить TX request в radiod через IPC
- `ulama_transport_rx_recv()` → получить RX frame из radiod через IPC
- vcpd больше НЕ открывает pcap напрямую
- Reliable/unreliable решает radiod, не vcpd

#### ~~Task 1.6: Адаптация ulamad~~ ✅ DONE

**Файл**: `ulama/tools/ulamad.c`

Аналогично vcpd:
- `ulama_transport_tx_send()` → IPC к radiod
- `ulama_transport_rx_recv()` → IPC от radiod
- CTRL фреймы маркируются priority=0 → radiod отправляет первыми

### Phase 2: Улучшение ulama-gw (host side)

На хосте ulama-gw уже единственный radio процесс. Но нужно
добавить TDMA scheduling в его main loop.

#### ~~Task 2.1: TDMA в main loop ulama-gw~~ ✅ DONE

**Файл**: `ulama-gw/tools/ulama_gw.c`

Текущий main loop: poll → cascade_rx → ulama_rx
Новый main loop с TDMA:
```c
while (running) {
    // TX slot: cascade_rx (CTRL имеет приоритет)
    handle_cascade_rx_ctrl_only();  // только CTRL фреймы
    handle_cascade_rx_other();      // VIDEO/TELEM если время есть

    // RX slot: ulama_rx с жёстким timeout
    handle_ulama_rx_timed(RX_SLOT_MS);

    // Stats, expiry, etc.
}
```

#### ~~Task 2.2: Приоритизация cascade TX~~ ✅ DONE

Cascade-core отправляет и CTRL и VIDEO (NACKs) фреймы.
ulama-gw должен разделить их по приоритету:
- CTRL → немедленная отправка (reliable)
- VIDEO NACKs → отложенная отправка (unreliable, rate-limited)

### Phase 3: Мониторинг и безопасность

#### ~~Task 3.1: Control link watchdog~~ ✅ DONE

**Файл**: `radiod/src/watchdog.c`

Если CTRL фреймы не принимаются >2 сек:
1. Прекратить VIDEO TX полностью
2. Переключить radio в RX-only mode
3. Слать EMERGENCY beacon каждые 100ms
4. Log warning

Это FAILSAFE: если видео забило канал, radiod автоматически
освобождает его для восстановления управления.

#### ~~Task 3.2: Radio utilization metrics~~ ✅ DONE

Добавить в radiod stats:
- TX utilization % (время в TX / общее время)
- RX utilization % (время в RX / общее время)
- CTRL latency (время от получения cascade CTRL до pcap_inject)
- Queue depths per priority
- ACK success rate per class

### Phase 4: Mesh networking (future)

#### Task 4.1: Multi-node routing

radiod знает topology — может маршрутизировать фреймы через relay nodes.
TTL в ULAMA header уже поддерживается.

#### Task 4.2: Channel hopping

radiod может переключать WiFi channel при интерференции.
Координация через CTRL class beacon.

---

## Key Constants for TDMA Design

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| TX_SLOT_SIZE | 4 pkts | Не более 4 TX подряд → radio переключается на RX |
| RX_SLOT_US | 2000 µs | 2 мс приём → ловит ACK и CTRL фреймы |
| CTRL_MAX_LATENCY | 10 ms | CRSF допускает 20ms, берём с запасом |
| VIDEO_MAX_PPS | 40 | 25fps × ~1.5 pkt/frame = 37.5 pps |
| TELEM_MAX_PPS | 10 | MSP poll every 500ms = 2 pps + margin |
| TOTAL_MAX_PPS | 55 | CTRL(5) + TELEM(10) + VIDEO(40) |
| CYCLE_TIME_US | 4000 | TX(~2ms) + RX(2ms) = 4ms → 250 cycles/sec |

## Estimated Throughput

При TDMA с TX_SLOT_SIZE=4 и RX_SLOT=2ms:
- 250 cycles/sec × 4 pkt/cycle = 1000 pkt/sec max TX capacity
- VIDEO gets 40 pps out of 1000 = 4% utilization
- CTRL gets guaranteed 5 pps = always fits
- Headroom for retries: 1000 - 55 = 945 pps for retries

## Implementation Order

1. **Phase 1.4** — TDMA loop prototype (standalone, no IPC)
2. **Phase 1.1** — IPC protocol
3. **Phase 1.2 + 1.3** — TX scheduler + RX dispatcher
4. **Phase 1.5** — vcpd adaptation
5. **Phase 1.6** — ulamad adaptation
6. **Phase 2.1** — ulama-gw TDMA
7. **Phase 3.1** — Watchdog (SAFETY CRITICAL)
8. **Phase 3.2** — Monitoring

## Quick Win (before full refactor)

Пока radiod не готов, **немедленный фикс безопасности**:
- vcpd: `--reliable 0` (unreliable video, no ACK flood)
- ulama-gw: NACKs enabled (восстанавливает потери через NACK)
- Это убирает 175 TX/sec ACK-retry трафика с radio
