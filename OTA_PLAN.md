# План перехода прошивки на OTA (схема min_spiffs)

Дата: 2026-06-11. Статус: реализовано и полностью проверено на живом
устройстве: USB-переход с сохранением NVS, OTA app0↔app1 обоими путями
(/api/update и espota), отказ bootloader/merged.bin, безвредный обрыв
загрузки, авто-rollback при boot-loop (три живых срабатывания). Оба слота
содержат рабочий образ 3.1.0-2026.06.11, устройство работает из app1.

## Контекст и почему прошлая попытка провалилась

Сейчас обновление только по USB: схема `huge_app`, OTA отключена. Архивный
PROJECT_CONTEXT.md фиксирует размер сборки на момент того решения: **1 408 041
байт** — это больше слота стандартной OTA-схемы `default` (0x140000 =
1 310 720 байт). OTA тогда физически не влезала → «Sketch too big» → huge_app.

Что изменилось / что выяснено сейчас:

- Свежий бинарник arduino-cli — **1 293 136 байт**. Схема **`min_spiffs`** даёт
  два OTA-слота по **0x1E0000 = 1 966 080 байт** → заполнение ~66% (с кодом
  OTA ожидаемо ~1.34 MB ≈ 68%). Схема `default` по-прежнему запрещена.
- NVS в `min_spiffs` на том же месте (0x9000/0x5000), что и сейчас → конфиг
  переживёт одноразовую USB-перепрошивку.
- arduino-cli с FQBN `huge_app` использует CSV из ядра esp32 3.3.8, где **уже
  есть otadata на 0xe000 и app0 типа ota_0** — раскладка почти совпадает,
  меняется только нарезка хвоста флеша. (Локальный partitions_huge_app.csv с
  phy_init использовался только PlatformIO; PHY-данные на ядре 3.x встроены в
  приложение, отдельная партиция не нужна.)
- platform.txt 3.3.8: USB-заливка пишет `boot_app0.bin` на 0xe000 → otadata
  инициализируется при первой же USB-прошивке, ничего вручную не делается.
- boards.txt: `min_spiffs.upload.maximum_size=1966080` (arduino-cli сам упадёт
  при превышении) и `upload.tool.network=esp_ota` → `arduino-cli upload
  --protocol network` (espota, порт 3232) работает из коробки.

Гарантия безопасности самого механизма: OTA пишет в **неактивный** слот;
работающая прошивка не трогается. Переключение otadata происходит атомарно в
`Update.end()`. До этого момента обрыв питания/связи безвреден — устройство
загрузит старую прошивку.

## Принятые решения

- Эндпоинт OTA **без аутентификации** (как остальные API).
- Анти-кирпич rollback на boot-счётчике (3 сбоя подряд → откат на другой слот).
- Два способа заливки: веб-UI (`/api/update`) и espota
  (`arduino-cli upload --protocol network -p 192.168.1.65`).

## Файлы к изменению

1. `esp32_matrix_controller22/src/app.cpp` — весь код OTA
2. `esp32_matrix_controller22/src/index_html.h` → экспорт в `web/index.html`
3. `esp32_matrix_controller22/platformio.ini` — `board_build.partitions = min_spiffs.csv`
4. `esp32_matrix_controller22/partitions_huge_app.csv` — удалить
5. `esp32_matrix_controller22/esp32_matrix_controller22.ino` — комментарий-шапка
6. `esp32_matrix_controller22/README.md`, `CLAUDE.md` — новый FQBN, раздел OTA,
   убрать «Do not reintroduce OTA slots»

## Шаг 1. app.cpp: includes, версия, состояние

- После `#include <esp_wifi.h>` (~:33): `<Update.h>`, `<ArduinoOTA.h>`,
  `<esp_ota_ops.h>`, `<esp_app_format.h>`.
- `static constexpr const char* FW_VERSION = "3.1.0-2026.06.11";` рядом с
  `CONFIG_VERSION` (~:91). Поднимать при каждой итерации — по нему проверяем,
  что OTA применилась.
- Рядом с `rebootPending`/`rebootMs` (app.cpp:319–320):
  `static std::atomic<bool> otaInProgress{false};`,
  `BOOT_GUARD_MAX_FAILS = 3`, `BOOT_GUARD_STABLE_MS = 60000`,
  `static bool bootGuardCleared = false;`

## Шаг 2. Boot-guard (анти-кирпич)

Отдельный Preferences namespace `"bootguard"` — StoredStateBlob не трогаем.

- **Инкремент** — в `setup()` сразу после `Serial.begin` (app.cpp:2600), до
  FastLED/loadConfig (задач ещё нет, мьютекс не нужен): читать `cnt`, +1;
  если уже было ≥3 сбоев — `esp_ota_set_boot_partition(
  esp_ota_get_next_update_partition(NULL))`, обнулить счётчик, `ESP.restart()`.
  `esp_ota_set_boot_partition` сам верифицирует образ: на пустом app1 (сразу
  после USB-флеша) вернёт ошибку → логируем «откат невозможен», обнуляем
  счётчик, работаем дальше. Обнуление до restart обязательно — иначе пинг-понг
  откатов.
- **Сброс** — в `loop()` после блока отложенного reboot (~:2771): один раз при
  `millis() >= 60000` обнулить под `prefsMtx` (app.cpp:347). Критерий — только
  аптайм (не Wi-Fi: AP-режим тоже валидная прошивка).
- Компромисс (задокументировать): 3 ручных ресета подряд в первые 60 с тоже
  выглядят как сбои и вызовут откат. Это безопасно (на втором слоте предыдущая
  рабочая прошивка либо отказ отката), но поведение надо знать.

## Шаг 3. `/api/update` — веб-загрузка

Регистрация рядом с `/api/reboot` (app.cpp:2722):
`server.on("/api/update", HTTP_POST, handleUpdateFinal, handleUpdateUpload);`
Реализация после `handleReboot()` (~:2366).

`otaEnterUpdateMode()`:
- `otaInProgress = true`
- `clearMatrix(); FastLED.show();` (clearMatrix — app.cpp:939) — гасим RMT/LED
  на время записи флеша и снижаем энергопотребление
- `esp_wifi_set_ps(WIFI_PS_NONE);` — прошивка в steady state держит
  `WIFI_PS_MIN_MODEM` (app.cpp:2670), на одноядерном C3 power save замедляет и
  рвёт приём при записи флеша

`otaAbortUpdateMode()` (вызывать при любой ошибке/обрыве):
- `otaInProgress = false; esp_wifi_set_ps(WIFI_PS_MIN_MODEM);`

**Upload-callback** (`server.upload()`):
- `UPLOAD_FILE_START`: `otaEnterUpdateMode()`, `flushDeferredSave(true)`
  (app.cpp:525); размер берём из query-параметра: веб-UI знает `file.size` и
  передаёт его как `/api/update?size=N` → `Update.begin(N)` валидирует
  влезание в слот **до** первой записи; если параметра нет —
  `Update.begin(UPDATE_SIZE_UNKNOWN)`.
- `UPLOAD_FILE_WRITE`, первый чанк — **защита от merged.bin** (критично:
  arduino-cli кладёт рядом `*.ino.bin` и `*.ino.merged.bin`; merged тоже
  начинается с 0xE9 и влезает в слот — Update примет его и слот окажется
  битым). Проверяем, что это образ приложения, а не бутлоадера:
  `buf[0] == 0xE9` и `*(uint32_t*)(buf + 0x20) == ESP_APP_DESC_MAGIC_WORD`
  (0xABCD5432 — у app-образа по смещению 0x20 лежит esp_app_desc_t, у
  bootloader/merged его нет). Иначе — `Update.abort()` с понятной ошибкой
  «это не app-образ; загружайте esp32_matrix_controller22.ino.bin».
  Дальше обычный `Update.write()`.
- `UPLOAD_FILE_END`: `Update.end(true)`.
- `UPLOAD_FILE_ABORTED`: `Update.abort()`, `otaAbortUpdateMode()` — матрица и
  задачи оживают.

**Final-handler**:
- `checkAuth()` для единообразия (всегда true, app.cpp:1998);
- ошибка (`Update.hasError() || !Update.isFinished()`) → 500 c JSON
  `{ok:false, error: Update.errorString()}`, `Update.abort()`,
  `otaAbortUpdateMode()`;
- успех → обнулить bootguard-счётчик (под `prefsMtx` — мы в loop task, но для
  единообразия), ответить `{ok:true}`, затем `rebootMs.store(millis());
  rebootPending.store(true)` — тот же паттерн отложенного ребута, что в
  `handleReboot` (app.cpp:2354/2365) — ответ успевает уйти клиенту.

Рендер LED живёт в `loop()` → на время синхронного `server.handleClient()` он
останавливается сам, дополнительной синхронизации не нужно.

## Шаг 4. Заморозка фоновых задач

Первой строкой `for(;;)` в `weatherTask` (app.cpp:2503) и `bambuTask`
(app.cpp:2523):
`if (otaInProgress.load()) { vTaskDelay(200 / portTICK_PERIOD_MS); continue; }`
— чтобы TLS-handshake (~12 KB heap) не стартовал посреди записи флеша.
`wifiConnTask` не трогаем (простаивает без `wifiConnPending`).

## Шаг 5. ArduinoOTA (espota)

Init в `setup()` после `server.begin()` (app.cpp:2725):

- `ArduinoOTA.setHostname("esp32-matrix"); ArduinoOTA.setMdnsEnabled(false);`
  (заливаем по IP, mDNS не нужен — экономим heap);
- `onStart` → `otaEnterUpdateMode()` + `flushDeferredSave(true)`;
- `onEnd` → обнулить bootguard-счётчик (espota ребутит устройство сам, final
  handler'а у него нет — иначе накопленный счётчик уедет в новую прошивку);
- `onError` → `otaAbortUpdateMode()`;
- `ArduinoOTA.begin();`

`ArduinoOTA.handle()` в `loop()` рядом с `server.handleClient()` (~:2775).
Конфликтов с WebServer нет (3232 vs 80); во время приёма handle() блокирует
loop — рендер и веб-сервер останавливаются сами. espota шлёт готовый app-образ
(`.ino.bin`), ловушки merged.bin тут нет.

## Шаг 6. `/api/status`: диагностика

В `handleStatus()` (~:2181, рядом с uptime):
`doc["fwVersion"] = FW_VERSION;`
`doc["fwPartition"] = esp_ota_get_running_partition()->label;` — "app0"/"app1",
видно фактическую смену слота после OTA.

## Шаг 7. Веб-UI (index_html.h)

Карточка «Обновление прошивки» во вкладке `Настройки → Устройство`
(`settings-device`), после карточки «Управление устройством»:

- `<input type="file" accept=".bin">`, кнопка, прогресс-бар, текущий
  `fwVersion` (заполняется из `fetchStatus()`).
- Подсказка в карточке: загружать `esp32_matrix_controller22.ino.bin`
  (сборка min_spiffs), **не** `merged.bin`.
- `uploadFirmware()`: `confirm()` с предупреждением; **XHR + FormData** (не
  fetch — нужен `xhr.upload.onprogress`); URL `/api/update?size=` + `file.size`;
  `xhr.timeout` ≥ 300 000 мс (загрузка ~1.3 MB через ESP идёт десятки секунд);
  на время загрузки отключить кнопку и **остановить polling /api/status**
  (WebServer однопоточный, конкурирующие запросы замедлят приём).
- `onload` 200 → «Прошивка загружена, перезагрузка…», далее опрос
  `/api/status` каждые 2 с до смены `fwVersion` → «Обновлено до X»;
  500 → показать `error` из JSON, возобновить polling.
- `onerror`/`ontimeout` → «Связь потеряна — если загрузка не завершилась,
  устройство продолжит работать на старой прошивке», возобновить polling.
- `PREVIEW_MODE`: замокать прогресс таймером (XHR идёт мимо `apiFetch`),
  в `mockStatus()` добавить `fwVersion:'preview'`.

После правок обязательно: `sh tools/export_web_ui.sh`, затем
`sh tools/check_web_ui_export.sh`.

## Шаг 8. Сборка и доки

- FQBN везде: `esp32:esp32:esp32c3:PartitionScheme=min_spiffs`.
- Сборку вести с `--export-binaries` (или `--output-dir build`), чтобы путь к
  `.ino.bin` был предсказуем для веб-загрузки.
- `platformio.ini`: `board_build.partitions = min_spiffs.csv` (CSV из ядра,
  локальная копия не нужна); локальный `partitions_huge_app.csv` удалить.
- README + CLAUDE.md: раздел OTA — два способа, boot-guard (namespace
  `bootguard`, 3 сбоя → откат, компромисс тройного ресета), лимит бинарника
  1 966 080 байт, запрет схемы `default` (исторический провал: 1.4 MB > 1.31 MB),
  предупреждение про merged.bin, одноразовая USB-прошивка для смены таблицы.
  Убрать «Do not reintroduce OTA slots» и упоминания huge_app.

## Порядок внедрения и проверки

1. Все правки кода/доков → `arduino-cli compile --fqbn
   esp32:esp32:esp32c3:PartitionScheme=min_spiffs --export-binaries .` —
   убедиться: размер < 1 966 080 (ожидаемо ~1.34 MB).
2. Одноразовая USB-прошивка `-u -p /dev/cu.usbmodem101`. Serial: конфиг
   загрузился из NVS (Wi-Fi/настройки живы), `[BootGuard] boot count = 1`,
   через 60 с сброс. `/api/status`: `fwVersion`, `fwPartition:"app0"`.
3. **OTA A→B**: поднять FW_VERSION, собрать, залить через веб-UI
   (прогресс-бар) → после ребута `fwPartition:"app1"`, новый `fwVersion`.
4. **OTA B→A**: ещё раз поднять версию, залить
   `arduino-cli compile -u --protocol network -p 192.168.1.65 ...` →
   `fwPartition:"app0"`. Оба слота проверены на запись и загрузку.
5. **Защита от merged.bin**: попытаться загрузить `*.merged.bin` через веб-UI —
   должен прийти отказ «не app-образ», устройство живо.
6. **Rollback**: тестовый бинарник с `abort()` через 5 с в `loop()`, залить по
   OTA; по Serial — 3 краш-ребута, на 4-й `[BootGuard] откат`, старт
   предыдущей прошивки с другого слота, `/api/status` подтверждает старый
   `fwVersion`.
7. **Негативный rollback**: на свежем USB-флеше (app1 пуст) 3 быстрых ресета →
   лог «откат невозможен», устройство продолжает работать.
8. **Обрыв**: прервать веб-загрузку на середине (закрыть вкладку) — матрица
   возобновляет рендер (`UPLOAD_FILE_ABORTED` → `otaAbortUpdateMode`),
   повторная загрузка проходит.
9. Перед коммитом: `sh tools/check_web_ui_export.sh`.
