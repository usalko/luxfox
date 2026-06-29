# SYNC Protocol — проектный документ

## Версия: 1.0 | Дата: 2026-06-29

---

## 1. Постановка задачи

Текущий radiod работает в квази-TDMA режиме **без синхронизации между узлами**.
Каждый узел независимо крутит цикл `TX burst → RX window → IPC drain`.
При 2 узлах (дрон + земля) это работает за счёт деdup + async retry.
При 5 узлах коллизии растут квадратично — нужна координация.

**Цель**: централизованный TDMA с автоматическим выбором мастера,
NTP-подобной синхронизацией часов и ретрансляцией SYNC через mesh.

---

## 2. Архитектура

### 2.1 Роли узлов

Роль узла **не задаётся вручную** — определяется автоматически:

```
┌─────────────────────────────────────────────────┐
│              Bully Election Algorithm           │
│                                                 │
│  Правило: мастер = узел с наибольшим node_id    │
│                                                 │
│  Startup:  state = CANDIDATE                    │
│            election_timer = 500ms               │
│                                                 │
│  Получен SYNC от node X:                        │
│    X.node_id > own  →  state = SLAVE            │
│    X.node_id < own  →  игнорировать             │
│    X.node_id == own →  ошибка конфигурации      │
│                                                 │
│  election_timer истёк (нет SYNC от старших):    │
│    state = MASTER, начать отправку SYNC         │
│                                                 │
│  Slave потерял SYNC от мастера (timeout):       │
│    state = CANDIDATE, перезапуск выборов        │
│                                                 │
│  Master увидел SYNC от node_id > own:           │
│    state = SLAVE (уступить мастерство)          │
└─────────────────────────────────────────────────┘
```

**Конфликт двух мастеров** разрешается однозначно:
оба узла и все наблюдатели принимают мастера с большим node_id.
Узел с меньшим node_id, увидев чужой SYNC от старшего, немедленно
переходит в SLAVE. Никакого голосования, переговоров или раундов —
один SYNC-кадр решает всё.

### 2.2 Состояния узла (FSM)

```
              ┌──────────┐
   startup ──►│CANDIDATE │◄──── sync_timeout (slave)
              └────┬─────┘
                   │
          ┌────────┴────────┐
          │                 │
  election_timer     SYNC от старшего
  истёк (ни одного       получен
  старшего не слышно)     │
          │                │
          ▼                ▼
     ┌────────┐      ┌─────────┐
     │ MASTER │      │  SLAVE  │
     └───┬────┘      └────┬────┘
         │                │
    SYNC от               sync_timeout
    старшего node_id      (>3 пропущенных)
         │                │
         ▼                ▼
     ┌─────────┐    ┌──────────┐
     │  SLAVE  │    │CANDIDATE │
     └─────────┘    └──────────┘
```

### 2.3 Суперкадр (Superframe)

```
Мастер (node_id=5):

|SYNC|    DL (master→all)    |G| UL1 (node=1) |G| UL2 (node=2) |G| UL3 (node=3) |G| UL4 (node=4) |G|
 50µs      2000 µs           300  2000 µs      300  2000 µs      300  2000 µs      300  2000 µs     300
                              µs                µs                µs                µs               µs
└─────────────────────────────────────────────────────────────────────────────────────────────────────┘
                              Суперкадр ~12.25 мс (при 4 slave)
```

- **SYNC**: beacon от мастера с расписанием и timestamp
- **DL**: мастер шлёт свои данные (CTRL/TELEM/VIDEO/BULK)
- **G**: guard interval (TX/RX turnaround)
- **UL1..UL4**: строго выделенные слоты для каждого ведомого
- Ведомый, которому нечего передавать, шлёт **NULL frame** (1 байт)
  для подтверждения присутствия

### 2.4 NTP-подобная синхронизация часов

Используется схема IEEE 1588 PTP (Precision Time Protocol),
адаптированная под broadcast TDMA:

```
        Master                          Slave
          │                               │
          │──── SYNC (T1=master_tx) ─────►│  T2 = slave_rx
          │                               │
          │                               │  rough_offset = T2 - T1
          │                               │  (содержит one-way delay)
          │                               │
          │◄── DELAY_REQ (T3=slave_tx) ───│  (в UL слоте slave)
          │  T4 = master_rx               │
          │                               │
          │── next SYNC (+ DELAY_RESP) ──►│  slave получает T4
          │                               │
          │                               │  offset = ((T2-T1) - (T4-T3)) / 2
          │                               │  rtt    = (T2-T1) + (T4-T3)
          │                               │
```

**Формулы:**
```
clock_offset = ((T2 - T1) - (T4 - T3)) / 2
round_trip   = (T2 - T1) + (T4 - T3)
one_way_est  = round_trip / 2

# Конвертация local → master time:
master_time = local_time - clock_offset
```

**Фильтрация**: offset усредняется скользящим средним (EMA)
с коэффициентом α=0.125 (как в NTP), отбрасывая выбросы > 2σ.

### 2.5 SYNC relay (PTP Boundary Clock)

Для mesh-сети каждый промежуточный узел работает как **boundary clock**:

```
  Master (5)              Relay (3)              Leaf (1)
      │                       │                      │
      │── SYNC ──────────────►│                      │
      │   T1=100              │  T2_local=100.3      │
      │                       │  (offset к мастеру   │
      │                       │   уже вычислен)      │
      │                       │                      │
      │                       │── SYNC (retx) ──────►│
      │                       │   T1'=conv(T_tx)     │
      │                       │   relay_hops++       │
      │                       │   master_node_id=5   │
      │                       │                      │
```

Каждый relay:
1. Принимает SYNC, синхронизирует свои часы к мастеру
2. Ретранслирует SYNC с **новым T1'** = своё время TX, пересчитанное
   в шкалу мастера через свой offset
3. Инкрементирует `relay_hops`
4. Сохраняет `master_node_id` без изменений

Лист-узел синхронизируется к relay через свой DELAY_REQ/RESP.
Relay отвечает на DELAY_REQ в master-шкале.

**Dedup**: по (master_node_id, superframe_seq) — предотвращает петли.

---

## 3. Wire Protocol

### 3.1 Новые UNOW subtypes

Добавить в `unow_wire.h`:

```c
#define UNOW_VENDOR_SUBTYPE_SYNC       0x04U
#define UNOW_VENDOR_SUBTYPE_DELAY_REQ  0x05U
```

### 3.2 SYNC Frame Format (subtype 0x04)

```
Offset  Size  Field               Description
─────────────────────────────────────────────────────────────
 0      1     magic               0xBE — быстрый фильтр
 1      1     version             0x01
 2      1     master_node_id      node_id первоисточника SYNC
 3      1     sender_node_id      node_id узла, который отправил
                                  этот конкретный кадр (= master
                                  при прямом, = relay при ретрансляции)
 4      4     superframe_seq      uint32_t LE, монотонный счётчик
 8      8     origin_time_us      int64_t LE, T1 в шкале мастера
                                  (CLOCK_MONOTONIC мастера при прямой
                                  отправке; пересчитанное значение
                                  при ретрансляции)
16      2     dl_duration_us      uint16_t LE, DL окно
18      2     ul_slot_us          uint16_t LE, длительность одного UL
20      2     guard_us            uint16_t LE, guard interval
22      1     num_slots           кол-во UL слотов (0-4)
23      1     relay_hops          сколько relay-ов пройдено (0 = прямой)
24      4     slot_map[4]         node_id для UL0..UL3 (0x00 = unused)
28      1     num_delay_resp      кол-во DELAY_RESP записей (0-4)
29      N*9   delay_resp[]        массив DELAY_RESP:
                                    [0]    uint8_t  node_id
                                    [1..8] int64_t  t4_us (master RX time)
─────────────────────────────────────────────────────────────
Total: 29 + num_delay_resp * 9 bytes (29-65 bytes)
```

### 3.3 DELAY_REQ Frame Format (subtype 0x05)

```
Offset  Size  Field               Description
─────────────────────────────────────────────────────────────
 0      1     magic               0xBD — быстрый фильтр
 1      1     version             0x01
 2      1     requester_node_id   node_id запрашивающего slave
 3      1     target_node_id      node_id мастера (для валидации)
 4      8     t3_us               int64_t LE, CLOCK_MONOTONIC slave
                                  в момент отправки
12      4     superframe_seq      uint32_t LE, к какому суперкадру
─────────────────────────────────────────────────────────────
Total: 16 bytes
```

### 3.4 NULL Frame (presence heartbeat)

Slave шлёт в своём UL слоте если нет данных.
Обычный UNOW DATA (subtype 0x01), payload = 1 байт:

```
 0      1     0x00                NULL marker
```

radiod распознаёт по длине 1 + значению 0x00 и учитывает как heartbeat.

---

## 4. Модули (новые файлы)

### 4.1 `radiod/include/radiod/sync_frame.h`

Pack/unpack для SYNC и DELAY_REQ кадров.
Чистые функции без состояния — легко тестируются.

```c
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SYNC_FRAME_MAGIC      0xBEU
#define SYNC_FRAME_VERSION    0x01U
#define DELAY_REQ_MAGIC       0xBDU
#define DELAY_REQ_VERSION     0x01U

#define SYNC_MAX_SLOTS        4
#define SYNC_MAX_DELAY_RESP   4

#define SYNC_FRAME_MIN_SIZE   29
#define SYNC_FRAME_MAX_SIZE   (29 + SYNC_MAX_DELAY_RESP * 9)
#define DELAY_REQ_FRAME_SIZE  16

typedef struct {
    uint8_t  node_id;
    int64_t  t4_us;
} sync_delay_resp_t;

typedef struct {
    uint8_t  master_node_id;
    uint8_t  sender_node_id;
    uint32_t superframe_seq;
    int64_t  origin_time_us;
    uint16_t dl_duration_us;
    uint16_t ul_slot_us;
    uint16_t guard_us;
    uint8_t  num_slots;
    uint8_t  relay_hops;
    uint8_t  slot_map[SYNC_MAX_SLOTS];
    uint8_t  num_delay_resp;
    sync_delay_resp_t delay_resp[SYNC_MAX_DELAY_RESP];
} sync_frame_t;

typedef struct {
    uint8_t  requester_node_id;
    uint8_t  target_node_id;
    int64_t  t3_us;
    uint32_t superframe_seq;
} delay_req_frame_t;

bool sync_frame_pack(const sync_frame_t *in,
                     uint8_t *out, size_t capacity, size_t *out_len);
bool sync_frame_unpack(const uint8_t *in, size_t in_len,
                       sync_frame_t *out);

bool delay_req_pack(const delay_req_frame_t *in,
                    uint8_t *out, size_t capacity, size_t *out_len);
bool delay_req_unpack(const uint8_t *in, size_t in_len,
                      delay_req_frame_t *out);
```

### 4.2 `radiod/include/radiod/clock_sync.h`

NTP-подобный фильтр часов. Отдельный модуль — может использоваться
независимо от TDMA.

```c
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CLOCK_SYNC_HISTORY    8
#define CLOCK_SYNC_EMA_SHIFT  3       /* α = 1/8 = 0.125 */
#define CLOCK_SYNC_MAX_AGE_US 5000000 /* 5 sec — sync stale */

typedef struct {
    /* NTP timestamps */
    int64_t t1;  /* master TX (from SYNC) */
    int64_t t2;  /* local RX */
    int64_t t3;  /* local TX of DELAY_REQ */
    int64_t t4;  /* master RX of DELAY_REQ (from DELAY_RESP) */
    bool    t3t4_valid;  /* true after first DELAY_RESP received */
} clock_sync_sample_t;

typedef struct {
    int64_t  offset_us;       /* master_time = local_time + offset_us */
    int64_t  rtt_us;          /* round-trip time */
    int64_t  last_update_us;  /* local CLOCK_MONOTONIC of last update */
    bool     synced;          /* true after first valid offset */

    /* EMA filter state */
    int64_t  ema_offset;
    bool     ema_initialized;

    /* History for jitter estimation */
    int64_t  offset_history[CLOCK_SYNC_HISTORY];
    uint8_t  history_count;
    uint8_t  history_index;

    /* Pending DELAY_REQ state */
    int64_t  pending_t3;
    uint32_t pending_seq;
    bool     delay_req_pending;

    /* Source info */
    uint8_t  sync_source_node;
} clock_sync_t;

void clock_sync_init(clock_sync_t *cs);

/* Process a received SYNC frame: store T1, T2.
 * Returns rough offset (T2-T1) — usable immediately for
 * coarse timing, refined after DELAY_RESP. */
int64_t clock_sync_on_sync_rx(clock_sync_t *cs,
                               int64_t t1_master, int64_t t2_local,
                               uint8_t source_node);

/* Prepare DELAY_REQ: records T3. Returns superframe_seq to include. */
void clock_sync_prepare_delay_req(clock_sync_t *cs,
                                   int64_t t3_local,
                                   uint32_t superframe_seq);

/* Process DELAY_RESP: provides T4, computes refined offset. */
void clock_sync_on_delay_resp(clock_sync_t *cs,
                               int64_t t4_master);

/* Convert local time to master time */
int64_t clock_sync_to_master(const clock_sync_t *cs, int64_t local_us);

/* Convert master time to local time */
int64_t clock_sync_to_local(const clock_sync_t *cs, int64_t master_us);

/* Is the sync still fresh? */
bool clock_sync_is_valid(const clock_sync_t *cs, int64_t local_now_us);
```

### 4.3 `radiod/include/radiod/sync.h`

Главный модуль: FSM состояний, выборы мастера, TDMA-цикл.

```c
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "radiod/clock_sync.h"
#include "radiod/sync_frame.h"

#define SYNC_ELECTION_TIMEOUT_US   500000   /* 500 ms */
#define SYNC_BEACON_INTERVAL_US    12000    /* 12 ms — superframe period */
#define SYNC_MISS_THRESHOLD        3        /* CANDIDATE after 3 misses */
#define SYNC_LOST_THRESHOLD        10       /* LINK_LOST after 10 misses */
#define SYNC_DEDUP_WINDOW          32
#define SYNC_MAX_NODES             5

typedef enum {
    RADIO_ROLE_CANDIDATE = 0,
    RADIO_ROLE_MASTER,
    RADIO_ROLE_SLAVE,
} radio_role_t;

typedef enum {
    SYNC_STATE_SEARCHING = 0,  /* CANDIDATE: ждём/ищем beacon   */
    SYNC_STATE_SYNCED,         /* SLAVE: синхронизированы        */
    SYNC_STATE_MASTER_TX,      /* MASTER: шлём DL               */
    SYNC_STATE_MASTER_RX,      /* MASTER: слушаем UL            */
} sync_state_t;

/* Known slave info (master-side) */
typedef struct {
    uint8_t  node_id;
    int64_t  last_seen_us;     /* last UL frame or NULL from this slave */
    bool     active;
    /* DELAY_RESP pending */
    int64_t  delay_req_t4;     /* T4: master RX time of DELAY_REQ */
    bool     delay_resp_pending;
} sync_slave_info_t;

/* SYNC dedup ring */
typedef struct {
    uint8_t  master_node_id;
    uint32_t superframe_seq;
} sync_dedup_key_t;

typedef struct {
    /* Identity */
    uint8_t          own_node_id;

    /* FSM */
    radio_role_t     role;
    sync_state_t     state;

    /* Election */
    int64_t          election_deadline_us;
    uint8_t          current_master_id;

    /* Master state */
    uint32_t         superframe_seq;
    sync_slave_info_t slaves[SYNC_MAX_NODES];
    uint8_t          num_known_slaves;

    /* Slave state */
    clock_sync_t     clock;
    uint8_t          missed_beacons;
    int64_t          last_sync_rx_us;
    int64_t          next_superframe_us;  /* expected start of next SF */

    /* Computed slot timing (absolute local clock) */
    int64_t          dl_start_us;
    int64_t          dl_end_us;
    int64_t          my_ul_start_us;
    int64_t          my_ul_end_us;
    uint8_t          my_slot_index;       /* 0xFF = no slot assigned */

    /* Schedule (from last SYNC or self-generated) */
    uint16_t         dl_duration_us;
    uint16_t         ul_slot_us;
    uint16_t         guard_us;
    uint8_t          num_slots;
    uint8_t          slot_map[SYNC_MAX_SLOTS];

    /* SYNC relay dedup */
    sync_dedup_key_t dedup_ring[SYNC_DEDUP_WINDOW];
    uint16_t         dedup_head;
    uint16_t         dedup_count;

    /* Stats */
    uint32_t         sync_tx_count;
    uint32_t         sync_rx_count;
    uint32_t         sync_relay_count;
    uint32_t         delay_req_tx_count;
    uint32_t         delay_resp_rx_count;
    uint32_t         elections_won;
    uint32_t         elections_lost;
    uint32_t         role_changes;
} radio_sync_t;

/* ---- Lifecycle ---- */

void radio_sync_init(radio_sync_t *s, uint8_t own_node_id,
                     uint16_t dl_us, uint16_t ul_us, uint16_t guard_us);

/* ---- Event handlers ---- */

/* Вызывается при получении SYNC кадра.
 * Возвращает true если кадр нужно ретранслировать. */
bool radio_sync_on_sync_rx(radio_sync_t *s,
                           const sync_frame_t *frame,
                           int64_t local_rx_us,
                           uint8_t sender_node_id);

/* Вызывается при получении DELAY_REQ (master-side). */
void radio_sync_on_delay_req_rx(radio_sync_t *s,
                                const delay_req_frame_t *dreq,
                                int64_t local_rx_us);

/* ---- Master actions ---- */

/* Собрать SYNC beacon для отправки.
 * Заполняет frame и delay_resp секцию. */
void radio_sync_build_beacon(radio_sync_t *s,
                              sync_frame_t *out_frame,
                              int64_t now_us);

/* Обновить slot_map: назначить слоты известным slave. */
void radio_sync_update_slot_map(radio_sync_t *s);

/* ---- Slave actions ---- */

/* Рассчитать абсолютные границы слотов (мой UL) из последнего SYNC.
 * Должна вызываться после radio_sync_on_sync_rx. */
void radio_sync_compute_timing(radio_sync_t *s, int64_t local_now_us);

/* Собрать DELAY_REQ для отправки.
 * Returns false если delay_req ещё не нужен. */
bool radio_sync_build_delay_req(radio_sync_t *s,
                                 delay_req_frame_t *out,
                                 int64_t now_us);

/* ---- Tick ---- */

/* Вызывается каждый цикл. Обновляет FSM, проверяет таймауты.
 * Возвращает текущую роль. */
radio_role_t radio_sync_tick(radio_sync_t *s, int64_t now_us);

/* ---- Relay ---- */

/* Подготовить SYNC для ретрансляции: пересчитать origin_time_us
 * через свой clock offset, инкрементировать relay_hops,
 * подставить sender_node_id = own. */
bool radio_sync_prepare_relay(radio_sync_t *s,
                               const sync_frame_t *rx_frame,
                               sync_frame_t *relay_frame,
                               int64_t local_tx_us);

/* ---- Queries ---- */

radio_role_t radio_sync_get_role(const radio_sync_t *s);
bool radio_sync_is_synced(const radio_sync_t *s);
int64_t radio_sync_get_offset(const radio_sync_t *s);
```

---

## 5. Изменения в существующих файлах

### 5.1 `unow/include/unow/unow_wire.h`

```diff
+#define UNOW_VENDOR_SUBTYPE_SYNC       0x04U
+#define UNOW_VENDOR_SUBTYPE_DELAY_REQ  0x05U
```

### 5.2 `unow/src/unow_radiotap.c` → `unow_parse_action_frame()`

Добавить SYNC и DELAY_REQ в список допустимых subtypes:

```diff
  if (vendor_header->subtype != UNOW_VENDOR_SUBTYPE_DATA &&
      vendor_header->subtype != UNOW_VENDOR_SUBTYPE_ACK &&
-     vendor_header->subtype != UNOW_VENDOR_SUBTYPE_DATA_SEQ) {
+     vendor_header->subtype != UNOW_VENDOR_SUBTYPE_DATA_SEQ &&
+     vendor_header->subtype != UNOW_VENDOR_SUBTYPE_SYNC &&
+     vendor_header->subtype != UNOW_VENDOR_SUBTYPE_DELAY_REQ) {
      return false;
  }
```

### 5.3 `radiod/src/rx_dispatcher.c` → `radio_rx_slot()`

Перед обработкой ACK/DATA/DATA_SEQ добавить ветку для SYNC/DELAY_REQ:

```c
/* SYNC frame → delegate to sync engine */
if (frame.subtype == UNOW_VENDOR_SUBTYPE_SYNC) {
    rxd->stats.rx_sync++;
    /* Pass to sync engine (via callback or direct pointer) */
    if (rxd->sync_ctx != NULL)
        radio_rx_handle_sync(rxd, &frame, now_us());
    continue;
}

/* DELAY_REQ → master processes */
if (frame.subtype == UNOW_VENDOR_SUBTYPE_DELAY_REQ) {
    rxd->stats.rx_delay_req++;
    if (rxd->sync_ctx != NULL)
        radio_rx_handle_delay_req(rxd, &frame, now_us());
    continue;
}
```

### 5.4 `radiod/tools/radiod.c` → main loop

Текущий TDMA-цикл (строки 434-560) разветвляется по роли:

```c
while (g_running) {
    /* ... pcap recovery (без изменений) ... */

    radio_role_t role = radio_sync_tick(&sync, now_us());

    switch (role) {
    case RADIO_ROLE_MASTER:
        master_cycle(&sync, &sched, &rxd, pcap_handle, ...);
        break;
    case RADIO_ROLE_SLAVE:
        slave_cycle(&sync, &sched, &rxd, pcap_handle, ...);
        break;
    case RADIO_ROLE_CANDIDATE:
        candidate_cycle(&sync, &rxd, pcap_handle, ...);
        break;
    }

    /* IPC drain, watchdog, stats — общие для всех ролей */
    radio_ipc_drain(&ipc, on_ipc_tx_request, &ipc_ctx);
    /* ... */
}
```

### 5.5 `radiod/include/radiod/watchdog.h`

Расширить для учёта SYNC:

```c
/* Вызывать когда slave получил SYNC (heartbeat мастера) */
void radio_watchdog_feed_sync(radio_watchdog_t *wd, int64_t now_us);
```

Логика: SYNC beacon от мастера = implicit CTRL heartbeat.
Если slave теряет SYNC → LINK_DEGRADED → LINK_LOST.

### 5.6 `radiod/include/radiod/stats.h`

Добавить sync-метрики:

```c
/* SYNC stats */
uint32_t sync_tx;
uint32_t sync_rx;
uint32_t sync_relay;
uint32_t delay_req_tx;
uint32_t delay_resp_rx;
int64_t  clock_offset_us;
int64_t  clock_rtt_us;
uint8_t  current_role;     /* 0=candidate, 1=master, 2=slave */
uint8_t  current_master;   /* master node_id */
```

### 5.7 `radiod/Makefile`

Новые .c файлы (sync_frame.c, clock_sync.c, sync.c) автоматически
подхватываются через `$(wildcard $(CURRENT_DIR)/src/*.c)`.
Новые тесты — через `$(wildcard $(CURRENT_DIR)/tests/test_*.c)`.
Изменения в Makefile не нужны.

---

## 6. Циклы работы (подробно)

### 6.1 CANDIDATE cycle

```c
static void candidate_cycle(radio_sync_t *sync, ...)
{
    /* Только слушаем эфир, ищем SYNC от кого-либо.
     * Если election_timer истёк — становимся мастером.
     * Длительность: фиксированная (например 5 мс RX). */

    int64_t rx_deadline = now_us() + 5000; /* 5 мс */
    radio_rx_slot(&rxd, pcap_handle, own_mac, rx_deadline);

    /* radio_sync_tick() проверит таймер и сменит роль */
}
```

### 6.2 MASTER cycle

```c
static void master_cycle(radio_sync_t *sync, ...)
{
    int64_t t_now = now_us();

    /* 1. Обновить slot map */
    radio_sync_update_slot_map(sync);

    /* 2. Построить и отправить SYNC beacon */
    sync_frame_t beacon;
    radio_sync_build_beacon(sync, &beacon, t_now);
    /* ... pack → build_action_frame_ex(SYNC) → pcap_inject ... */

    /* 3. DL слот: отправить свои данные */
    int64_t dl_deadline = now_us() + sync->dl_duration_us;
    while (now_us() < dl_deadline) {
        /* Flush CTRL first, then P1/P2/P3 */
        const radio_tx_slot_t *slot = radio_tx_dequeue(&sched, &prio);
        if (!slot) break;
        tx_inject_slot(slot, ...);
    }
    /* Досып до конца DL окна */
    sleep_until(dl_deadline);

    /* 4. Guard */
    usleep(sync->guard_us);

    /* 5. UL слоты: приём от каждого slave */
    for (uint8_t i = 0; i < sync->num_slots; i++) {
        int64_t ul_deadline = now_us() + sync->ul_slot_us;
        radio_rx_slot(&rxd, pcap_handle, own_mac, ul_deadline);
        usleep(sync->guard_us);
    }

    /* 6. Async retry, IPC drain */
    radio_async_tick(&rxd, pcap_handle, ...);
}
```

### 6.3 SLAVE cycle

```c
static void slave_cycle(radio_sync_t *sync, ...)
{
    /* 1. Ожидание SYNC beacon */
    int64_t sync_deadline = sync->next_superframe_us + 2000;
    /*    (ждём чуть дольше ожидаемого момента) */
    radio_rx_slot(&rxd, pcap_handle, own_mac, sync_deadline);

    if (!radio_sync_is_synced(sync)) {
        /* Пропущен beacon — sync_tick обработает */
        return;
    }

    /* 2. DL фаза: только приём (данные от мастера) */
    radio_rx_slot(&rxd, pcap_handle, own_mac, sync->dl_end_us);

    /* 3. Ожидание своего UL слота */
    if (sync->my_slot_index != 0xFF) {
        sleep_until(sync->my_ul_start_us);

        /* 4. UL фаза: отправка своих данных */
        int64_t ul_deadline = sync->my_ul_end_us;

        /* 4a. DELAY_REQ первым (для NTP sync) */
        delay_req_frame_t dreq;
        if (radio_sync_build_delay_req(sync, &dreq, now_us())) {
            /* ... pack → build_action_frame_ex(DELAY_REQ)
             *     → pcap_inject ... */
        }

        /* 4b. Данные */
        while (now_us() < ul_deadline) {
            const radio_tx_slot_t *slot = radio_tx_dequeue(&sched, &prio);
            if (!slot) {
                /* NULL frame — heartbeat */
                uint8_t null_byte = 0x00;
                /* ... inject null_byte ... */
                break;
            }
            tx_inject_slot(slot, ...);
        }
    }

    /* 5. Ожидание конца суперкадра (RX — слушаем остальные UL) */
    int64_t sf_end = sync->next_superframe_us;
    radio_rx_slot(&rxd, pcap_handle, own_mac, sf_end);
}
```

---

## 7. Сценарии

### 7.1 Холодный старт 5 узлов

```
t=0ms    Все узлы: CANDIDATE, election_timer=500ms
         Все слушают эфир

t=500ms  Таймер истёк у всех почти одновременно
         Все начинают слать SYNC

t=500ms  Node 5 (НСУ) шлёт SYNC(master_id=5)
         Node 4 шлёт SYNC(master_id=4)
         Node 3 шлёт SYNC(master_id=3)
         ...

t=501ms  Node 4 получает SYNC(master_id=5) → SLAVE
         Node 3 получает SYNC(master_id=5) → SLAVE (через relay или прямо)
         Node 1 получает SYNC(master_id=5) → SLAVE

t=501ms  Все увидели node_id=5 > свой → все стали SLAVE
         Node 5 не видит SYNC от старших → остаётся MASTER
         Конфликт разрешён за 1 мс
```

### 7.2 Потеря мастера

```
t=0      Node 5 (MASTER) шлёт SYNC каждые 12 мс
t=100ms  Node 5 отключился (батарея/USB/crash)
t=136ms  Slaves пропустили 3 SYNC → CANDIDATE
t=636ms  election_timer: Node 4 (наибольший из оставшихся) → MASTER
         Node 1-3 получают SYNC(master_id=4) → SLAVE
         Восстановление за ~600 мс
```

### 7.3 Возвращение старого мастера

```
t=0      Node 4 = MASTER (после потери Node 5)
t=5s     Node 5 перезагрузился, CANDIDATE
t=5.5s   Node 5 не слышит старших → MASTER
         Node 5 шлёт SYNC(master_id=5)
t=5.5s   Node 4 получает SYNC(5) — 5 > 4 → SLAVE
         Все остальные тоже → SLAVE к node 5
         Бесшовная смена мастера
```

---

## 8. Подробный TODO для реализации

Задачи пронумерованы в порядке зависимостей.
Каждая задача — один коммит или один PR.

---

### ФАЗА 1: Wire Protocol (sync_frame)

#### TODO 1.1: Создать `radiod/include/radiod/sync_frame.h`

**Файл**: `radiod/include/radiod/sync_frame.h`

Определить структуры:
- `sync_frame_t` — SYNC beacon (см. секцию 4.1)
- `delay_req_frame_t` — DELAY_REQ
- `sync_delay_resp_t` — встроенный DELAY_RESP

Определить константы:
- `SYNC_FRAME_MAGIC 0xBE`
- `SYNC_FRAME_VERSION 0x01`
- `DELAY_REQ_MAGIC 0xBD`
- `DELAY_REQ_VERSION 0x01`
- `SYNC_MAX_SLOTS 4`
- `SYNC_MAX_DELAY_RESP 4`
- `SYNC_FRAME_MIN_SIZE 29`
- `SYNC_FRAME_MAX_SIZE (29 + 4*9)` = 65
- `DELAY_REQ_FRAME_SIZE 16`

Объявить 4 функции: pack/unpack для обеих структур.

#### TODO 1.2: Реализовать `radiod/src/sync_frame.c`

**Файл**: `radiod/src/sync_frame.c`

Реализовать `sync_frame_pack()`:
- Записать magic, version, master_node_id, sender_node_id
- Записать superframe_seq как uint32_t LE (4 байта)
- Записать origin_time_us как int64_t LE (8 байт)
- Записать dl_duration_us, ul_slot_us, guard_us как uint16_t LE
- Записать num_slots, relay_hops
- Записать slot_map[4]
- Записать num_delay_resp
- Для каждого delay_resp: записать node_id (1 байт) + t4_us (8 байт LE)
- Вернуть true и записать out_len

Реализовать `sync_frame_unpack()`:
- Проверить in_len >= SYNC_FRAME_MIN_SIZE
- Проверить magic == 0xBE, version == 0x01
- Прочитать все поля в обратном порядке pack
- Проверить num_delay_resp <= SYNC_MAX_DELAY_RESP
- Проверить in_len >= 29 + num_delay_resp * 9
- Прочитать delay_resp массив
- Вернуть true

Реализовать `delay_req_pack()`:
- 16 байт: magic, version, requester, target, t3_us (8 LE), superframe_seq (4 LE)

Реализовать `delay_req_unpack()`:
- Проверить in_len >= 16, magic == 0xBD, version == 0x01
- Прочитать все поля

**Важно**: все multi-byte поля — Little Endian. Использовать memcpy для
выравнивания (ARM unaligned access может быть медленным).

Вспомогательные функции (static):
```c
static void write_le16(uint8_t *p, uint16_t v);
static void write_le32(uint8_t *p, uint32_t v);
static void write_le64(uint8_t *p, int64_t v);
static uint16_t read_le16(const uint8_t *p);
static uint32_t read_le32(const uint8_t *p);
static int64_t  read_le64(const uint8_t *p);
```

#### TODO 1.3: Unit test для sync_frame

**Файл**: `radiod/tests/test_sync_frame.c`

Тесты:
1. `test_sync_pack_unpack_roundtrip` — pack → unpack, сравнить все поля
2. `test_sync_pack_with_delay_resp` — с 1-4 DELAY_RESP записями
3. `test_sync_pack_no_delay_resp` — num_delay_resp=0
4. `test_sync_unpack_short_buffer` — in_len < MIN_SIZE → false
5. `test_sync_unpack_bad_magic` — magic != 0xBE → false
6. `test_sync_unpack_bad_version` — version != 0x01 → false
7. `test_sync_unpack_truncated_delay_resp` — заявлено 3, но данных на 1
8. `test_delay_req_pack_unpack_roundtrip`
9. `test_delay_req_unpack_short` — < 16 байт → false
10. `test_endianness` — проверить конкретные байты packed буфера

Собирается через существующий `$(HOST_TEST_BINS)` в Makefile.

---

### ФАЗА 2: Clock Sync (NTP-like)

#### TODO 2.1: Создать `radiod/include/radiod/clock_sync.h`

**Файл**: `radiod/include/radiod/clock_sync.h`

Структуры и API — см. секцию 4.2.

#### TODO 2.2: Реализовать `radiod/src/clock_sync.c`

**Файл**: `radiod/src/clock_sync.c`

`clock_sync_init()`:
- Обнулить всё, synced=false

`clock_sync_on_sync_rx(t1_master, t2_local, source_node)`:
- Сохранить t1, t2 в текущий sample
- rough_offset = t2 - t1 (с учётом one-way delay)
- Если ранее получен DELAY_RESP (t3t4_valid):
  offset_refined = ((t2-t1) - (t4-t3)) / 2
- Иначе: offset = rough_offset (первое приближение)
- Обновить EMA:
  ```c
  if (!ema_initialized) {
      ema_offset = offset;
      ema_initialized = true;
  } else {
      ema_offset = ema_offset + (offset - ema_offset) / 8;
  }
  ```
- Записать в history для jitter оценки
- offset_us = ema_offset
- last_update_us = t2_local
- synced = true
- Вернуть rough_offset

`clock_sync_prepare_delay_req(t3_local, superframe_seq)`:
- pending_t3 = t3_local
- pending_seq = superframe_seq
- delay_req_pending = true

`clock_sync_on_delay_resp(t4_master)`:
- offset = ((t2-t1) - (t4-t3)) / 2 с сохранёнными t1, t2, t3
- rtt = (t2-t1) + (t4-t3)
- Обновить EMA
- delay_req_pending = false

`clock_sync_to_master(local_us)`:
- return local_us + offset_us

`clock_sync_to_local(master_us)`:
- return master_us - offset_us

`clock_sync_is_valid(local_now_us)`:
- return synced && (local_now_us - last_update_us < CLOCK_SYNC_MAX_AGE_US)

#### TODO 2.3: Unit test для clock_sync

**Файл**: `radiod/tests/test_clock_sync.c`

Тесты:
1. `test_initial_rough_offset` — без DELAY_RESP, только T1/T2
2. `test_refined_offset_symmetric` — симметричная задержка 500µs:
   T1=0, T2=500, T3=1000, T4=1500 → offset=0, rtt=1000
3. `test_refined_offset_asymmetric` — асимметрия:
   T1=0, T2=300, T3=1000, T4=1700 → offset=-200, rtt=1000
4. `test_ema_converges` — серия сэмплов, offset сходится
5. `test_ema_rejects_outliers` — выброс не ломает фильтр
   (TODO: добавить outlier rejection если нужно)
6. `test_to_master_to_local_roundtrip` — convert(convert(x)) ≈ x
7. `test_is_valid_expiry` — через 5 сек → не valid
8. `test_multiple_sync_sources` — смена source_node сбрасывает EMA?

---

### ФАЗА 3: Sync Engine (FSM + Election)

#### TODO 3.1: Создать `radiod/include/radiod/sync.h`

**Файл**: `radiod/include/radiod/sync.h`

Структуры и API — см. секцию 4.3.

#### TODO 3.2: Реализовать `radiod/src/sync.c`

**Файл**: `radiod/src/sync.c`

Подробно по функциям:

**`radio_sync_init(s, own_node_id, dl_us, ul_us, guard_us)`**:
- Обнулить всю структуру
- own_node_id = own_node_id
- role = RADIO_ROLE_CANDIDATE
- state = SYNC_STATE_SEARCHING
- election_deadline_us = 0 (будет установлен при первом tick)
- dl_duration_us = dl_us, ul_slot_us = ul_us, guard_us = guard_us
- my_slot_index = 0xFF
- clock_sync_init(&s->clock)
- current_master_id = 0

**`radio_sync_tick(s, now_us)`**:
```
switch (role):
  CANDIDATE:
    if election_deadline_us == 0:
        election_deadline_us = now_us + SYNC_ELECTION_TIMEOUT_US
    if now_us >= election_deadline_us:
        role = MASTER
        state = SYNC_STATE_MASTER_TX
        current_master_id = own_node_id
        superframe_seq = 0
        elections_won++
        role_changes++
        log "became MASTER"
    return role

  MASTER:
    (ничего особого — master cycle управляется из radiod.c)
    return role

  SLAVE:
    elapsed = now_us - last_sync_rx_us
    expected_interval = dl_duration_us + num_slots*(ul_slot_us+guard_us) + guard_us
    if elapsed > expected_interval * SYNC_MISS_THRESHOLD:
        missed_beacons++
        if missed_beacons >= SYNC_LOST_THRESHOLD:
            role = CANDIDATE
            state = SYNC_STATE_SEARCHING
            election_deadline_us = now_us + SYNC_ELECTION_TIMEOUT_US
            current_master_id = 0
            role_changes++
            log "SYNC lost, back to CANDIDATE"
    return role
```

**`radio_sync_on_sync_rx(s, frame, local_rx_us, sender_node_id)`**:
```
// Dedup check
if dedup_check(master_node_id, superframe_seq):
    return false  // уже видели

// Master election: frame->master_node_id vs own_node_id
if frame->master_node_id > own_node_id:
    // Старший мастер — подчиняемся
    if role == MASTER:
        log "yielding to node %u", frame->master_node_id
        elections_lost++
        role_changes++
    role = SLAVE
    state = SYNC_STATE_SYNCED
    current_master_id = frame->master_node_id
    missed_beacons = 0
    last_sync_rx_us = local_rx_us

    // NTP: T1 = frame->origin_time_us, T2 = local_rx_us
    clock_sync_on_sync_rx(&clock, frame->origin_time_us, local_rx_us,
                          sender_node_id)

    // Проверить DELAY_RESP для нас
    for (i = 0; i < frame->num_delay_resp; i++):
        if frame->delay_resp[i].node_id == own_node_id:
            clock_sync_on_delay_resp(&clock, frame->delay_resp[i].t4_us)
            delay_resp_rx_count++

    // Сохранить расписание
    dl_duration_us = frame->dl_duration_us
    ul_slot_us = frame->ul_slot_us
    guard_us = frame->guard_us
    num_slots = frame->num_slots
    memcpy(slot_map, frame->slot_map, 4)

    // Найти свой слот
    my_slot_index = 0xFF
    for (i = 0; i < num_slots; i++):
        if slot_map[i] == own_node_id:
            my_slot_index = i
            break

    sync_rx_count++
    return true  // нужно ретранслировать

elif frame->master_node_id < own_node_id:
    // Младший мастер — игнорируем его SYNC, не ретранслируем
    return false

elif frame->master_node_id == own_node_id:
    // Наш собственный SYNC вернулся (через relay) — игнорируем
    return false
```

**`radio_sync_on_delay_req_rx(s, dreq, local_rx_us)`**:
```
if role != MASTER: return
if dreq->target_node_id != own_node_id: return

// Найти или создать запись slave
for i in slaves:
    if slaves[i].node_id == dreq->requester_node_id:
        slaves[i].delay_req_t4 = local_rx_us
        slaves[i].delay_resp_pending = true
        return

// Новый slave
for i in slaves:
    if !slaves[i].active:
        slaves[i].node_id = dreq->requester_node_id
        slaves[i].active = true
        slaves[i].delay_req_t4 = local_rx_us
        slaves[i].delay_resp_pending = true
        slaves[i].last_seen_us = local_rx_us
        num_known_slaves++
        return
```

**`radio_sync_build_beacon(s, out_frame, now_us)`**:
```
out_frame->master_node_id = own_node_id
out_frame->sender_node_id = own_node_id
out_frame->superframe_seq = ++superframe_seq
out_frame->origin_time_us = now_us  // T1
out_frame->dl_duration_us = dl_duration_us
out_frame->ul_slot_us = ul_slot_us
out_frame->guard_us = guard_us
out_frame->num_slots = num_slots
out_frame->relay_hops = 0
memcpy(out_frame->slot_map, slot_map, 4)

// Собрать DELAY_RESP для slave-ов с pending
out_frame->num_delay_resp = 0
for i in slaves:
    if slaves[i].delay_resp_pending && out_frame->num_delay_resp < 4:
        out_frame->delay_resp[n].node_id = slaves[i].node_id
        out_frame->delay_resp[n].t4_us = slaves[i].delay_req_t4
        out_frame->num_delay_resp++
        slaves[i].delay_resp_pending = false
```

**`radio_sync_update_slot_map(s)`**:
```
// Назначить слоты активным slave-ам (отсортированным по node_id)
// Сначала собрать список активных
uint8_t active_ids[SYNC_MAX_NODES]
int n_active = 0
for i in slaves:
    if slaves[i].active && now - slaves[i].last_seen < 5*superframe_period:
        active_ids[n_active++] = slaves[i].node_id

// Сортировать по node_id (детерминизм)
sort(active_ids, n_active)

// Назначить (максимум SYNC_MAX_SLOTS)
memset(slot_map, 0, sizeof(slot_map))
num_slots = min(n_active, SYNC_MAX_SLOTS)
for i = 0..num_slots-1:
    slot_map[i] = active_ids[i]
```

**`radio_sync_compute_timing(s, local_now_us)`**:
```
// SYNC пришёл в last_sync_rx_us
// DL начинается сразу после SYNC
dl_start_us = last_sync_rx_us  // (SYNC уже принят)
dl_end_us = dl_start_us + dl_duration_us

// UL слоты начинаются после DL + guard
int64_t ul_base = dl_end_us + guard_us
for i = 0..num_slots-1:
    int64_t slot_start = ul_base + i * (ul_slot_us + guard_us)
    if slot_map[i] == own_node_id:
        my_ul_start_us = slot_start
        my_ul_end_us = slot_start + ul_slot_us
        my_slot_index = i

// Следующий суперкадр
next_superframe_us = ul_base + num_slots * (ul_slot_us + guard_us) + guard_us
```

**`radio_sync_build_delay_req(s, out, now_us)`**:
```
if role != SLAVE: return false
if !clock.synced: return true  // нужен DELAY_REQ для первой синхронизации
if clock.delay_req_pending: return false  // ждём ответ на предыдущий

out->requester_node_id = own_node_id
out->target_node_id = current_master_id
out->t3_us = now_us
out->superframe_seq = ... // текущий superframe_seq

clock_sync_prepare_delay_req(&clock, now_us, out->superframe_seq)
delay_req_tx_count++
return true
```

**`radio_sync_prepare_relay(s, rx_frame, relay_frame, local_tx_us)`**:
```
if role != SLAVE: return false
if !clock.synced: return false  // не можем пересчитать время

// Копировать все поля
*relay_frame = *rx_frame

// Пересчитать origin_time_us: конвертировать local_tx_us в master time
relay_frame->origin_time_us = clock_sync_to_master(&clock, local_tx_us)
relay_frame->sender_node_id = own_node_id
relay_frame->relay_hops = rx_frame->relay_hops + 1

// Очистить delay_resp (relay не ретранслирует чужие DELAY_RESP)
relay_frame->num_delay_resp = 0

return true
```

#### TODO 3.3: Unit test для sync engine

**Файл**: `radiod/tests/test_sync.c`

Тесты:
1. `test_init_candidate` — начальное состояние = CANDIDATE
2. `test_election_timeout_become_master` — tick после 500ms → MASTER
3. `test_higher_sync_become_slave` — получен SYNC(node=10), own=5 → SLAVE
4. `test_lower_sync_ignored` — получен SYNC(node=3), own=5 → всё ещё CANDIDATE/MASTER
5. `test_equal_sync_ignored` — SYNC(node=5), own=5 → игнор (наш echo)
6. `test_master_yields_to_higher` — мастер (5) получает SYNC(10) → SLAVE
7. `test_slave_miss_threshold` — 3 пропущенных SYNC → CANDIDATE
8. `test_slave_lost_threshold` — 10 пропущенных → перевыборы
9. `test_slot_map_assignment` — 3 active slaves → slot_map = [1,2,3,0]
10. `test_compute_timing_slot0` — проверить my_ul_start для slot 0
11. `test_compute_timing_slot2` — проверить my_ul_start для slot 2
12. `test_compute_timing_no_slot` — own не в slot_map → my_slot_index=0xFF
13. `test_dedup_rejects_duplicate_sync` — тот же (master, seq) → false
14. `test_dedup_passes_new_seq` — новый seq → true
15. `test_delay_req_generation` — slave генерирует DELAY_REQ
16. `test_delay_resp_in_beacon` — master включает T4 в SYNC
17. `test_relay_prepare` — origin_time пересчитан, hops++, sender=own
18. `test_role_change_counter` — каждая смена роли инкрементирует
19. `test_cold_start_5_nodes` — симуляция 5 узлов, проверить что node_id=5 побеждает
20. `test_master_loss_and_reelection` — node5 пропал, node4 становится мастером

---

### ФАЗА 4: Интеграция в unow

#### TODO 4.1: Добавить subtypes в `unow_wire.h`

**Файл**: `unow/include/unow/unow_wire.h`

Добавить после строки 11:
```c
#define UNOW_VENDOR_SUBTYPE_SYNC       0x04U
#define UNOW_VENDOR_SUBTYPE_DELAY_REQ  0x05U
```

#### TODO 4.2: Обновить `unow_parse_action_frame()` в `unow_radiotap.c`

**Файл**: `unow/src/unow_radiotap.c`, строка 144

Добавить SYNC и DELAY_REQ в whitelist допустимых subtypes.

**Diff**:
```diff
  if (vendor_header->subtype != UNOW_VENDOR_SUBTYPE_DATA &&
      vendor_header->subtype != UNOW_VENDOR_SUBTYPE_ACK &&
-     vendor_header->subtype != UNOW_VENDOR_SUBTYPE_DATA_SEQ) {
+     vendor_header->subtype != UNOW_VENDOR_SUBTYPE_DATA_SEQ &&
+     vendor_header->subtype != UNOW_VENDOR_SUBTYPE_SYNC &&
+     vendor_header->subtype != UNOW_VENDOR_SUBTYPE_DELAY_REQ) {
      return false;
  }
```

**Тест**: существующий `super_ack_test.c` должен продолжать проходить.
Дополнительно: добавить тест что SYNC subtype парсится корректно.

---

### ФАЗА 5: Интеграция в rx_dispatcher

#### TODO 5.1: Расширить `radio_rx_dispatcher_t`

**Файл**: `radiod/include/radiod/rx_dispatcher.h`

Добавить в структуру:
```c
/* Sync engine (set by radiod main, NULL if standalone) */
void *sync_ctx;  /* radio_sync_t* — void* чтобы избежать circular include */

/* SYNC/DELAY_REQ stats */
uint32_t rx_sync;
uint32_t rx_delay_req;
uint32_t sync_relayed;
```

Добавить в `radio_rx_stats_t`:
```c
uint32_t rx_sync;
uint32_t rx_delay_req;
uint32_t sync_relayed;
```

#### TODO 5.2: Добавить обработку SYNC/DELAY_REQ в `radio_rx_slot()`

**Файл**: `radiod/src/rx_dispatcher.c`

После блока self-drop (строка 268), перед ACK обработкой (строка 273),
добавить:

```c
/* SYNC frame → delegate to sync engine, possibly relay */
if (frame.subtype == UNOW_VENDOR_SUBTYPE_SYNC) {
    rxd->stats.rx_sync++;
    if (rxd->sync_ctx != NULL) {
        sync_frame_t sf;
        if (sync_frame_unpack(frame.payload, frame.len, &sf)) {
            int64_t rx_time = now_us();
            bool should_relay = radio_sync_on_sync_rx(
                (radio_sync_t *)rxd->sync_ctx,
                &sf, rx_time, sf.sender_node_id);
            if (should_relay) {
                /* Подготовить relay-копию и поставить в очередь */
                radio_sync_relay_enqueue(rxd, &sf, pcap, own_mac);
            }
        }
    }
    continue;
}

/* DELAY_REQ → master records T4 */
if (frame.subtype == UNOW_VENDOR_SUBTYPE_DELAY_REQ) {
    rxd->stats.rx_delay_req++;
    if (rxd->sync_ctx != NULL) {
        delay_req_frame_t dreq;
        if (delay_req_unpack(frame.payload, frame.len, &dreq)) {
            radio_sync_on_delay_req_rx(
                (radio_sync_t *)rxd->sync_ctx,
                &dreq, now_us());
        }
    }
    continue;
}
```

**Добавить helper** `radio_sync_relay_enqueue()`:
```c
static void radio_sync_relay_enqueue(radio_rx_dispatcher_t *rxd,
                                     const sync_frame_t *rx_sf,
                                     pcap_t *pcap,
                                     const uint8_t own_mac[6])
{
    radio_sync_t *sync = (radio_sync_t *)rxd->sync_ctx;
    sync_frame_t relay_sf;
    int64_t tx_time = now_us();

    if (!radio_sync_prepare_relay(sync, rx_sf, &relay_sf, tx_time))
        return;

    uint8_t packed[SYNC_FRAME_MAX_SIZE];
    size_t packed_len;
    if (!sync_frame_pack(&relay_sf, packed, sizeof(packed), &packed_len))
        return;

    /* Inject relayed SYNC */
    uint8_t wire[sizeof(struct unow_radiotap_tx_header) +
                 sizeof(struct unow_dot11_mgmt_header) +
                 sizeof(struct unow_action_vendor_header) +
                 SYNC_FRAME_MAX_SIZE];
    const uint8_t broadcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    size_t wire_len = unow_build_action_frame_ex(
        wire, sizeof(wire), own_mac, broadcast,
        packed, packed_len,
        UNOW_TX_RATE_1MBPS, UNOW_VENDOR_SUBTYPE_SYNC);
    if (wire_len > 0U) {
        pcap_inject(pcap, wire, wire_len);
        rxd->stats.sync_relayed++;
    }
}
```

#### TODO 5.3: Добавить функцию `radio_rx_dispatcher_set_sync()`

**Файл**: `radiod/include/radiod/rx_dispatcher.h`

```c
void radio_rx_dispatcher_set_sync(radio_rx_dispatcher_t *rxd, void *sync_ctx);
```

**Файл**: `radiod/src/rx_dispatcher.c`

```c
void radio_rx_dispatcher_set_sync(radio_rx_dispatcher_t *rxd, void *sync_ctx)
{
    if (rxd != NULL)
        rxd->sync_ctx = sync_ctx;
}
```

---

### ФАЗА 6: Модификация radiod main loop

#### TODO 6.1: Добавить CLI опции

**Файл**: `radiod/tools/radiod.c`

Добавить в `radiod_config_t`:
```c
uint16_t    sync_dl_us;        /* DL slot duration (default: 2000) */
uint16_t    sync_ul_us;        /* UL slot duration (default: 2000) */
uint16_t    sync_guard_us;     /* guard interval (default: 300) */
bool        sync_enabled;      /* enable SYNC protocol (default: false) */
```

Добавить в `config_defaults()`:
```c
cfg->sync_dl_us = 2000;
cfg->sync_ul_us = 2000;
cfg->sync_guard_us = 300;
cfg->sync_enabled = false;
```

Добавить опции в `long_opts`:
```c
{"sync",     no_argument,       NULL, 'S'},
{"dl-us",    required_argument, NULL, 'D'},
{"ul-us",    required_argument, NULL, 'U'},
{"guard-us", required_argument, NULL, 'G'},
```

Обновить usage().

#### TODO 6.2: Инициализация sync engine в main()

После `radio_rx_dispatcher_init()`:

```c
radio_sync_t sync_engine;
if (cfg.sync_enabled) {
    radio_sync_init(&sync_engine, cfg.node_id,
                    cfg.sync_dl_us, cfg.sync_ul_us, cfg.sync_guard_us);
    radio_rx_dispatcher_set_sync(&rxd, &sync_engine);
    fprintf(stderr, "radiod: SYNC protocol enabled, node_id=%u\n", cfg.node_id);
}
```

#### TODO 6.3: Реализовать `master_cycle()`

**Файл**: `radiod/tools/radiod.c`

Новая static функция. Подробный алгоритм — см. секцию 6.2.

Ключевые моменты:
- `radio_sync_build_beacon()` → `sync_frame_pack()` → `unow_build_action_frame_ex(SYNC)` → `pcap_inject()`
- DL слот: flush CTRL, затем P1/P2/P3 до dl_deadline
- UL слоты: `radio_rx_slot()` с отдельным deadline для каждого
- Guard: `usleep(guard_us)` между слотами (или busy-wait для точности)
- В конце: обновить slot_map для slave-ов которых слышали в UL

#### TODO 6.4: Реализовать `slave_cycle()`

**Файл**: `radiod/tools/radiod.c`

Новая static функция. Подробный алгоритм — см. секцию 6.3.

Ключевые моменты:
- Ожидание SYNC (rx_slot с таймаутом)
- `radio_sync_compute_timing()` после приёма SYNC
- DL фаза: `radio_rx_slot()` до dl_end_us
- sleep_until(my_ul_start_us)
- Отправка DELAY_REQ первым пакетом в UL слоте
- Отправка данных до my_ul_end_us
- NULL frame если нечего слать
- RX до конца суперкадра (слушаем чужие UL — для routing/learning)

#### TODO 6.5: Реализовать `candidate_cycle()`

**Файл**: `radiod/tools/radiod.c`

Простая функция:
- RX в течение 5 мс
- Если получен SYNC — tick() переведёт в SLAVE или оставит CANDIDATE
- Если election_timer истёк — tick() переведёт в MASTER

#### TODO 6.6: Модифицировать main loop

**Файл**: `radiod/tools/radiod.c`

Заменить единый TDMA-цикл (строки 434-560) на:

```c
if (cfg.sync_enabled) {
    radio_role_t role = radio_sync_tick(&sync_engine, now_us());
    switch (role) {
    case RADIO_ROLE_MASTER:    master_cycle(...); break;
    case RADIO_ROLE_SLAVE:     slave_cycle(...); break;
    case RADIO_ROLE_CANDIDATE: candidate_cycle(...); break;
    }
} else {
    /* Старый standalone цикл — без изменений */
    // ... существующий код строк 434-560 ...
}
```

**ВАЖНО**: standalone режим (--sync не указан) работает точно как раньше.
Ни одна строка старого пути не меняется.

---

### ФАЗА 7: Watchdog + Stats интеграция

#### TODO 7.1: Расширить watchdog для SYNC

**Файл**: `radiod/include/radiod/watchdog.h`

```c
void radio_watchdog_feed_sync(radio_watchdog_t *wd, int64_t now_us);
```

**Файл**: `radiod/src/watchdog.c`

```c
void radio_watchdog_feed_sync(radio_watchdog_t *wd, int64_t now_us)
{
    /* SYNC beacon = implicit CTRL heartbeat */
    radio_watchdog_feed(wd, now_us);
}
```

Вызывать из `radio_sync_on_sync_rx()` (slave) или из slave_cycle.

#### TODO 7.2: Расширить stats отчёт

**Файл**: `radiod/include/radiod/stats.h`

Добавить sync-метрики (см. секцию 5.6).

**Файл**: `radiod/src/stats.c`

В `radio_stats_report()` добавить вывод:
```
sync[role=SLAVE master=5 offset=+123µs rtt=890µs tx=150 rx=148 relay=12]
```

---

### ФАЗА 8: Тестирование

#### TODO 8.1: Host-level integration test

**Файл**: `radiod/tests/test_sync_integration.c`

Симуляция 2 узлов без реального Wi-Fi:
- Создать 2 экземпляра radio_sync_t (node_id=1, node_id=5)
- Прогнать 100 тиков, передавая SYNC между ними в памяти
- Проверить: node5 = MASTER, node1 = SLAVE
- Проверить: clock offset сходится к 0 (одна машина)

#### TODO 8.2: Host-level 5-node test

**Файл**: `radiod/tests/test_sync_5node.c`

Симуляция 5 узлов:
- node_id = 1, 2, 3, 4, 5
- Все стартуют одновременно
- Проверить: node5 = MASTER через ≤ election_timeout
- Все остальные = SLAVE
- slot_map содержит [1, 2, 3, 4]
- Убрать node5 → node4 = MASTER через timeout+election
- Вернуть node5 → node5 = MASTER снова

#### TODO 8.3: Host-level relay test

Симуляция: node1 ←→ node3 ←→ node5
(node1 не видит node5 напрямую)
- node5 = MASTER
- node3 = SLAVE, ретранслирует SYNC
- node1 = SLAVE (через relay от node3)
- Проверить: relay_hops=1 для node1
- Проверить: clock sync работает через relay

#### TODO 8.4: Реальное тестирование на 2 устройствах

Запустить radiod с `--sync` на двух LuckFox:
- Node 1 (drone), Node 5 (GCS)
- Проверить в логах:
  - Node 5: "became MASTER"
  - Node 1: "SLAVE, synced to node 5"
  - Clock offset в stats стабилен (±100µs)
  - CTRL/VIDEO проходят без потерь

#### TODO 8.5: Стресс-тест: потеря и восстановление

- Запустить 2 устройства, дождаться стабильного sync
- Выключить мастера (kill radiod)
- Проверить: slave → CANDIDATE → MASTER (если один)
- Включить старого мастера
- Проверить: бесшовная смена мастера обратно

---

### ФАЗА 9: Документация и polish

#### TODO 9.1: Обновить usage() и --help

Добавить описание новых опций в справку radiod.

#### TODO 9.2: Обновить init script

**Файл**: `radiod/scripts/S96radiod`

Добавить `--sync` в аргументы запуска (если включено в конфиге).

#### TODO 9.3: Добавить конфигурацию

**Файл**: `radiod/defaults/radiod.conf`

Добавить параметры sync (закомментированные по умолчанию):
```
# SYNC_ENABLED=0
# SYNC_DL_US=2000
# SYNC_UL_US=2000
# SYNC_GUARD_US=300
```

---

## 9. Зависимости между задачами

```
1.1 ──► 1.2 ──► 1.3
                  │
2.1 ──► 2.2 ──► 2.3
                  │
         ┌───────┘
         ▼
3.1 ──► 3.2 ──► 3.3
                  │
4.1 ──► 4.2 ────►│
                  │
         ┌───────┘
         ▼
5.1 ──► 5.2 ──► 5.3
                  │
         ┌───────┘
         ▼
6.1 ──► 6.2 ──► 6.3 ──► 6.4 ──► 6.5 ──► 6.6
                                           │
                                    ┌──────┘
                                    ▼
                              7.1 ──► 7.2
                                       │
                                ┌──────┘
                                ▼
                          8.1 ──► 8.2 ──► 8.3 ──► 8.4 ──► 8.5
                                                            │
                                                     ┌──────┘
                                                     ▼
                                               9.1 ──► 9.2 ──► 9.3
```

**Параллельно можно делать**: Фазы 1 и 2 (wire + clock — независимы).
Фаза 3 зависит от обеих. Фазы 4 и 5 — параллельно после 3.

---

## 10. Оценка трудоёмкости

| Фаза | Задачи | Оценка | Сложность |
|------|--------|--------|-----------|
| 1. Wire protocol | 1.1-1.3 | 4-6 часов | Низкая |
| 2. Clock sync | 2.1-2.3 | 6-8 часов | Средняя |
| 3. Sync engine | 3.1-3.3 | 12-16 часов | Высокая |
| 4. UNOW integration | 4.1-4.2 | 1-2 часа | Низкая |
| 5. RX dispatcher | 5.1-5.3 | 4-6 часов | Средняя |
| 6. Main loop | 6.1-6.6 | 12-16 часов | Высокая |
| 7. Watchdog+Stats | 7.1-7.2 | 2-3 часа | Низкая |
| 8. Тестирование | 8.1-8.5 | 8-12 часов | Средняя |
| 9. Documentation | 9.1-9.3 | 2-3 часа | Низкая |
| **Итого** | **29 задач** | **~50-70 часов** | |

---

## 11. Критические решения (принять до начала)

1. **Источник времени**: `CLOCK_MONOTONIC` через `clock_gettime()`.
   Точнее чем `gettimeofday()` (нет NTP-скачков), подходит для
   относительных измерений. Все компоненты должны использовать
   единообразно.

2. **Guard interval 300 µs**: нужно замерить реальный TX/RX turnaround
   на RTL8812AU + LuckFox. Если >300 µs — увеличить.
   Измерить: отправить пакет, замерить время до pcap_inject return.

3. **Точность таймеров**: `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)`
   для точного ожидания slot boundaries. НЕ usleep() — у него дрейф.
   Замерить jitter на LuckFox: ожидаем ±50-100 µs.

4. **SYNC relay vs data relay**: SYNC ретранслируется через pcap_inject
   в обработчике RX (inline), НЕ через TX scheduler. Причина: SYNC
   должен уйти немедленно, без ожидания TX слота.
   Data relay по-прежнему через TX scheduler.

5. **Standalone fallback**: `--sync` не указан → старый цикл без изменений.
   Это гарантирует что обновление radiod не сломает существующие
   системы без SYNC.
