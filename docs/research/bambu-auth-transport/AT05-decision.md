# AT05 — Выбор production auth transport

Date: 2026-09-02

Status: decided.

Inputs (канонические, других не использовано):
[AT01-baseline.md](./AT01-baseline.md),
[AT02-mbedtls.md](./AT02-mbedtls.md),
[AT03-wolfssl.md](./AT03-wolfssl.md),
[AT04-clienthello.md](./AT04-clienthello.md).
Эксперименты не повторялись; deploy, real login, MQTT, canary и soak не
выполнялись. ADR 0001 дополнен решением (amended 2026-09-02); master map в
`docs/development-tickets/0002-bambu-auth-transport-spikes.md` синхронизирован.

## Проверка входов

- Все четыре артефакта содержат обязательные элементы контракта: oracle
  command, tested profile, verdict, redaction check; AT04 содержит ≥3
  повторения `PASS` на нескольких профилях (требование закрыто с запасом);
  AT02/AT03 содержат точные причины `ERROR`/infeasibility.
- Scan артефактов: credentials, cookies, tokens, operation IDs, raw
  request/response bodies отсутствуют; использован только фиксированный
  synthetic `.invalid` input class; в AT03 присутствуют только публичные
  self-signed корневые сертификаты (SHA-256 fingerprint) из поддерживаемых
  источников.
- Кандидат существует, поэтому ветка «пересмотр ADR» (шаг 6 тикета) не
  применяется. Ослабления TLS/credentials/signed OTA/rollback нигде не
  потребовалось; S3 как «fingerprint-фикс» не предлагался ни одним артефактом.

## Сводка кандидатов по short-circuit order

Предпочтение по тикету: штатный Mbed TLS profile → официальный auth-only
wolfSSL adapter → минимальный ClientHello-compatible adapter. Более сложный
кандидат не выбирается только из-за более близкого fingerprint, если более
простой уже даёт стабильный Bambu application response и проходит resource
projection.

| Кандидат | Verdict | Certificate validation | Scope | Flash / RAM | Maintenance risk |
|---|---|---|---|---|---|
| AT02 — штатные профили Mbed TLS/ESP-TLS (profiles 0–4) | `NO PASS`: 0/1/3 воспроизводимо `BLOCKED`, profile 2 — локальный `TRANSPORT_ERROR` (plumbing до ClientHello), profile 4 не имеет допустимой комбинации | CA + hostname/SNI включены во всех применимых профилях | только auth transport, production не изменён | production 1,386,023 B / 49,780 B globals — без изменений | n/a — не выбран |
| AT03 — auth-only wolfSSL 5.8.4 | `NO PASS`: после исправления CA-цепочки воспроизводимо `TRANSPORT_BLOCKED` ×2 — первый валидный fingerprint verdict, провал доказан на уровне ClientHello | `WOLFSSL_VERIFY_PEER` + `wolfSSL_check_domain_name` + SNI, fail-closed | auth-only; GPLv3 + официальный компонент требует ESP-IDF 6.x (проект на 5.5.x) | +231 KB flash (82% слота), +920 B globals | не выбран независимо от вердикта: лицензия и toolchain-несовместимость |
| AT04 — минимальный ClientHello-compatible (фактически: stock-Mbed TLS адаптер + фиксация wire) | `PASS` ×13 (profiles 8×4, 9×3, 10×2, 6×2, 7×1, 11×1); profile 12 — контрольный `BLOCKED` | `MBEDTLS_SSL_VERIFY_REQUIRED` + CA bundle + `set_hostname`, не ослаблены | auth-only, probe-isolated; MQTT/weather/OTA/selector не затронуты | probe 1,406,617 B; прогноз интеграции адаптера ≈ +5–10 KB flash, +0 B globals; heap min 43,480 B, largest block 61,428 B, CPU ≤45.3 °C | зависимость от wire-политики Cloudflare; зависимость от HTTPClient allow-list при обновлениях core |

Ключевой результат AT04, меняющий основу решения: дискриминатором Cloudflare
был **не TLS ClientHello**, а одна HTTP-строка — `User-Agent: ESP32HTTPClient`,
которую `HTTPClient` молча подставляет вместо отбрасываемого
`addHeader("User-Agent", …)` (allow-list ядра исключает User-Agent). Доказано
контрольной парой на байт-идентичном ClientHello и wrapper-порядке заголовков:
profile 12 (UA `ESP32HTTPClient`) → `BLOCKED`, profile 10 (тот же wire, другой
UA) → `PASS`. Заявленная production-идентичность
`bambu_network_agent/02.07.01.51` никогда реально не отправлялась, хотя
остальные `X-BBL-*` заголовки её уже декларируют.

## Применение правила выбора

- AT02 — первого кандидата — `PASS` не получил: штатный профиль блокируется при
  фактическом wire UA `ESP32HTTPClient`.
- AT03 — второго кандидата — `PASS` не получил: wolfSSL блокируется по
  fingerprint сам по себе (плюс независимые GPLv3/toolchain основания отказа).
- AT04 — единственный `PASS`. Однако его результат означает, что после
  исправления UA пропускную способность возвращает **самый простой** кандидат —
  штатный стек: адаптер AT04 имеет ClientHello, байт-идентичный production
  wrapper'у, и profile 10 воспроизводит wrapper-порядок заголовков с `PASS`.
  По правилу («более простой, дающий стабильный response и проходящий resource
  projection, предпочтительнее») минимальная форма кандидата AT04 — исправление
  UA на неизменном штатном Mbed TLS/HTTPClient стеке — предпочтительнее полного
  адаптера: resource projection тривиален (одна строка, flash ≈ +0, TLS-стек и
  certificate validation без изменений), а поверхность сопровождения меньше.

## Решение

**Выбранный production transport: действующий штатный Mbed TLS/HTTPClient
стек (замена TLS-стека не выполняется) с восстановлением заявленной
идентичности клиента — `User-Agent: bambu_network_agent/02.07.01.51`,
устанавливаемого через `HTTPClient::setUserAgent(...)` вместо молча
отбрасываемого `HTTPClient::addHeader(...)` (текущее место —
`esp32_matrix_controller22/src/bambu_cloud.cpp:638`).** Всё остальное
production auth path не меняется: CA/hostname validation, таймауты, порядок
заголовков, classifier, password erasure, active/candidate/recovery
transaction, single-TLS coordination, signed OTA и rollback.

Уточнение к правилу ADR 0001 «подмена только HTTP headers не считается
решением»: правило было сформулировано в предположении TLS-fingerprint
блокера и защищало от спуфинга произвольных заголовков как замены
transport-квалификации. AT04 измеренно доказал, что блокером была именно
испорченная заголовком идентичность клиента. Коррекция принята только в
совокупности: (а) она восстанавливает идентичность, уже заявленную остальными
`X-BBL-*` заголовками production-кода, а не вводит произвольное значение;
(б) `PASS` измерен оракулом 13 раз; (в) реализация обязана пройти полные
ворота — T04 auth acceptance (AT06), затем T05 canary/soak и C3 resource
gates. Отключение certificate validation по-прежнему запрещено.

Fallback (не является target'ом AT06): прямой stock-Mbed TLS адаптер AT04
(измеренный `PASS` ×13, включая profile 8 с default cipher list) применяется
только если wrapper-путь не воспроизведёт `PASS` на устройстве; такой исход
фиксируется как один blocker в AT06 и требует addendum к настоящему решению,
после чего адаптер становится единственным target'ом.

## AT06 target (единственный production implementation/acceptance target)

- Implementation: единственное изменение production HTTPS auth path —
  корректная установка `User-Agent` (`http.setUserAgent("bambu_network_agent/02.07.01.51")`
  с удалением неэффективного `addHeader("User-Agent", …)`) в login/
  verification/refresh запросах `BambuCloud`. Probe-only код, capture-режимы и
  лишние профили в production не переносятся.
- Verification ladder (по порядку):
  1. host tests: существующие C++ auth seam tests и Python oracle tests —
     зелёные; новый host-тест не обязателен, но допустим (фиксация, что UA
     задаётся через `setUserAgent`, а не `addHeader`);
  2. production build: gate ≤ 1,750,000 B тривиально выполняется (baseline
     `4.3.7`: 1,386,023 B / 49,780 B globals); метрики фиксируются до device-
     прогона;
  3. device oracle: `TRANSPORT_PASS` командой AT01 на probe-сборке с
     production UA-фиксом (parsed-JSON proof по контракту оракула);
  4. собственная работа AT06: signed OTA и T04 real acceptance (invalid
     credentials → direct login → серверная verification/TOTP ветка → выбор
     принтера → первый валидный MQTT report → logout → повторный login) с
     локальным ignored `esp32_matrix_controller22/.secrets/bambu-test.env`.
- Неизменные ограничения: certificate/hostname validation, credential erasure,
  active/candidate/recovery transaction, `SecureNetworkCoordinator`
  (одновременных TLS-сеансов не более одного), signed image + boot-guard
  rollback. MQTT/weather/OTA transport не меняются.
- Регрессионный детектор: пара oracle-профилей AT04 — 10 (`PASS`) /
  12 (`BLOCKED`) — ранний сигнал, если Cloudflare начнёт блокировать и
  заявленный UA; выполняется после обновлений toolchain или при подозрении на
  возврат блоков.
- T05 остаётся заблокированным до полного завершения AT06 (production
  integration + полный T04 login/verification acceptance). Матрица T05 и
  численные C3 gates не расширены и не ослаблены настоящим решением.

## Риски и условия пересмотра

- UA-правило Cloudflare может иметь ограниченный срок: mitigations —
  регрессионный детектор 10/12, документированный fallback-адаптер, условия
  пересмотра ADR 0001.
- Поведение `HTTPClient` (allow-list заголовков, значение `_userAgent` по
  умолчанию) может измениться в будущих версиях Arduino ESP32 core — device
  oracle обязателен после обновления core.
- Зависимость от недокументированного Cloud endpoint уже принята ADR 0001;
  появление поддерживаемого партнёрского контракта Bambu заменяет adapter без
  изменения `PrinterStatusSource`.

## Статусы

| Тикет | Статус |
|---|---|
| AT01 | complete — desktop `PASS`, production ESP `BLOCKED`, oracle и wire baseline |
| AT02 | complete — `NO PASS` (`BLOCKED` / локальный `ERROR`) |
| AT03 | complete — `NO PASS` (воспроизводимо `BLOCKED`, fingerprint-level) |
| AT04 | complete — `PASS` ×13, дискриминатор: `User-Agent: ESP32HTTPClient` |
| AT05 | decided 2026-09-02 — настоящее решение |
| AT06 | разблокирован; один target — см. выше |

## Redaction check

- Документ использует только агрегаты AT01–AT04; credentials, cookies, tokens,
  operation IDs, raw bodies отсутствуют; input class — synthetic `.invalid`.
- Deploy, real login, MQTT, print, canary, soak не выполнялись.
- Изменения в этом коммите — документация только: AT05-decision, ADR 0001
  (amended), master map и статусы тикетов AT01–AT05; production-код не
  изменялся.
