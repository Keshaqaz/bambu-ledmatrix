# T05 device/canary runbook

Этот прогон выполняется только после явного разрешения владельца. Он объединяет
OTA, Bambu authorization, shadow/canary, fault recovery, rollback и запуск
72-часового soak в одно присутствие владельца. Пароль, verification/TOTP codes,
tokens и print payload не записываются.

Локально подготовленный `4.3.3-2026.09.01`: linked image `1 385 857` байт,
экспортированный `.ino.bin` `1 386 000` байт, globals `49 780` байт; SHA-256
image `59834aa4102695c20e5d4fab3a8d819de9153fb9e6c0508e12419042eef8983e`.

## Canary evidence — 2026-09-01

- USB-переход с T04 на `4.3.1` и следующий passwordless signed Web OTA прошли;
  слот сменился `app0 -> app1`, неподписанный запрос отклонён `403`.
- После boot guard: minimum internal heap `114 388`, minimum largest block
  `114 676`, reserve loop `3 024`, weather `17 236`, printer `7 488` байт.
  Эти доступные C3 gates проходят; MQTT reserve ожидает рабочую Cloud session.
- Реальный initial password login на `4.3.3` классифицирован как
  `cloudflare_blocked` до email/SMS/TOTP state. Обезличенный differential probe
  с Mac, тем же endpoint, HTTP/1.0 и header set дошёл до Bambu API и получил
  ожидаемый `400` для фиктивного аккаунта. Значит Cloudflare отклоняет сетевой
  fingerprint ESP32 до проверки credentials.
- Это не C3 memory/CPU failure: перенос того же firmware/TLS stack на S3 не
  устранит блокировку. Direct Cloud canary и 72-часовой soak не запускаются до
  отдельного решения для поддерживаемого Cloud login transport.

## До окна владельца

1. Повторить production build для ESP32-C3 `min_spiffs`, проверить image
   `<= 1 750 000` байт и подписать именно `.ino.bin` штатным ECDSA-P256 ключом.
2. Повторить host tests и WebUI gzip/export check. Записать SHA-256 image и
   подписи. Ничего не загружать на устройство.
3. Подготовить каталог вне репозитория для обезличенного soak JSONL.

## Одно окно владельца (примерно 75–90 минут)

1. Владелец подтверждает доступ к матрице и router relay, открывает локальный
   WebUI. Проверить, что AP, WebUI и `/api/*` открыты по security level 0, а OTA
   по-прежнему принимает только корректно подписанный image.
2. Загрузить подписанный T05 image через WebUI OTA. Проверить новый
   `fwVersion`, смену `app0/app1`, стабильные 60 секунд boot guard, часы,
   погоду, яркость, AP и сохранение настроек. Неподписанный тестовый image не
   коммитить; достаточно локальной проверки rejection path до `Update.end()`.
3. Оставить renderer source `relay`. Владелец сам вводит Bambu password и
   verification/TOTP code, явно выбирает printer. Убедиться, что printer
   остаётся в Cloud Mode и виден в Bambu Handy. Проверить первый direct report;
   в diagnostics допустимы только counters/reason codes.
4. Запустить одну короткую representative print. На `relay` сравнить lifecycle,
   visible lifecycle, progress, layers, remaining time, job, stage и detail;
   необъяснённые shadow mismatch counters должны остаться на нуле.
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
9. Запустить unattended soak:

   ```bash
   python3 tools/t05_soak_monitor.py \
     --url http://192.168.1.65 \
     --hours 72 --interval 60 \
     --output /tmp/ledmatrix-t05-soak.jsonl
   ```

После запуска владельцу больше не нужно оставаться у устройства.

## Через 72 часа

Проверить, что матрица работала в `cloud_direct`, printer оставался Cloud Mode и
доступным в Handy, были несколько reconnect, а current heap/largest block не
показывают монотонного падения. Финальные ворота: image `<= 1 750 000`, heap
`>= 51 200`, largest block `>= 32 768`, каждый task reserve `>= 2 048`, zero
unexplained shadow mismatch, без регрессий clock/weather/brightness/AP/WebUI/
signed OTA/overlay. При успехе решение — C3. При любом обязательном провале —
решение S3 с сохранением relay rollback и security level 0.
