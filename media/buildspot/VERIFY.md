# Проверка сценария buildspot MVP

Этот сценарий проверяет текущую архитектуру: не фоновый UART-сервис, а хостовая консоль `buildspot-console.py` и foreground-агент `buildspot-agent.sh`.

## 1. Предварительная проверка на ПК

Сначала убедитесь, что локальная часть готова:

```bash
cd /home/pascale/projects/63411/luxfox
python3 -m py_compile media/buildspot/buildspot-console.py
sh -n media/buildspot/buildspot-agent.sh
bash media/buildspot/build.sh
```

Ожидаемый результат:
- `py_compile` завершился без ошибок;
- `sh -n` не показал синтаксических ошибок;
- `media/buildspot/build.sh` положил агент в `output/out/oem/usr/bin/buildspot-agent.sh`.

Проверьте, что новый файл реально есть:

```bash
ls -l output/out/oem/usr/bin/buildspot-agent.sh
```

---

## 2. Тест 1: один запуск `sync` на реальном UART

Этот режим подходит для быстрой проверки без интерактивной консоли.

1. Подключите LuckFox к ПК по UART (`/dev/ttyUSB0` или другой порт).
2. Дождитесь, пока на устройстве появится обычный shell-приглашение на UART.
3. На ПК выполните:

```bash
cd /home/pascale/projects/63411/luxfox
python3 media/buildspot/buildspot-console.py sync \
  --device /dev/ttyUSB0 \
  --source output/out/oem \
  --timeout 60 \
  --handshake-timeout 10
```

Ожидаемый результат:
- в выводе видно `BUILDSPOT/1 READY`;
- программа считает локальный индекс из `output/out/oem`;
- затем начинает обмен по UART;
- в конце печатает `[buildspot-console] Sync complete`.

Проверка после синхронизации на устройстве:

```sh
ls -la /oem/.buildspot.index
find /oem -maxdepth 2 -type f | head -20
```

---

## 3. Тест 2: интерактивная консоль `console`

Этот режим — основной сценарий для ручной проверки.

1. На ПК запустите:

```bash
cd /home/pascale/projects/63411/luxfox
python3 media/buildspot/buildspot-console.py console \
  --device /dev/ttyUSB0 \
  --source output/out/oem
```

2. В этой же сессии вы увидите обычный вывод UART.
3. На устройстве должен быть доступен обычный shell.
4. В хост-консоли нажмите:
   - `Ctrl-]`, затем `s`

Ожидаемый результат:
- программа переходит в режим синхронизации;
- запускается агент `/oem/usr/bin/buildspot-agent.sh --stdio --root /oem`;
- появляется banner `BUILDSPOT/1 READY`;
- проходит обмен индексом/файлами;
- после завершения вы снова возвращаетесь в обычную консоль.

---

## 4. Что считать успехом

Сценарий считается успешным, если:

- `python3 -m py_compile media/buildspot/buildspot-console.py` завершился без ошибок;
- `bash media/buildspot/build.sh` создал `output/out/oem/usr/bin/buildspot-agent.sh`;
- на реальном UART `sync` или `console` дошёл до `BUILDSPOT/1 READY` и не упал на `TimeoutError`;
- индекс `/oem/.buildspot.index` появился или обновился;
- после завершения консоль вернулась в обычный режим и shell продолжил работать.

---

## 5. Базовая диагностика

Если синхронизация не стартует:

```bash
ls -l /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
python3 media/buildspot/buildspot-console.py --help
```

Если агент не стартует на устройстве:

```sh
ls -l /oem/usr/bin/buildspot-agent.sh
sh -x /oem/usr/bin/buildspot-agent.sh --stdio --root /oem
```

Если нужен полный сброс индекса:

```sh
rm -f /oem/.buildspot.index
```

