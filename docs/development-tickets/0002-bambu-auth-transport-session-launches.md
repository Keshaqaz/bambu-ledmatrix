# Запуск auth-transport тикетов в чистых Codex-сессиях

Команды проверены по установленному `codex exec --help` 2026-09-01. Каждый вызов
создаёт отдельную сессию в одном workspace. Запускайте их последовательно по
short-circuit policy из
`docs/development-tickets/0002-bambu-auth-transport-spikes.md`.

```sh
export LEDMATRIX_ROOT=/Users/kesha/Desktop/vibecoding/ledmatrix
```

## AT01 — oracle и baseline

```sh
codex exec -C "$LEDMATRIX_ROOT" --approve-for-me 'Выполни автономно AT01 из docs/development-tickets/0003-bambu-auth-transport-baseline.md, строго следуя ADR 0001 и общему контракту docs/development-tickets/0002-bambu-auth-transport-spikes.md. Используй только synthetic invalid credentials. Для project-owned device deploy действует постоянное разрешение из общего контракта; не запрашивай его повторно. Обязательно создай docs/research/bambu-auth-transport/AT01-baseline.md.'
```

## AT02 — штатные Mbed TLS profiles

Запускать после готового `AT01-baseline.md`.

```sh
codex exec -C "$LEDMATRIX_ROOT" --approve-for-me 'Выполни автономно AT02 из docs/development-tickets/0004-bambu-auth-mbedtls-profiles.md по docs/research/bambu-auth-transport/AT01-baseline.md и общему контракту. Не создавай новый oracle, не используй реальные credentials и не расширяй фиксированную матрицу. Для project-owned device deploy действует постоянное разрешение из общего контракта; не запрашивай его повторно. Запиши результат в docs/research/bambu-auth-transport/AT02-mbedtls.md.'
```

Если AT02 получил стабильный `PASS`, пропустите AT03 и AT04 и запускайте AT05.

## AT03 — auth-only wolfSSL

Запускать только если AT02 не дал `PASS`.

```sh
codex exec -C "$LEDMATRIX_ROOT" --approve-for-me 'Выполни автономно AT03 из docs/development-tickets/0005-bambu-auth-wolfssl-spike.md по AT01-baseline.md и общему контракту. Используй synthetic invalid credentials, сохрани CA/hostname validation и не меняй MQTT/weather/OTA backend. Для project-owned device deploy действует постоянное разрешение из общего контракта; не запрашивай его повторно. Запиши результат в docs/research/bambu-auth-transport/AT03-wolfssl.md.'
```

Если AT03 получил стабильный `PASS`, пропустите AT04 и запускайте AT05.

## AT04 — ClientHello-compatible transport

Запускать только при отсутствии `PASS` AT02/AT03 и документированных
`BLOCKED`/toolchain-resource incompatibility.

```sh
codex exec -C "$LEDMATRIX_ROOT" --approve-for-me 'Выполни автономно AT04 из docs/development-tickets/0006-bambu-auth-clienthello-spike.md только при выполненных prerequisites. Используй существующий AT01 oracle и только зафиксированный wire diff; не используй реальные credentials, не ослабляй TLS и не реализуй cryptography/browser engine самостоятельно. Для project-owned device deploy действует постоянное разрешение из общего контракта; не запрашивай его повторно. Запиши результат в docs/research/bambu-auth-transport/AT04-clienthello.md.'
```

## AT05 — выбор production transport

Запускать после первого `PASS`, либо после завершённого отрицательного AT04.

```sh
codex exec -C "$LEDMATRIX_ROOT" --approve-for-me 'Выполни автономно AT05 из docs/development-tickets/0007-bambu-auth-transport-decision.md. Не повторяй experiments: используй только канонические artifacts, выбери первый PASS по short-circuit order или зафиксируй пересмотр ADR. Синхронизируй ADR/master map и создай docs/research/bambu-auth-transport/AT05-decision.md. Не выполняй deploy или real login.'
```

При отсутствии выбранного кандидата AT06 не запускается.

## AT06 — production integration и T04 auth acceptance

Постоянное разрешение на диагностическое использование Bambu account уже
зафиксировано в общем контракте. Credentials находятся только в ignored-файле
`esp32_matrix_controller22/.secrets/bambu-test.env`; команда ниже не раскрывает
их и не передаёт через arguments.

```sh
codex exec -C "$LEDMATRIX_ROOT" --approve-for-me 'Выполни автономно AT06 из docs/development-tickets/0008-bambu-auth-transport-integration.md по выбранному кандидату AT05. Используй локальный ignored .secrets/bambu-test.env только для разрешённых Bambu diagnostic login/verification/refresh и никогда не выводи credentials. Не запрашивай повторного разрешения на Bambu account или project-owned device deploy; verification code запроси только если он действительно потребуется. Не запускай print или T05 soak. Создай docs/research/bambu-auth-transport/AT06-integration.md.'
```

После успешного AT06 возвращайтесь к существующему T05 prompt из
`docs/development-tickets/0001-bambu-cloud-on-device-prompts.md`.
