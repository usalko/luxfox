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

### Phase 4: Mesh relay

Relay mode: radiod пересылает пакеты, адресованные другим узлам.
ACK модель: **hop-by-hop** — каждый hop подтверждает приём на L2 (UNOW),
end-to-end гарантий нет. Для дронов это приемлемо: CTRL идёт с
reliable=1 на каждом hop, потери VIDEO восстанавливаются через NACK.

#### ~~Task 4.1: Relay engine в rx_dispatcher~~ ✅ DONE

**Файлы**: `radiod/src/rx_dispatcher.c`, `radiod/include/radiod/rx_dispatcher.h`

Сейчас rx_dispatcher слепо отдаёт ВСЕ RX фреймы в IPC broadcast.
Для relay нужно после парсинга UNOW payload распаковать ULAMA header
и принять решение:

```
RX frame → unpack ULAMA header → проверить dst_node:

  dst_node == my_node:
      → dispatch to IPC clients (как сейчас)

  dst_node == 0xFF (broadcast):
      → dispatch to IPC clients
      → relay: decrement TTL, set MESH_RELAY flag, re-enqueue в TX scheduler

  dst_node == другой узел:
      → НЕ dispatch локально
      → relay: decrement TTL, set MESH_RELAY flag, re-enqueue в TX scheduler
      → если TTL == 0 после декремента — drop (loop protection)
```

Добавить в `radio_rx_dispatcher_t`:
- `uint8_t own_node_id` — наш node_id для фильтрации dst_node
- `radio_tx_scheduler_t *relay_sched` — указатель на TX scheduler для relay
- `bool relay_enabled` — включение relay mode (по умолчанию off)

Relay пакет сохраняет исходный traffic_class → приоритет в TX scheduler.
Relay CTRL → P0, relay VIDEO → P2. Свой и чужой трафик конкурируют
в одних очередях — TDMA scheduler обеспечивает fairness.

#### ~~Task 4.2: ULAMA-level dedup~~ ✅ DONE

**Файлы**: `radiod/src/rx_dispatcher.c`

Текущий dedup работает по UNOW seq (2 bytes из DATA_SEQ subtype).
При relay один и тот же ULAMA frame приходит с РАЗНЫМИ unow_seq
(оригинал от A с seq=42, relay от B с seq=100).

Нужен второй dedup-слой по ключу `(src_node, ulama_seq)`:

```c
#define RADIO_ULAMA_DEDUP_WINDOW 128

typedef struct {
    uint8_t  src_node;
    uint16_t ulama_seq;
} radio_ulama_dedup_key_t;

radio_ulama_dedup_key_t ulama_dedup_ring[RADIO_ULAMA_DEDUP_WINDOW];
```

Проверяется ПОСЛЕ UNOW-level dedup, ПЕРЕД relay/dispatch решением.
Если `(src_node, seq)` уже видели — drop (не relay, не dispatch).

Без этого: broadcast storm при 3+ узлах (A→B→C→A→B→...).

#### ~~Task 4.3: Route table (auto-learning)~~ ✅ DONE

**Файлы**: `radiod/include/radiod/route_table.h`, `radiod/src/route_table.c`

Таблица маршрутизации для выбора next-hop MAC при relay TX:

```c
#define RADIO_MAX_ROUTES 32
#define RADIO_ROUTE_EXPIRE_US 30000000  /* 30 секунд без пакетов → expired */

typedef struct {
    uint8_t  dst_node;
    uint8_t  next_hop_mac[6];
    uint8_t  hop_count;
    int8_t   rssi;
    int64_t  last_seen_us;
    bool     active;
} radio_route_entry_t;

typedef struct {
    radio_route_entry_t entries[RADIO_MAX_ROUTES];
} radio_route_table_t;
```

Auto-learning: при каждом RX фрейме:
- `route[frame.src_node] = { next_hop = rx_src_mac, hops = 1, rssi }`
- Если фрейм с `MESH_RELAY` flag и оригинальный `src_node`:
  - `route[src_node] = { next_hop = relay_mac, hops = TTL_DEFAULT - frame.ttl + 1 }`

Выбор next-hop при TX:
- Есть route к `dst_node` → unicast на `next_hop_mac`
- Нет route → broadcast (0xff:ff:ff:ff:ff:ff)

Expire: удалять routes старше 30 секунд.

Route discovery (ROUTE_DISC flag) — НЕ реализуем на этом этапе,
auto-learning по RX трафику достаточно для star/chain topology.

#### ~~Task 4.4: Watchdog — фильтрация по dst_node~~ ✅ DONE

**Файлы**: `radiod/tools/radiod.c`

Сейчас watchdog кормится на ЛЮБОЙ RX активности. При relay через
узел идут CTRL фреймы ЧУЖИХ дронов — watchdog не должен на них
реагировать.

Изменение: кормить watchdog только когда получен CTRL фрейм
с `dst_node == my_node` или `dst_node == 0xFF` (broadcast).

Для этого rx_dispatcher при dispatch должен сообщать caller'у
traffic_class и dst_node полученного фрейма.

#### ~~Task 4.5: Relay metrics в stats~~ ✅ DONE

**Файлы**: `radiod/include/radiod/stats.h`, `radiod/src/stats.c`

Дополнительные счётчики для мониторинга relay:
- `relay_forwarded` — пакеты пересланные через нас
- `relay_dropped_ttl` — пакеты с TTL=0 (loop или слишком длинный путь)
- `relay_dropped_dedup` — дубликаты на ULAMA level
- `relay_by_prio[4]` — пересланные пакеты по классам
- `route_table_size` — количество известных маршрутов
- `route_table_expired` — маршруты удалённые по timeout

#### Task 4.6: Channel hopping (future, не в этой фазе)

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

1. ~~**Phase 1.4** — TDMA loop prototype~~ ✅
2. ~~**Phase 1.1** — IPC protocol~~ ✅
3. ~~**Phase 1.2 + 1.3** — TX scheduler + RX dispatcher~~ ✅
4. ~~**Phase 1.5** — vcpd adaptation~~ ✅
5. ~~**Phase 1.6** — ulamad adaptation~~ ✅
6. ~~**Phase 2.1** — ulama-gw TDMA~~ ✅
7. ~~**Phase 3.1** — Watchdog (SAFETY CRITICAL)~~ ✅
8. ~~**Phase 3.2** — Monitoring~~ ✅
9. **Phase 4.2** — ULAMA-level dedup (нужен ДО relay, иначе broadcast storm)
10. **Phase 4.3** — Route table (auto-learning)
11. **Phase 4.1** — Relay engine в rx_dispatcher
12. **Phase 4.4** — Watchdog фильтрация по dst_node
13. **Phase 4.5** — Relay metrics

## Key Constants for Mesh Relay

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| ULAMA_DEDUP_WINDOW | 128 | Больше чем UNOW (64), т.к. relay удваивает трафик |
| ROUTE_MAX_ENTRIES | 32 | До 32 узлов в сети |
| ROUTE_EXPIRE_US | 30 000 000 | 30 сек без пакетов → route expired |
| RELAY_MAX_HOPS | 8 | = ULAMA_FRAME_DEFAULT_TTL, макс. глубина цепочки |
| RELAY_REQUEUE_PRIO | same | Relay сохраняет исходный priority class |

## Mesh Topology Examples

```
Star (один relay):           Chain (два relay):
    Drone-A                    Drone-A
       ↕                         ↕
    Relay-B ←→ GW            Relay-B
       ↕                         ↕
    Drone-C                  Relay-C ←→ GW
```

При hop-by-hop ACK в chain topology:
- A→B: reliable (ACK from B) ✓
- B→C: reliable (ACK from C) ✓
- C→GW: reliable (ACK from GW) ✓
- Если B→C потерян: B retry, A не знает об этом
- Worst case: A считает "доставлено" хотя GW не получил
- Для CTRL допустимо: CRSF шлёт 150 Hz, потеря одного фрейма некритична

## Quick Win (before full refactor)

Пока radiod не готов, **немедленный фикс безопасности**:
- vcpd: `--reliable 0` (unreliable video, no ACK flood)
- ulama-gw: NACKs enabled (восстанавливает потери через NACK)
- Это убирает 175 TX/sec ACK-retry трафика с radio
