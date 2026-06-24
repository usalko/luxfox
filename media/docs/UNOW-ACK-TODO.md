# TODO: Снижение NAL drop rate в видеопотоке UNOW

> **Проблема:** ~40% NAL-юнитов H.265 теряются при передаче через UNOW радио,
> несмотря на ASK-механизм. Один NAL = 10-23 LTS-пакета. Потеря ЛЮБОГО одного
> LTS-пакета из группы → весь NAL дропается assembler'ом.
>
> **Дата:** 2026-06-24

## 1. Текущее состояние

### Измерения (bench test, ~1м, ch6, 256 Kbps, 15fps, GOP=5)

```
[stats] video_rx=948 telem_rx=77 ctrl_rx=0 | NAL ok=92 drop=73 | video_out=86 Kbit/s
[stats] video_rx=962 telem_rx=66 ctrl_rx=0 | NAL ok=106 drop=83 | video_out=77 Kbit/s
[stats] video_rx=974 telem_rx=61 ctrl_rx=0 | NAL ok=88 drop=75 | video_out=92 Kbit/s
```

- **UNOW packet delivery:** ~95% (video_rx ≈ ожидаемым)
- **NAL success rate:** 55-60% (ok / (ok + drop))
- **NAL drop cause:** gap в LTS seq → assembler дропает весь NAL

### Почему 95% доставка пакетов → 55% NAL success

NAL из N LTS-пакетов доходит целиком только если ВСЕ N пакетов доставлены:

```
P(NAL ok) = (1 - PER)^N

При PER = 5%:
  N=1  (VPS/SPS/PPS):  P = 95%    ← параметры долетают
  N=10 (P-frame):      P = 60%    ← совпадает с измерениями!
  N=20 (IDR):          P = 36%    ← ключевые кадры теряются часто
```

### Цепочка передачи

```
vcpd (NAL → LTS frag) → UNOW TX (ASK per packet) → air → UNOW RX
  → ulama-gw (LTS → NAL assembler) → cascade-frame → cascade-core → WS
```

### Что уже сделано

- [x] ASK в UNOW для video-class трафика
- [x] VPS/SPS/PPS × 3 дублирование перед каждым IDR
- [x] GOP=5 (IDR каждые 5 кадров при 15fps = каждые 0.33с)
- [x] NAL assembler с gap detection в ulama-gw
- [x] Single-packet NAL fix (VPS/SPS/PPS теперь долетают)
- [x] H.265 test pattern подтверждён (SMPTE color bars декодируются)

---

## 2. Анализ путей решения

### Путь A: FEC (Forward Error Correction) на уровне LTS

**Идея:** добавить избыточные пакеты (XOR или Reed-Solomon) к каждой
группе LTS-пакетов одного NAL. При потере ≤M из N+M пакетов — восстановить
NAL без ретрансмиссии.

**Варианты FEC:**

| Схема | Overhead | Восстановление | Сложность |
|-------|----------|----------------|-----------|
| XOR parity (N+1) | +1 пакет на N | 1 потеря из N+1 | Низкая |
| Reed-Solomon (N, N+M) | +M пакетов | до M потерь | Средняя |
| Fountain codes (LT/Raptor) | ~5-10% | любые потери до overhead | Высокая |

**Для NAL из 10 пакетов при PER=5%:**

- XOR (10+1): P(≤1 loss in 11) = 57% + 34% = **91%**
- RS (10+2): P(≤2 losses in 12) ≈ **97%**
- RS (10+3): P(≤3 losses in 13) ≈ **99.5%**

**Плюсы:** нет round-trip, предсказуемая latency, работает при burst  
**Минусы:** постоянный overhead (+10-30%), нужна буферизация NAL перед FEC  
**Bandwidth:** 80 Kbit/s + 10% XOR = 88 Kbit/s — вписывается

### Путь B: Дублирование LTS-пакетов

**Идея:** каждый LTS-пакет отправляется 2 раза.

**При PER=5%, NAL из 10 пакетов:**
- P(оба потеряны) = 0.05² = 0.25% per slot
- P(NAL ok) = (1 - 0.0025)^10 = **97.5%**

**Плюсы:** тривиальная реализация (1 строка кода)  
**Минусы:** **+100% bandwidth** — 256 × 2 = 512 Kbps

**Вариант B2: селективное дублирование** — только NAL > 5 пакетов.
VPS/SPS/PPS уже ×3. IDR и большие P-frame ×2. Мелкие P-frame ×1.
Overhead: ~60-70%.

### Путь C: NACK-based ретрансмиссия на уровне LTS

**Идея:** ulama-gw обнаруживает gap в LTS seq, шлёт NACK → vcpd
ретрансмитирует пропущенный пакет.

**Плюсы:** нулевой overhead при чистом канале  
**Минусы:** round-trip ~10-50мс, обратный канал, буферизация, сложность  
**Примечание:** LTS уже имеет `lts_nack_t`, `lts_decoder_detect_gaps`,
но ретрансмиссия не подключена.

### Путь D: Уменьшить размер NAL

**Идея:** меньше NAL → меньше LTS-пакетов → выше P(ok).

| Параметр | Текущее | Вариант | NAL pkts | P(ok) |
|----------|---------|---------|----------|-------|
| Bitrate | 256 Kbps | 128 Kbps | ~5 | 77% |
| Resolution | 480×320 | 320×240 | ~4 | 81% |
| Slicing | 1 slice | 4 slices | ~2-3 per slice | 90%+ |

**Slicing** — самый перспективный: потеря одного slice = артефакт в части
кадра, а не потеря всего кадра.

---

## 3. Рекомендация

**Комбинация C (NACK-ретрансмиссия) + D (слайсинг) для достижения >95% NAL success:**

Основание: обратный канал уже есть (telem_rx ~60-70 пакетов в stats), механизм NACK
в LTS практически готов (lts_nack_t, lts_decoder_detect_gaps). Ретрансмиссия роняет
вероятность потери до 0.05²=0.25% за попытку. Слайсинг уменьшает размер NAL-групп
и обеспечивает гранулярность потерь.

| Шаг | Действие | Ожидаемый NAL ok | Effort |
|-----|----------|------------------|--------|
| 1 | NACK-ретрансмиссия в LTS | 55% → >95% | Реализовано |
| 2 | H.265 4 slices/frame | per-slice >97% | Реализовано |
| 3 | (опц.) XOR FEC для слайсов | per-slice >99% | 1 день |

---

## 4. План реализации

### [x] 4.1 NACK-ретрансмиссия в LTS

Файлы:
- `vcpd/include/vcpd/lts_encoder.h` — retransmit buffer + NACK decode
- `vcpd/src/lts_encoder.c` — реализация retx buf + NACK decode
- `vcpd/tools/vcpd.c` — хранение пакетов, обработка входящих NACK
- `ulama-gw/tools/ulama_gw.c` — генерация и отправка NACK

Протокол:
1. vcpd хранит отправленные LTS-пакеты в кольцевом буфере (128 слотов)
2. ulama-gw при обнаружении gap формирует NACK (start_seq + 16-bit bitmask)
3. NACK отправляется в ULAMA frame (CTRL class) через обратный канал
4. vcpd декодирует NACK, ищет пакеты в буфере и ретрансмитирует
5. Throttle: не более 1 NACK за 20 мс

Готово, когда: NAL drop < 5% при 256 Kbps, PER=5%

### [x] 4.2 H.265 слайсинг (4 слайса)

Файлы: `vcpd/src/video_mpp.c`

Действия:
1. `RK_MPI_VENC_SetSliceSplit(VENC_CHN_ID, &split)` с mode=1 (по CTU)
2. `u32SplitSize = ceil(CTU_rows / 4)` — ~4 слайса на кадр
3. Каждый слайс = отдельный NAL = отдельная LTS-группа
4. Потеря слайса = локальный артефакт, а не полный drop кадра

Готово, когда: визуальное качество acceptable при PER=5%

### [ ] 4.3 (Опционально) XOR FEC для слайсов

Если задержка окажется выше целевой — добавить XOR-пакет (N+1) для
каждого слайса. Для слайса из 4 пакетов: P(восстановление) ≈ 97.7%.
Overhead ~10-15%, реализация ~1 день.

### [ ] 4.4 Замеры и сравнение

Таблица результатов (заполнить по мере тестов):

| Конфигурация | NAL ok | NAL drop | Drop % | Video Kbit/s | Latency |
|---|---|---|---|---|---|
| Baseline (256K, no NACK) | 92 | 73 | 44% | 86 | ~300ms |
| 256K + NACK + 4 slices | | | | | |
| 256K + NACK + 4 slices + XOR | | | | | |

---

## 5. Метрики успеха

| Метрика | Текущее | Цель MVP | Цель production |
|---------|---------|----------|-----------------|
| NAL success rate | 55% | >85% | >95% |
| IDR success rate | ~36% | >80% | >95% |
| Video latency | ~300ms | <500ms | <200ms |
| Bandwidth overhead | 0% | ~5-10% (NACK) | <20% |
| Видимые артефакты | Постоянные | Редкие (1-2/сек) | Минимальные |
