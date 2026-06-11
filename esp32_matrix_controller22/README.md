# ESP32 Matrix Controller

Этот проект можно открывать напрямую из `Arduino IDE` как обычный скетч:

`/Users/kesha/Desktop/vibecoding/ledmatrix/esp32_matrix_controller22`

Главный файл скетча:

`esp32_matrix_controller22.ino`

Основной код прошивки находится в:

`src/app.cpp`

Что выбрать в Arduino IDE:

- Board package: `esp32 by Espressif Systems`
- Board: `ESP32C3 Dev Module`
- Partition Scheme: `Huge APP (3MB No OTA/1MB SPIFFS)`

OTA-обновления в этом проекте сознательно не поддерживаются. Целевой сценарий обновления
прошивки — через USB/serial из Arduino IDE или PlatformIO. Поэтому разметка использует один
большой app-раздел вместо двух OTA-слотов.

Какие библиотеки должны быть установлены:

- `FastLED` (проверено на 3.10.3)
- `ArduinoJson` (проверено на 7.4.3)

Основной путь сборки и прошивки — `arduino-cli`:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c3:PartitionScheme=huge_app .
# сборка + прошивка подключённой платы:
arduino-cli compile --fqbn esp32:esp32:esp32c3:PartitionScheme=huge_app -u -p /dev/cu.usbmodem101 .
```

Важно: arduino-cli использует ядро esp32 3.x, где `NetworkClient::setTimeout()`
принимает миллисекунды (в ядре 2.x были секунды). При портировании кода со старого
ядра проверяйте клиентские таймауты.

PlatformIO-конфигурация (`platformio.ini`, `partitions_huge_app.csv` — 3MB app + ~1MB SPIFFS)
сохранена, но PlatformIO требует Python ≥3.10 и на текущей машине не работает
(системный Python 3.9).

Для локального предпросмотра веб-интерфейса без прошивки:

- запустите `sh tools/export_web_ui.sh`
- откройте `web/index.html` в браузере
- перед коммитом можно проверить, что экспорт не забыли: `sh tools/check_web_ui_export.sh`

Этот HTML умеет работать в режиме предпросмотра сам по себе:

- при открытии как локальный файл используется встроенный мок API
- тот же шаблон продолжает работать на ESP32 через реальные `/api/*`

## Текущие режимы и экран

В прошивке оставлены только актуальные режимы:

- `Часы + темп`
- `Огонь`
- `Плазма`
- `Лава`

Отдельные режимы `Температура`, `Часы` и `Тест погоды` удалены. Выбор шрифтов также
удалён: активный информационный режим использует единственный компактный шрифт `Micro 3×5`.

Шкала яркости в интерфейсе — `1..255`, но на матрицу применяется мягкая
квадратичная калибровка. Это даёт более точную регулировку на малой яркости и позволяет
опустить минимум до едва заметного свечения.

### Быстрые локальные проверки

```bash
sh tools/export_web_ui.sh
sh tools/check_web_ui_export.sh
python3 -m py_compile tools/bambu_relay/relay.py tools/bambu_relay/mock_relay.py
python3 -m unittest discover -s tools/bambu_relay -p "test_*.py"
for f in tools/export_web_ui.sh tools/check_web_ui_export.sh tools/bambu_relay/openwrt/bambu-relay.init; do sh -n "$f" || exit 1; done
```

Для проверки прошивки:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c3:PartitionScheme=huge_app .
```

## Bambu Relay

Для live-статуса принтера используется внешний relay:

- `tools/bambu_relay/relay.py`
- `tools/bambu_relay/requirements.txt`
- деплой на OpenWrt-роутер описан в `tools/bambu_relay/README.md`

Прошивка ESP больше не выполняет прямой вход в Bambu Cloud. Она только опрашивает
`Relay URL`, а relay уже получает статус принтера по MQTT. Для мониторинга у relay
есть `GET /healthz` — отвечает `{"ok": true}` без авторизации и без данных печати.

Relay умеет два режима:

- `cloud MQTT`: рекомендуемый путь, сохраняет облачный режим принтера
- `LAN MQTT`: опциональный fallback для `LAN only`

### Cloud MQTT relay

Нужны:

- `Access Token` Bambu
- `serial` принтера
- компьютер в одной сети с матрицей

Минимальный запуск:

```bash
cd /Users/kesha/Desktop/vibecoding/ledmatrix/esp32_matrix_controller22
python3 -m pip install -r tools/bambu_relay/requirements.txt
python3 tools/bambu_relay/relay.py \
  --cloud-token '<BAMBU_ACCESS_TOKEN>' \
  --printer-serial '<PRINTER_SERIAL>' \
  --listen-host 0.0.0.0 \
  --listen-port 8787
```

`uid` аккаунта relay попытается получить сам через cloud API. Если это не удалось, его можно передать явно:

```bash
python3 tools/bambu_relay/relay.py \
  --cloud-token '<BAMBU_ACCESS_TOKEN>' \
  --cloud-uid '<BAMBU_UID>' \
  --printer-serial '<PRINTER_SERIAL>'
```

Если нужен другой региональный брокер, его можно переопределить:

```bash
--mqtt-host us.mqtt.bambulab.com
```

### LAN MQTT relay

Этот режим нужен только если принтер переведён в `LAN only` и у вас есть `access code`.

```bash
python3 tools/bambu_relay/relay.py \
  --printer-host 192.168.1.50 \
  --printer-serial '<PRINTER_SERIAL>' \
  --printer-access-code '<LAN_ACCESS_CODE>' \
  --listen-host 0.0.0.0 \
  --listen-port 8787
```

### Подключение матрицы

После запуска relay в веб-интерфейсе матрицы включите:

- `Bambu -> Показывать статус принтера в режиме Часы + темп`
- `Relay URL = http://<IP_компьютера_с_relay>:8787/status`

Опционально можно задать `Relay Key` и передавать его relay-сервису через `--auth-key`
(или `BAMBU_RELAY_KEY` в конфиге). Ключ проверяется timing-safe сравнением;
запросы без заголовка `X-Relay-Key` получают `401`.
