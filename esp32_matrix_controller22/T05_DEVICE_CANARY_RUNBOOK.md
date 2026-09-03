# T05 device/canary runbook

Этот прогон выполняется только после явного разрешения владельца. Он объединяет
OTA, shadow/canary, fault recovery, rollback и запуск 72-часового soak в одно
присутствие владельца. Пароль, verification/TOTP codes, tokens и print payload
не записываются.

Актуально для 2026-09-03: на матрице production `4.3.12-2026.09.02` (слот
`app0`, flash 1 386 725 B / 70%, globals 49 780 B / 15%), `4.3.11` остаётся на
`app1` как boot-guard fallback; T04 auth acceptance закрыт в AT06, T05 canary
идёт (см. execution record внизу файла).

Подготовленный T05 canary-образ `4.3.12-2026.09.02` (production-сборка,
отличие от `4.3.11` — только bump `FW_VERSION`): linked image `1 386 725` байт
(70%), globals `49 780` байт (15%), экспортированный `.ino.bin` `1 386 880`
байт; gate `<= 1 750 000` выполнен; SHA-256 image
`6d0f1e40e5f461d78e7629741dd6573c044fb064563710cd51877d9d0a4b45b4`; подписан
`tools/sign_ota.sh` (ECDSA-P256, `X-OTA-Signature`), подпись рядом с образом.

## Canary evidence — 2026-09-01 (pre-AT06) и 2026-09-02 (AT06)

- USB-переход с T04 на `4.3.1` и следующий passwordless signed Web OTA прошли;
  слот сменился `app0 -> app1`, неподписанный запрос отклонён `403`.
- После boot guard: minimum internal heap `114 388`, minimum largest block
  `114 676`, reserve loop `3 024`, weather `17 236`, printer `7 488` байт.
  Эти доступные C3 gates проходят; MQTT reserve подтверждается в окне T05.
- AT06 (2026-09-02) устранил прежний `cloudflare_blocked`: дискриминатором был
  `User-Agent: ESP32HTTPClient`, молча подставляемый `HTTPClient` вместо
  отбрасываемого ядром `addHeader`; фикс — `setUserAgent("bambu_network_agent/
  02.07.01.51")`. Device oracle на probe-сборке с production-фиксом:
  `TRANSPORT_PASS` ×2 (см. [AT06-integration](../docs/research/bambu-auth-transport/AT06-integration.md)).
- Полный T04 acceptance выполнен на production `4.3.11`: real login, email
  verification, printer selection, первый валидный Cloud MQTT report
  (`mqttConnected=true`, `firstValidReport=true`, `ready`) — дважды, logout
  (`signed_out`, credentials стёрты) и повторный login до `ready`.
  **В окне T05 password login / verification / printer selection / первый
  direct report повторно не требуются** — достаточно убедиться, что
  `bambuDirect.state=ready`, `configured=true`, `firstValidReport=true`.
- Это не C3 memory/CPU failure и больше не transport blocker: Direct Cloud
  canary и 72-часовой soak разрешены; S3-ветка остаётся только реакцией на
  провал численных C3 gates в T05.

## До окна владельца

1. Собрать production build для ESP32-C3 `min_spiffs` (выполнено:
   `4.3.12-2026.09.02`, метрики и SHA-256 выше), проверить image
   `<= 1 750 000` байт и подписать именно `.ino.bin` штатным ECDSA-P256 ключом.
   Перед загрузкой убедиться в свежести `build/` (см. gotcha в CLAUDE.md) и в
   строке `4.3.12-2026.09.02` внутри образа.
2. Прогнать host tests (8 C++ + AT01 oracle + relay) и WebUI gzip/export check.
   Ничего не загружать на устройство до подтверждения окна.
3. Подготовить каталог вне репозитория для обезличенного soak JSONL.

## Одно окно владельца (примерно 75–90 минут)

1. Владелец подтверждает доступ к матрице и router relay, открывает локальный
   WebUI. Проверить, что AP, WebUI и `/api/*` открыты по security level 0, а OTA
   по-прежнему принимает только корректно подписанный image.
2. Загрузить подписанный T05 image через WebUI OTA. Проверить новый
   `fwVersion 4.3.12-2026.09.02`, смену `app1 -> app0`, стабильные 60 секунд
   boot guard, часы, погоду, яркость, AP и сохранение настроек. Неподписанный
   тестовый image не коммитить; достаточно локальной проверки rejection path до
   `Update.end()`.
3. Login/verification/printer selection/первый direct report уже проверены в
   AT06 на `4.3.11` — повторять не нужно. Достаточно зафиксировать, что после
   OTA `bambuDirect.state=ready`, `configured=true`, `mqttConnected=true`,
   `firstValidReport=true`, и printer остаётся в Cloud Mode и виден в Bambu
   Handy. В diagnostics допустимы только counters/reason codes.
4. Владелец сам запускает одну короткую representative print. На `relay`
   сравнить lifecycle, visible lifecycle, progress, layers, remaining time, job,
   stage и detail; необъяснённые shadow mismatch counters должны остаться на
   нуле.
5. Во время печати запросить weather refresh и sunrise: HTTP может выполниться,
   HTTPS должен быть отложен без остановки MQTT. Затем последовательно проверить
   bounded recovery после Wi-Fi loss, DNS failure, MQTT disconnect и временного
   Bambu API failure. После каждого сбоя устройство должно восстановиться без
   reboot, а прежний overlay сохраняться до валидного `idle`/нового report.
6. Переключить `relay -> cloud_direct` без reboot, сравнить матрицу и WebUI,
   затем `cloud_direct -> relay` как ручной rollback и снова вернуть
   `cloud_direct` для canary. Relay всё это время продолжает shadow polling.
7. Проверить OTA rollback: зафиксировать текущий slot, вызвать boot guard на
   canary slot тремя ранними сбоями, убедиться в возврате на предыдущий signed
   slot, затем повторно установить подписанный T05 image и выдержать boot-guard
   окно. Не менять signed-image, credential или rollback защиту.
8. Снять начальные gates из `/api/status`: minimum internal heap `>= 51 200`,
   minimum largest internal block `>= 32 768`, reserve loop/weather/printer/MQTT
   каждого `>= 2 048` байт. Если любой gate уже провален, прекратить C3 canary,
   сохранить измерения и принять S3; функции и защиты не удалять.
9. Запустить unattended soak (JSONL — строго вне репозитория):

   ```bash
   python3 tools/t05_soak_monitor.py \
     --url http://192.168.1.65 \
     --hours 72 --interval 60 \
     --output "$TMPDIR/ledmatrix-t05-soak.jsonl"
   ```

После запуска владельцу больше не нужно оставаться у устройства.

## Execution record — окно 2026-09-03

Окно подтверждено владельцем 09:21 MSK. Хронология и результаты:

- **Wi-Fi recovery:** матрица была в onboarding AP `ESP32-Matrix-75DC3C`
  (после ошибочного `/api/disconnect` прошлой сессии). Восстановлена на
  `Bibas_2.4` через Wi-Fi модуль pve (`wlp3s0` + wpa_supplicant + dhclient,
  POST `/api/connect`) без участия Mac. После подключения устройство
  перезагрузилось само — это ожидаемое поведение `/api/connect`.
- **Переключения без reboot:** `cloud_direct → relay` (effectiveSource за ~6 с,
  Finding A flap прекратился — relay демотит в idle и остаётся), затем
  API-failure тест (невалидный `bambuRelayUrl` на порт 9): bounded failure
  (`bambuConnected=false`, lifecycle unknown, видимый overlay сохранён,
  статус-эндпоинт жив, без reboot), восстановление URL → `connected=true`,
  lifecycle=idle. Финал: `relay → cloud_direct` за ~6 с, без reboot
  (аптайм непрерывен сквозь все три операции).
- **OTA/boot инцидент:** `/api/reboot` из стабильного 4.3.12 → серия ранних
  крашей в первом ~60 с окне → boot-guard откат на `4.3.11`/`app1`. Повторная
  подписанная OTA `4.3.12` → та же серия → откат. Третья установка —
  стабильно `4.3.12`/`app0`, soak стартовал с неё. Это 3-й эпизод паттерна
  (включая первую установку 2026-09-02); краш интермиттирующий, ограничен
  boot-окном. Root-cause требует сериального лога — **открытый дефект**,
  блокер для закрытия T05 (см. CLAUDE.md, Build & Flash).
- **`/api/update` quirk:** оба OTA-загрузки вернули curl `HTTP 000`
  (обрыв ответа), но образ каждый раз принимался, подпись проверялась и
  коммитился — доверять только `/api/status` после загрузки.
- **Finding A воспроизведён живьём** (read-only замер, 3 мин, опрос 5 с):
  цикл `idle(1)/visible 0` ↔ `finished(5)/visible 5/progress 100` с периодами
  ~15–65 с, shadow mismatches росли 5→10 за 3 мин; суммарно за окно
  mismatch-счётчик дошёл до 45/45, маска причин покрывает lifecycle/visible/
  progress + job_name/stage/detail. Дефект присутствует и в `4.3.11`, и в
  `4.3.12`. Фикс после окна, TDD (см. CLAUDE.md, Bambu Integration).
- **C3 gates (стартовые, после стабилизации 4.3.12):** reserves PASS
  (loop 3 024, weather 17 232, printer 7 160, mqtt 3 148), largest block PASS
  (min 38 900–61 428 vs 32 768), image PASS (1 386 725 ≤ 1 750 000).
  Gate `heapInternalMinimum >= 51 200` — **недостижим в steady state**:
  low-water mark падает до ~46–50 K уже через минуты из-за TLS-хендшейков
  (direct MQTT + weather). Реальный сигнал утечки — `heapMonotonicallyFalling`
  в soak-мониторе. Калибровку гейта решает владелец; S3 по этому гейту
  не инициирован.
- **Soak v1 (упразднён):** 2026-09-03 ~06:43–07:10 UTC на `4.3.12`/`app0`
  вёлся Mac-монитором `tools/t05_soak_monitor.py` (31 сэмпл, JSONL
  `/Users/kesha/Desktop/vibecoding/ledmatrix-t05-soak/ledmatrix-t05-soak.jsonl`)
  — остановлен по требованию автономности, заменён журналом в прошивке (ниже).

- **Soak перенесён в прошивку (автономно, без Mac/pve/роутера):** владелец
  потребовал наблюдение строго на ESP. Внешний монитор остановлен (31 сэмпл
  06:43–07:10 UTC, JSONL — история). Собрана, host-протестирована и прошита по
  USB (`/dev/cu.usbmodem101`, 4.6 с, hash verified) `4.3.13-2026.09.03`:
  журнал в NVS (`soaklog`) — минимумы heap/блоков/стеков, итоги shadow/reconnect,
  аптайм, кольцо из 16 boot-записей с `esp_reset_reason()`; `GET /api/soak`,
  `POST /api/soak/reset`.
- **Корневая причина ранних крашей найдена по сериалу** (захват
  `/Users/kesha/Desktop/vibecoding/ledmatrix-t05-soak/serial_reboot.log`):
  при ранней загрузке прошивка сразу идёт в Bambu Cloud MQTT; если DNS ещё не
  готов (`getaddrinfo() returns 202` для `us.mqtt.bambulab.com`), обработчик
  ошибки mqtt/esp-tls падает в `Store access fault` (вариант: lwIP
  `udp_remove` assert без TCPIP lock) → серия паник в первые ~3–15 с →
  boot-guard откатывает. Холодный старт обычно выигрывает DNS-гонку,
  `/api/reboot` и OTA-ребуты — проигрывают. Тот же дефект ронял устройство
  посреди сетевого OTA (5 из 6 попыток — краш на моменте загрузки, HTTP 000,
  троттлинг не помог). Фикс отдельным TDD-проходом: не коннектиться к MQTT до
  подтверждённых Wi-Fi+DNS, ревизия esp-tls cleanup.
- **Soak перезапущен ~11:00 MSK на 4.3.13/app0:** журнал сброшен
  (`POST /api/soak/reset`), вердикт через `curl http://192.168.1.65/api/soak`
  (доступно и с телефона). Известные косметические дефекты журнала 4.3.13:
  `checkpointCount` всегда 0; после reset дельта mismatch считается от загрузки
  (несколько объяснённых Finding-A mismatch могут быть зачтены).

## Через 72 часа

Проверить, что матрица работала в `cloud_direct`, printer оставался Cloud Mode и
доступным в Handy, были несколько reconnect, а current heap/largest block не
показывают монотонного падения. Финальные ворота: image `<= 1 750 000`, heap
`>= 51 200`, largest block `>= 32 768`, каждый task reserve `>= 2 048`, zero
unexplained shadow mismatch, без регрессий clock/weather/brightness/AP/WebUI/
signed OTA/overlay. При успехе решение — C3. При любом обязательном провале —
решение S3 с сохранением relay rollback и security level 0.
