---
status: proposed
date: 2026-09-01
source: ../adr/0001-optimize-esp32-c3-and-move-bambu-cloud-on-device.md
---

# Тикеты: автономный Bambu auth transport на ESP

Этот поток устраняет обнаруженный на реальном ESP32-C3 блокер первичной
авторизации: Cloudflare возвращает HTML/`403` до Bambu application response.
Реализованные password, verifyCode и TOTP ветки при этом ещё не достигаются.

Готовые команды для запуска каждого тикета в отдельной чистой сессии находятся
в [session launch guide](./0002-bambu-auth-transport-session-launches.md).

Цель неизменна: первичный login, verification и refresh выполняются самим
устройством. Ручной token, companion script, login на роутере, отключение TLS
certificate validation и скрытый relay fallback не являются успехом.

## Общий oracle

Все transport spikes используют синтетическую невалидную учётную запись и один
критерий:

- `TRANSPORT_PASS` — endpoint вернул Bambu application JSON, независимо от
  ожидаемого отказа credentials;
- `TRANSPORT_BLOCKED` — получена Cloudflare block/challenge page или отдельный
  код `cloudflare_blocked`;
- `TRANSPORT_ERROR` — TLS/DNS/timeout/parse failure, который нельзя ошибочно
  считать ни успехом, ни Cloudflare block.

Probe не печатает request body, cookies, response body или credentials. Разрешены
только status, content type, bounded body classification, TLS/profile ID, heap
metrics и elapsed time. Сертификат сервера проверяется во всех вариантах.

## Карта

| ID | Самодостаточный тикет | Зависимость |
|---|---|---|
| AT01 | [Зафиксировать transport oracle и wire baseline](./0003-bambu-auth-transport-baseline.md) | текущая production build |
| AT02 | [Проверить штатные профили Mbed TLS](./0004-bambu-auth-mbedtls-profiles.md) | AT01 |
| AT03 | [Проверить auth-only wolfSSL](./0005-bambu-auth-wolfssl-spike.md) | AT01 |
| AT04 | [Проверить ClientHello-compatible transport](./0006-bambu-auth-clienthello-spike.md) | AT02/AT03 не дали `PASS` |
| AT05 | [Выбрать и зафиксировать production transport](./0007-bambu-auth-transport-decision.md) | первый `PASS` либо завершённый AT04 |
| AT06 | [Интегрировать transport и закрыть T04 auth acceptance](./0008-bambu-auth-transport-integration.md) | AT05 выбрал кандидата |

По умолчанию применяется short-circuit: AT02 → при отсутствии `PASS` AT03 → при
отсутствии `PASS` AT04. Независимый AT03 можно подготовить после AT01, но его
device-прогон не нужен, если AT02 уже дал стабильный `PASS`. `ERROR` не считается
провалом fingerprint: сессия обязана либо устранить локальную ошибку, либо
документировать несовместимость toolchain/resource, после чего следующий
кандидат разрешён. После AT06 возобновляется T05 canary/soak; его сценарии и
численные ворота не дублируются здесь.

## Контракт артефактов между сессиями

Все результаты создаются в каноническом каталоге
`docs/research/bambu-auth-transport/`:

- `AT01-baseline.md` — oracle command, firmware/toolchain versions, synthetic
  input class, desktop/ESP verdicts, bounded classifier contract, wire metadata
  и redaction check;
- `AT02-mbedtls.md`, `AT03-wolfssl.md`, `AT04-clienthello.md` — tested profile,
  verdict, три повторения для `PASS`, image/RAM delta, certificate validation и
  точная причина `ERROR`/infeasibility;
- `AT05-decision.md` — выбранный кандидат или основание пересмотра ADR;
- `AT06-integration.md` — production commit/image metrics и результаты полного
  T04 auth acceptance.

Каждый downstream ticket читает только явно перечисленные файлы. Если требуемый
artifact отсутствует или не содержит oracle command/verdict/redaction check,
сессия сообщает один blocker и не создаёт новый параллельный harness.

## Общие ограничения

- Не использовать реальные Bambu credentials: transport oracle не требует их.
- Не выполнять deploy/OTA без явного разрешения пользователя.
- Не добавлять универсальный mock framework или browser/JavaScript runtime.
- Не отключать CA/hostname validation и не ослаблять signed image/rollback.
- Не менять production source selector на основании одного transport response.
- Не расширять каждый spike до verification, MQTT, print или 72-hour soak.
- Сохранять security level 0 только для локального AP/WebUI/API; он не ослабляет
  внешний Bambu TLS contract.
- Если кандидат не проходит flash/RAM gates без удаления функций, записать
  измерения; S3 рассматривается только как ресурсный fallback, а не как способ
  автоматически изменить Cloudflare fingerprint.

## Тестовый Bambu account и постоянное разрешение

Владелец проекта 2026-09-01 разрешил без повторного запроса использовать его
тестовый Bambu account в диагностических целях этого проекта. Credentials лежат
только локально в ignored-файле с mode `0600`:

`esp32_matrix_controller22/.secrets/bambu-test.env`

Файл экспортирует `BAMBU_TEST_EMAIL` и `BAMBU_TEST_PASSWORD`. Значения запрещено
переносить в tracked Markdown, Git, command arguments, terminal output, logs,
diagnostics, browser storage или artifacts. Инструмент должен читать их из
environment/stdin, а пароль — затирать сразу после login operation.

Постоянное разрешение покрывает read-only диагностический login, отправку и ввод
verification/TOTP code, account/profile/printer discovery, token refresh и Cloud
MQTT telemetry. Оно не разрешает запуск реальной печати, bind/unbind принтера,
изменение password/MFA/account settings, удаление Cloud data, deploy или OTA.
Для этих действий по-прежнему требуется отдельное явное разрешение. Если Bambu
потребует verification code, сессия сначала доводит всё до готовности и просит у
пользователя только сам код/подтверждение в одном окне.

AT01–AT04 используют synthetic invalid credentials и не читают этот файл: это
делает transport verdict воспроизводимым и исключает лишние login attempts.
AT06 использует файл только после локального build/tests и разрешённого device
deploy; отдельного разрешения на сам Bambu login больше не запрашивает.
