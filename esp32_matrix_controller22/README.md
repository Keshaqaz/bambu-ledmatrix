# ESP32 Matrix Controller

Этот проект можно открывать напрямую из `Arduino IDE` как обычный скетч:

`/Users/kesha/Desktop/vibecoding/ledmatrix/esp32_matrix_controller22`

Главный файл скетча:

`esp32_matrix_controller22.ino`

Основной код прошивки находится в:

`src/app.cpp`

Установленная версия: `4.2.0-2026.08.28` (`app1`), OTA выполнено 2026-08-28.
Текущий исходник T04: `4.2.2-2026.08.31`. Production-сборка занимает
1 389 979 байт flash (70%)
и 49 828 байт глобальной памяти (15%). От последней записанной T01-сборки это
+113 038 байт flash и +7 784 байта globals; относительно исходного ADR baseline
дельта +37 692 и +3 232 байта. On-device minimum heap, largest block и task
high-water marks T04 пока не сняты.

Что выбрать в Arduino IDE:

- Board package: `esp32 by Espressif Systems`
- Board: `ESP32C3 Dev Module`
- Partition Scheme: `Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)`

Прошивка поддерживает OTA-обновления: два app-слота по 1.875 MB (`min_spiffs`),
образ пишется в неактивный слот, переключение происходит атомарно в конце загрузки.
Подробности — в разделе «OTA-обновления» ниже. Схему `default` (слоты ~1.25 MB)
использовать нельзя: бинарник в неё не влезает — именно поэтому первая попытка
OTA в этом проекте когда-то провалилась.

Какие библиотеки должны быть установлены:

- `FastLED` (проверено на 3.10.3)
- `ArduinoJson` (проверено на 7.4.3)

Основной путь сборки и прошивки — `arduino-cli`:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c3:PartitionScheme=min_spiffs --export-binaries .
# сборка + прошивка по USB:
arduino-cli compile --fqbn esp32:esp32:esp32c3:PartitionScheme=min_spiffs -u -p /dev/cu.usbmodem101 .
# подписать production image для WebUI OTA:
sh tools/sign_ota.sh build/esp32.esp32.esp32c3/esp32_matrix_controller22.ino.bin
```

## OTA-обновления

Владелец проекта 2026-09-02 дал постоянное разрешение на firmware/device deploy
на принадлежащие проекту LED Matrix устройства. Повторно спрашивать разрешение
перед signed WebUI OTA или необходимой диагностической перепрошивкой не нужно.
Перед загрузкой всё равно проверяются точный target, версия и image; после неё —
`fwVersion`, `fwPartition`, health и доступность rollback/boot-guard. Разрешение
не распространяется на чужие устройства и несвязанные разрушительные действия.

Сетевой путь один: WebUI. По принятому security level 0 локальный WebUI/API не
требует admin password/session/CSRF; ECDSA-подпись сохраняется как проверка
целостности firmware. Уже установленная `4.2.0` всё ещё содержит прежний T01
session gate — его нужно удалить следующей firmware-итерацией. ArduinoOTA/espota
на порту 3232 отключён. В `Система → Обновление прошивки` выбрать оба файла:

1. Production image
   `build/esp32.esp32.esp32c3/esp32_matrix_controller22.ino.bin` (появляется при
   сборке с `--export-binaries`). **Не** `merged.bin` — прошивка отклонит его
   (нет app-дескриптора), но лучше не путать. Перед заливкой проверяйте свежесть
   `build/`: `arduino-cli compile --output-dir <dir>` молча перезаписывает и его,
   так что там может лежать не та сборка, что вы думаете.
2. Подпись `.sig`, созданную `sh tools/sign_ota.sh <image>`. Приватный ключ
   находится локально в `.secrets/ota_signing_private.pem`, исключён из Git и
   должен храниться как эксплуатационный secret. Неподписанный или подписанный
   другим ключом image записывается только во временный OTA slot и не коммитится.

Защита от кирпича:

- Образ пишется в неактивный слот; обрыв связи/питания во время загрузки
  безвреден — устройство продолжает работать на старой прошивке.
- Boot-guard: счётчик загрузок в NVS (namespace `bootguard`) сбрасывается после
  60 с стабильной работы. 3 сбоя подряд → автоматический откат на предыдущий
  слот. Побочный эффект: 3 ручных ресета подряд в первые 60 с тоже вызовут
  откат — это осознанный компромисс.
- USB-прошивка всегда остаётся запасным путём.

Проверка после OTA: `/api/status` отдаёт `fwVersion` (поднимайте `FW_VERSION`
в `src/app.cpp` при каждой итерации) и `fwPartition` (`app0`/`app1` — слот
меняется при каждом успешном OTA).

Переход со старой разметки `huge_app` на `min_spiffs` требует одной прошивки по
USB (смена таблицы партиций по OTA невозможна). NVS-раздел расположен по тому же
смещению — настройки при этом сохраняются. Для матрицы на `192.168.1.65` переход
выполнен 2026-06-11, оба слота проверены.

Важно: arduino-cli использует ядро esp32 3.x, где `NetworkClient::setTimeout()`
принимает миллисекунды (в ядре 2.x были секунды). При портировании кода со старого
ядра проверяйте клиентские таймауты.

PlatformIO-конфигурация (`platformio.ini`, разметка `min_spiffs.csv` из ядра)
сохранена, но текущая локальная установка повреждена: launcher из
`~/.platformio/penv/bin/` ссылается на отсутствующий Python, а старая установка
в `~/Library/Python/3.9/bin/` несовместима с Python 3.9. Используйте
`arduino-cli`, пока окружение PlatformIO не пересоздано.

## Переподключение Wi-Fi

При потере сохранённой сети прошивка сохраняет точку доступа настройки и сама
повторяет подключение через `5 → 10 → 20 → 40 → 60` секунд. Пока роутер выключен,
дальнейшие попытки идут раз в минуту; после успешного подключения backoff
сбрасывается. Таймер начинается в момент потери связи или окончания неудачной
попытки, поэтому он не зависит от uptime устройства. Та же политика работает,
если сохранённая сеть была недоступна уже во время загрузки.

Хостовый регрессионный тест политики:

```bash
c++ -std=c++11 -Wall -Wextra -pedantic tests/wifi_reconnect_policy_test.cpp \
  -o /tmp/wifi_reconnect_policy_test && /tmp/wifi_reconnect_policy_test
```

`tests/t01_security_policy_test.cpp` проверяет отменённую ADR 0002 модель с
паролями и сессией и поэтому не входит в актуальный набор проверок. При удалении
старого gate из прошивки его нужно заменить узким host-тестом открытого локального
доступа security level 0.

Для локального предпросмотра веб-интерфейса без прошивки:

- запустите `sh tools/export_web_ui.sh`
- откройте `web/index.html` в браузере
- перед коммитом можно проверить, что экспорт не забыли: `sh tools/check_web_ui_export.sh`

Этот HTML умеет работать в режиме предпросмотра сам по себе:

- при открытии как локальный файл используется встроенный мок API
- тот же шаблон продолжает работать на ESP32 через реальные `/api/*`

## Экран и веб-интерфейс

Матрица показывает единственный экран: часы (формат 12/24 ч, без разделителя)
и уличную температуру шрифтом `Micro 3×5`, плюс полосу прогресса печати Bambu
внизу. Декоративные режимы (огонь/плазма/лава) и настройки разделителя удалены
вместе с выбором режима. При кратком обрыве Wi-Fi, недоступности relay или
временном статусе `unknown/offline` последний прогресс остаётся на экране;
полоса очищается после явного статуса `idle`.

В обычном статичном состоянии кадр проверяется раз в секунду, при этом кэш
пропускает `FastLED.show()`, если минута, температура, настройки и состояние
принтера не изменились. Частота 4 FPS остаётся только для анимированных
состояний Bambu (`preparing`, `paused`, `finished`, `error`). Основной `loop()`
обслуживает WebServer и Wi-Fi state machine с частотой 20 Гц и спит по 50 мс между
проходами. Это исправило перегрев: на `4.0.4` датчик CPU показывал `71.3 °C`
после 22 минут, на `4.0.5` — `49.3 °C` через 4 минуты и `51.3 °C` через 5 минут.

Веб-интерфейс — пять разделов в нижней навигации:

- **Экран** — яркость (ручная или авто день/ночь: по часам либо по рассвету/закату),
  цвета часов и температуры (автоцвет по градусам), позиции строк, поворот/отражение
- **Сеть** — статус подключения, сканирование и смена Wi-Fi
- **Погода** — провайдер (`Авто` = OWM с фоллбэком на Open-Meteo / только OWM /
  только Open-Meteo), API-ключ OWM, локация городом или координатами, интервал.
  Open-Meteo работает без ключа, но только по координатам
- **Принтер** — включение статуса печати, Relay URL/Key, live-прогресс
- **Система** — версия/слот прошивки, NTP (с зашитыми фоллбэками
  `time.google.com`/`time.cloudflare.com`), часовой пояс из локации погоды или
  вручную, OTA-загрузка, перезагрузка

Все изменения сохраняются автоматически (индикатор в шапке). Шкала яркости в
интерфейсе — `1..255` с мягкой квадратичной калибровкой: точная регулировка на
малой яркости, минимум — едва заметное свечение.

Локальная модель доступа — **security level 0** из
[`ADR 0002`](../docs/adr/0002-use-security-level-zero-for-local-access.md): onboarding
AP открыт, WebUI и `/api/*` не требуют admin password, session cookie или CSRF.
Любой клиент LAN/AP может читать diagnostics, менять настройки, запускать Cloud
onboarding/logout/refresh, reboot и signed Web OTA; владелец принимает этот риск
ради отсутствия раздражающего домашнего login flow. Установленная `4.2.0` пока
не соответствует решению: её уникальный AP password и admin provisioning должны
быть удалены следующей firmware-итерацией. Bambu password при этом по-прежнему
не сохраняется, а Cloud tokens не возвращаются через API.

Анимации на самой матрице: янтарная «волна зажигания» при включении, значок
Wi-Fi с дугами при поиске сети, столб прогресса при OTA (зелёная заливка —
успех, красный крест — ошибка). Все рисуются с учётом настроенного
поворота/отражения.

Погодные запросы OWM и Open-Meteo выполняются в порядке HTTP → HTTPS. HTTP
быстрее после холодного старта и не требует TLS-handshake, но для OWM раскрывает
API-ключ в transit. На время `fetchWeather()` прошивка выключает Wi-Fi power save
и затем возвращает `WIFI_PS_MIN_MODEM`. Не добавляйте в один вызов
`fetchWeather()` одновременные HTTPS- и HTTP-клиенты: эксперимент `4.0.2`
переполнил стек `weatherTask` и вызвал boot loop. Текущая реализация создаёт
только один клиент за раз и запускает HTTPS только после неудачного HTTP.
Диагностика gateway/DNS и resolved IP доступна в `/api/status` и `weatherError`.

Положение температуры проверено для всех сочетаний поворота и зеркалирования.
При автоцентрировании, `ct6TempY=10`, `rotation=2`, `flipH=true` строка `+17`
полностью помещается в 16×16; полоса Bambu на строках 7–8 её не перекрывает.

### Быстрые локальные проверки

```bash
sh tools/export_web_ui.sh
sh tools/generate_web_ui_gzip.sh
sh tools/check_web_ui_export.sh
c++ -std=c++17 -Wall -Wextra -pedantic tests/wifi_reconnect_policy_test.cpp -o /tmp/wifi_reconnect_policy_test && /tmp/wifi_reconnect_policy_test
c++ -std=c++17 -Wall -Wextra -pedantic tests/printer_status_source_test.cpp -o /tmp/printer_status_source_test && /tmp/printer_status_source_test
c++ -std=c++17 -Wall -Wextra -pedantic tests/bambu_report_parser_test.cpp -o /tmp/bambu_report_parser_test && /tmp/bambu_report_parser_test
c++ -std=c++17 -Wall -Wextra -pedantic tests/bambu_lifecycle_mapping_test.cpp -o /tmp/bambu_lifecycle_mapping_test && /tmp/bambu_lifecycle_mapping_test
c++ -std=c++17 -Wall -Wextra -pedantic tests/bambu_credential_store_test.cpp -o /tmp/bambu_credential_store_test && /tmp/bambu_credential_store_test
c++ -std=c++17 -Wall -Wextra -pedantic tests/bambu_auth_contract_test.cpp -o /tmp/bambu_auth_contract_test && /tmp/bambu_auth_contract_test
python3 -m py_compile tools/bambu_relay/relay.py tools/bambu_relay/mock_relay.py
python3 -m unittest discover -s tools/bambu_relay -p "test_*.py"
for f in tools/export_web_ui.sh tools/check_web_ui_export.sh tools/bambu_relay/openwrt/bambu-relay.init; do sh -n "$f" || exit 1; done
```

Для проверки прошивки:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c3:PartitionScheme=min_spiffs .
```

## Bambu Cloud и Relay

T04 добавляет в WebUI direct Cloud onboarding: email/password, email/SMS
verification, явный выбор принтера, MQTT `pushall`, refresh и logout. После
TOTP firmware повторяет first-party portal flow: получает временную session
cookie от `/api/sign-in/tfa`, затем запрашивает полный credential envelope у
`/api/auth/token`. Cookie ограничена фиксированным буфером, передаётся только
между этими последовательными TLS-запросами и сразу затирается. Неполный
envelope даёт recoverable error и не заменяет active credential.
Пароль существует только во время login operation и затирается после её
завершения; active/candidate credentials хранятся атомарно с checksum и rollback.
Google OAuth не эмулируется: для Google-created account сначала один раз задайте
Bambu-пароль через официальный `Forgot password`.

Повторный login для штатной работы не нужен. Direct Cloud сам обновляет tokens
за 7 дней до expiry (во время печати откладывает до срочного окна 24 часа),
обрабатывает MQTT auth reject и повторяет временные ошибки через
1 мин / 5 мин / 15 мин / 1 ч / 6 ч. Ротированный refresh token и account UID
записываются во flash ESP в отдельный checksum-protected recovery slot до
MQTT-проверки: rollback или reboot продолжит операцию с самым новым token только
для того же аккаунта. Ручной кнопки и API refresh нет. Это сознательный
reliability-over-security выбор security level 0;
повторный login остаётся только для внешнего revoke/блокировки аккаунта или
несовместимого изменения Bambu Cloud contract.

До T05 это shadow path: матрица и основной status WebUI продолжают читать
`RelayHttpSource`, а direct snapshot доступен только в `/api/status.bambuDirect`.
Внешний relay (`tools/bambu_relay/`) остаётся рабочим rollback path.

Запуск, деплой на OpenWrt-роутер, HTTP API и конфигурация relay описаны в
[`tools/bambu_relay/README.md`](tools/bambu_relay/README.md).
Проверка и безопасная замена истёкшего Cloud token — в
[`tools/bambu_relay/TOKEN_ROTATION_RUNBOOK.md`](tools/bambu_relay/TOKEN_ROTATION_RUNBOOK.md).

Со стороны матрицы в веб-интерфейсе (раздел `Принтер`) нужно включить:

- тумблер `Статус печати`
- `Relay URL = http://<хост_relay>:8787/status`
- опционально `Relay Key`, если relay запущен с ключом

Direct Cloud настраивается в соседней карточке того же раздела. Cloud REST,
Cloud MQTT, weather HTTPS/sunrise и OTA проходят через единый координатор:
одновременно на ESP32-C3 существует не более одного TLS-сеанса.
