# Проект 2 — `unow` (UNOW: ESP-NOW-аналог поверх сырых 802.11)

> Цель: транспорт L2 «как ESP-NOW», но на обычном Wi-Fi-адаптере (MW300UH /
> RTL8192EU) через **monitor-mode + frame injection**. Реализует **тот же
> интерфейс**, что `radio_espnow.h` из ULAMA, чтобы быть drop-in заменой для
> перенесённого стека (см. [`media/ulama/TODO.md`](../ulama/TODO.md)).
>
> UNOW — **тупой L2-датаграм-пайп**: connectionless, адресация по MAC (+
> broadcast), payload ≤ 250 Б, без IP/DHCP/ассоциаций. Вся надёжность, mesh,
> TTL, dedup — **выше**, в стеке ULAMA. Это сознательное архитектурное решение
> (см. §1 «Критический разбор»).

Статусы: `[ ]` не начато, `[~]` в работе, `[x]` готово, `[!]` риск/решение.

---

## 1. Критический разбор присланного совета (важно — читать первым)

Совет (libtins + Action-frame + mesh в листенере) в целом задаёт верное
направление, но содержит **ошибки и упрощения**, которые на железе выстрелят.
Ниже — что берём, что исправляем.

| # | В совете | Проблема | Решение в UNOW |
|---|---|---|---|
| 1 | `frame.addr2(iface)` | **Баг.** `addr2` — это 6-байтовый MAC источника, а не имя интерфейса. libtins не подставит MAC из строки. | Читать self-MAC через `SIOCGIFHWADDR` / `/sys/class/net/<if>/address`, класть в `addr2`. |
| 2 | `struct UNOW_Packet{...}` + `memcpy` по эфиру | Нет `__attribute__((packed))`, не задан endianness `msg_id` → формат «плывёт» между компиляторами/арх. | **Не изобретать второй формат.** Нести **ULAMA L1** (magic/version/seq/crc уже стабильны) как payload action-frame. UNOW добавляет только 802.11-несущую. |
| 3 | `frame.payload(raw)` / `af->payload()` | Неточный libtins-API. Внутренние байты достаются через `RawPDU`. | TX: `... / RawPDU(bytes)`. RX: `pdu.rfind_pdu<RawPDU>().payload()`. |
| 4 | (нет ACK) | Monitor-inject **fire-and-forget**: нет 802.11 MAC-ACK/retry, в отличие от ESP-NOW. | Надёжность — на ULAMA L2 ARQ (CTRL/BULK selective-ACK уже есть). UNOW сам не ретраит. |
| 5 | `iw ... set bitrates legacy-2.4 1` | Влияет только на **managed**-режим. Для **инжекта** рейт задаётся в **RadioTap-заголовке** кадра. | Ставить fixed legacy rate (1–6 Мбит/с) полем RadioTap `rate`/`MCS` в каждом TX-кадре. |
| 6 | mesh-ретрансляция внутри `unow_listener` | **Дублирует** mesh ULAMA, обходит его seq-dedup/TTL/health → двойной flood, петли. | UNOW строго **L2 one-hop**. Relay/TTL/dedup владеет `mesh_router` (проект 1). |
| 7 | RX игнорирует RSSI | Health-scoring ULAMA требует RSSI/per-radio метрики. | Парсить RSSI из RadioTap (`RadioTap::dbm_signal()`), прокидывать в rx-callback (`radio_espnow_frame_t.rssi`). |
| 8 | `Sniffer ... PROMISC` без фильтра | На занятом канале весь эфир валится в userspace → CPU/латентность. | BPF-prefilter по нашему OUI/subtype (`pcap_compile`/`set_filter`). |
| 9 | libtins на LuckFox | Тяжёлый C++ (exceptions, `<thread>`) на **uclibc**-тулчейне — риск сборки/размера. | **LuckFox-сторона на чистом C** через `libpcap`/`AF_PACKET`. libtins — только для desktop-пира (удобство). |
| 10 | (нет) self-reception | monitor-iface часто ловит **свои же** инжектнутые кадры и второе радио на том же хосте. | Dedup: `src_mac == self` → drop; плюс ULAMA seq-dedup. |

**Вывод:** идею «raw 802.11 vendor action-frame как несущая» берём; реализацию
переписываем на C/libpcap на устройстве, формат пакета — родной ULAMA L1, mesh и
ACK — не здесь.

---

## 2. Аппаратная база и допущения

- 2× **MW300UH** (чип **RTL8192EU**, драйвер `rtl8xxxu`, USB id ~`0bda:818b`),
  симметрично: оба конца умеют monitor + inject.
- LuckFox: драйвер уже пропатчен под monitor/inject (см.
  `/memories/repo/luckfox-wifi-build.md`, `wifi_injection.txt`: `CONFIG_RWNX_MON_XMIT=y`,
  `CONFIG_RWNX_MON_RXFILTER=y`). Если на LuckFox реально стоит MW300UH по USB —
  путь `rtl8xxxu`; если внутренний `aic8800dc` — путь его monitor-инжекта (оба
  валидны, важно зафиксировать, **какой адаптер несёт UNOW**).
- Buildroot уже имеет `BR2_PACKAGE_IW=y`, `BR2_PACKAGE_TCPDUMP=y` ⇒ **libpcap
  присутствует**. libtins — нет (нужен новый пакет, только если решим тащить
  его на устройство; по умолчанию — не тащим, см. §1.9).
- Репо-скрипты для bring-up переиспользуемы:
  [`wifi_mon.sh`](../../wifi_mon.sh) (airmon-ng + `iw dev ... interface add mon0
  type monitor` + `set channel 6`), [`wifi_up.sh`](../../wifi_up.sh),
  [`wifi_injection.txt`](../../wifi_injection.txt).

---

## 3. ГЕЙТ 0 — инжект/capture loopback (делать ПЕРВЫМ, до кода)

`[!]` Это **критический риск №1** всего предприятия: injection на `rtl8xxxu`
исторически капризен (ядро может молча дропать инжект или игнорировать RadioTap-
rate). Пока это не доказано на железе — писать стек бессмысленно.

- [ ] **0.1** Обе стороны в monitor на одном канале (скрипт `unow-mon.sh` на базе
  `wifi_mon.sh`): `iw dev <if> set monitor control otherbss`, `ip link set mon0
  up`, `iw dev mon0 set channel 6`.
- [ ] **0.2** LuckFox → desktop: инжект тестового кадра (`packeth`/`aireplay-ng
  --test mon0` или 10-строчный `pcap_inject` на RadioTap+Dot11). Поймать на
  desktop `tcpdump -i mon0 -e`.
- [ ] **0.3** Обратное направление desktop → LuckFox.
- [ ] **0.4** Замерить «инжект-рейт без потерь» (сколько к/с проходит), зафиксить
  рабочий канал и legacy-rate.
- [ ] **Артефакт:** лог `tcpdump` с обеих сторон, где видны наши кадры; число
  PER на 1000 инжектов. **Без прохода G0 проект 1 не стартует.**

---

## 4. Формат кадра UNOW (802.11-несущая)

Несущая — **vendor-specific 802.11 Action frame** (как делает сам ESP-NOW),
payload — **родной ULAMA L1-кадр** (не выдумываем второй формат, см. §1.2).

```
RadioTap (TX: present=RATE|TX_FLAGS; RX: читаем dbm_signal)
└─ Dot11 (type=Management, subtype=Action)
   addr1 = dst MAC  (или ff:ff:ff:ff:ff:ff = broadcast)
   addr2 = self MAC (из SIOCGIFHWADDR)            ← фикс бага совета
   addr3 = UNOW BSSID (фикс., напр. 0x52:NOW:OUI) — фильтр «свой/чужой»
   Action body:
     category   = 127 (Vendor-specific)
     OUI[3]     = UNOW_OUI (выбрать приватный, напр. 0xFA,0xCE,0x01)
     vendor_subtype = 0x01 (UNOW_DATA)
     payload[]  = ULAMA L1 frame (14B header + ≤220B), как из ulama_frame_pack()
```

- [x] **4.1** Зафиксировать константы: `UNOW_OUI`, `UNOW_BSSID`, subtype'ы
  (`DATA`, опц. `BEACON/PROBE` не нужны). Документировать в `include/unow/unow_wire.h`.
- [ ] **4.2** Помнить: `ULAMA_ESPNOW_MAX_PAYLOAD = 240` (radio_espnow.h). 802.11
  + RadioTap + action-header укладываются в ~250-байтовый «esp-now-подобный»
  бюджет — проверить, что MTU стека (фрагментация на 220 Б) сходится.

---

## 5. API (drop-in для ULAMA) — `include/unow/radio_unow.h`

Зеркалить `radio_espnow.h`, чтобы link_manager не заметил подмены. Имя файла
можно оставить `radio_espnow.h` (через compat) или дать `radio_unow.h` +
тонкий алиас.

```c
// те же сигнатуры, что radio_espnow.h:
int  unow_init(uint8_t node_id, const char *iface);   // вместо radio_espnow_init
int  unow_send(const uint8_t dst_mac[6], const uint8_t *payload, size_t len);
int  unow_add_peer(const uint8_t mac[6]);
bool unow_peer_known(void);
bool unow_get_peer_mac(uint8_t out[6]);
void unow_set_rx_callback(radio_espnow_rx_cb_t cb, void *ctx);      // frame{src_mac,data,len,rssi}
void unow_set_control_callback(radio_espnow_control_cb_t cb, void *ctx);
void unow_get_stats(radio_espnow_stats_t *out);       // та же struct статистики
```

- [x] **5.1** Заголовок-алиас: `radio_espnow_*` → `unow_*` (макросы/обёртки), чтобы
  перенесённый `link_manager.c` собрался без правок.
- [~] **5.2** Сохранить семантику ESP-NOW: broadcast-MAC `ff:..:ff`, peer-таблица,
  «первый услышанный peer фиксируется» (как `s_peer_known` в radio_espnow.c).
- [~] **5.3** Статистика — заполнять ту же `radio_espnow_stats_t` (tx/rx/fail/rssi
  min/max/last, queue depth), чтобы `ULAMA_STATS` работал без изменений.

---

## 6. Реализация на устройстве (C/libpcap) — `media/unow/src/`

### 6.1 Bring-up интерфейса
- [~] **6.1.1** `unow_iface.c`: программно (без скрипта) или через вызов
  helper-скрипта поднять monitor+channel; читать self-MAC. Идемпотентность.
- [x] **6.1.2** Открыть `pcap` на `mon0` (`pcap_open_live`, `DLT_IEEE802_11_RADIO`),
  выставить `pcap_setnonblock`/таймаут, `pcap_set_immediate_mode`.

### 6.2 TX-путь (inject)
- [x] **6.2.1** Преаллоцированный буфер кадра: `[RadioTap][Dot11 action hdr]
  [OUI/subtype][payload]`. RadioTap с полем **rate** (фикс legacy) и TX_FLAGS
  (`NOACK|NOSEQ`).
- [x] **6.2.2** `unow_send()`: заполнить addr1=dst/bcast, addr2=self, addr3=BSSID,
  вкопировать payload, `pcap_inject()`. Без аллокаций на горячем пути (важно для
  VIDEO-битрейта — фикс замечания совета о `std::vector` per-send).
- [ ] **6.2.3** TX-воркер + очередь (как radio_espnow: ring, eviction старых,
  pacing) — переиспользовать `ulama_queue` из проекта 1 или локальный.

### 6.3 RX-путь (sniff)
- [x] **6.3.1** BPF-prefilter: компилировать фильтр «management/action + наш OUI»
  (`pcap_compile`+`pcap_setfilter`) — отсечь чужой эфир (фикс §1.8).
- [~] **6.3.2** RX-воркер: `pcap_loop`/`pcap_next_ex`; распарсить RadioTap →
  `rssi`; проверить addr3==BSSID, category==127, OUI==UNOW; `src=addr2`.
- [~] **6.3.3** Self-reception dedup: `addr2==self` → drop (фикс §1.10).
- [ ] **6.3.4** Отдать `radio_espnow_frame_t{src_mac, data=ULAMA-L1, len, rssi}`
  в rx-callback link_manager'а.
- [ ] **6.3.5** Control-frames (HELLO/OGM) ULAMA проходят тем же путём — UNOW не
  знает про их семантику (это L2 ULAMA).

### 6.4 RadioTap helpers
- [x] **6.4.1** `unow_radiotap.c`: минимальный билдер TX-RadioTap (rate, tx_flags)
  и парсер RX-RadioTap (present-bitmap → dbm_signal). Без внешних либ.

### 6.5 Диагностика
- [x] **6.5.1** `unow_diag`: отдельный bin/target для LuckFox, который умеет
  инициализировать интерфейс, печатать state/stats, отправлять тестовый UNOW-кадр
  и слушать эфир с декодированием UNOW payload + RSSI.
- [x] **6.5.2** Базовый log layer: `UNOW_LOG_LEVEL`, уровни `error..trace`,
  hexdump TX-пакетов и явные сообщения при ошибках bring-up/pcap/inject.

---

## 7. Desktop-пир — `host/unow-peer/` (libtins разрешён)

На «большом» Linux (Ubuntu desktop с MW300UH) можно взять libtins — там нет
uclibc-ограничений и это быстрый способ получить второй узел/референс.

- [ ] **7.1** Установка: `apt install libtins-dev libpcap-dev` (как в совете §2.1).
- [ ] **7.2** Реализовать `unow-peer` на libtins, **но** с исправлениями §1
  (addr2=self-MAC, payload=ULAMA-L1 через `RawPDU`, RSSI из RadioTap, BPF-фильтр,
  без mesh-в-листенере).
- [ ] **7.3** Совместимость по эфиру C-устройство ↔ libtins-desktop: один и тот же
  `UNOW_OUI/BSSID/subtype` и один ULAMA L1. Кросс-тест: LuckFox шлёт — desktop
  принимает и наоборот.
- [ ] **Артефакт:** desktop `unow-peer` обменивается ULAMA-кадрами с LuckFox.

---

## 8. Сборка — `media/unow/Makefile`

- [x] **8.1** Makefile по образцу `media/luxfox/Makefile`: `CC :=
  $(RK_MEDIA_CROSS)-gcc`, собрать `libunow.a` (+ `.so`), линковка `-lpcap`.
- [ ] **8.2** Гарантировать `BR2_PACKAGE_LIBPCAP=y` в Buildroot (сейчас приходит
  транзитивно через `tcpdump`; добавить явно, чтобы не зависеть от tcpdump).
- [ ] `[!]` **8.3** Если когда-нибудь решим тащить libtins на устройство — завести
  buildroot-пакет `package/libtins` (C++/uclibc-ng — отдельно проверить
  exceptions/threads). По умолчанию: **не тащим**, устройство на чистом C.
- [ ] **8.4** Host-таргет (x86) для юниттестов парсера RadioTap/wire-формата.

---

## 9. Скрипты bring-up — `media/unow/scripts/`

- [x] **9.1** `unow-mon.sh` — обобщить `wifi_mon.sh`: параметризовать имя
  интерфейса и канал (`$IFACE`, `$CHAN`), убрать хардкод `wlx088af1287d57`.
- [x] **9.2** `unow-down.sh` — на базе `wifi_up.sh` (выход из monitor, рестарт
  NetworkManager на desktop; на LuckFox — просто `ip link set ... down`).
- [ ] **9.3** Документировать выбор канала/региона/мощности (совет §5: HT20,
  фикс rate, max TX-power региона, внешняя антенна на RP-SMA).

---

## 10. Тестовые гейты

| Гейт | Что | Критерий |
|---|---|---|
| **G0** | raw inject/capture обе стороны (§3) | наши кадры видны в `tcpdump`; PER замерен |
| **G1** | `unow_send`/rx-callback с ULAMA-L1, петля LuckFox↔LuckFox | 1000 кадров, PER < 2%, RSSI читается |
| **G1b** | кросс-совместимость C-device ↔ libtins-desktop | обмен в обе стороны |
| **G1c** | BPF-фильтр на занятом канале | CPU userspace в норме, чужой эфир отсечён |
| **G1d** | throughput одного хопа | замер потолка к/с и Мбит/с на фикс legacy-rate |

> После G1 — отдать UNOW в проект 1 как `radio` и идти к его G3.

---

## 11. Риски

- `[!]` **R1. rtl8xxxu inject** — закрывается G0. План B при провале: попробовать
  `aic8800dc` (внутренний, уже пропатчен) как несущую UNOW; либо иной адаптер с
  заведомо рабочим инжектом (mt76/ath9k_htc).
- `[!]` **R2. RadioTap-rate игнорируется драйвером** — некоторые драйверы не
  уважают rate из RadioTap. Проверить на G0; если так — жить на дефолтном рейте
  драйвера, зафиксировать в замерах.
- **R3. Нет MAC-ACK** — by design; надёжность на ULAMA L2 (ARQ CTRL/BULK).
- **R4. Регуляторка/мощность/канал** — занятый канал 6 даёт коллизии; выбрать
  тихий канал, зафиксировать на обоих концах.
- **R5. Self/двойной приём** — dedup по self-MAC + ULAMA seq.

---

## 12. Целевая структура каталога

```
media/unow/
  Makefile
  include/unow/
    radio_unow.h        # API, зеркало radio_espnow.h (§5)
    unow_wire.h         # OUI/BSSID/subtype, layout кадра (§4)
  src/
    unow_iface.c        # monitor bring-up, self-MAC, pcap open (§6.1)
    unow_tx.c           # inject hot-path (§6.2)
    unow_rx.c           # sniff + BPF + RadioTap RSSI (§6.3)
    unow_radiotap.c     # RadioTap build/parse (§6.4)
    unow.c              # API-склейка, peer-table, stats (§5)
  scripts/
    unow-mon.sh         # ← обобщённый wifi_mon.sh (§9)
    unow-down.sh
  host/unow-peer/       # desktop-референс на libtins (§7)
  tests/                # host-юниттесты wire/RadioTap
  TODO.md               # этот файл
```
