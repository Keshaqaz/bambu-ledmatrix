# AT06 — Интеграция transport и закрытие T04 auth acceptance

Date: 2026-09-02

Status: complete. Production auth path использует выбранный AT05 transport-фикс
(`User-Agent: bambu_network_agent/02.07.01.51` через `setUserAgent`), device
oracle вернул `TRANSPORT_PASS` ×2, полный T04 login/verification acceptance
пройден на production-сборке, image gate ≤ 1 750 000 B выполнен. T05
разблокирован. Deploy, real login, verification, printer selection, MQTT и
logout выполнены в рамках постоянного разрешения владельца
(project-owned device, 2026-09-02); print и T05 canary/soak не запускались.

## Входы и target

- Target — единственный из [AT05-decision](./AT05-decision.md): штатный
  Mbed TLS/HTTPClient стек без замены TLS, восстановление заявленной
  идентичности клиента `User-Agent: bambu_network_agent/02.07.01.51` через
  `HTTPClient::setUserAgent(...)` вместо молча отбрасываемого
  `addHeader("User-Agent", ...)`; всё остальное production auth path не менялся.
- Oracle — [AT01-baseline](./AT01-baseline.md); probe-only расширения AT02–AT04
  (профили 0–12) в production не переносятся, диагностический код остаётся
  изолированным за `BAMBU_AUTH_TRANSPORT_PROBE`.
- Credentials — локальный ignored `esp32_matrix_controller22/.secrets/bambu-test.env`
  (mode `0600`), читался процессами диагностики in-process, в argv/env шелла не
  передавался, в вывод не попадал.

## Implementation (production `src/bambu_cloud.cpp`)

Целевое изменение AT05 плюс три минимальных фикса, потребованных первым
реальным достижением Bambu application state (все пути до UA-фикса не доходили
из-за Cloudflare-блока и были латентно сломаны; каждый фикс измерен
bounded-диагностикой probe-сборки):

1. **UA-фикс (target AT05).** В `cloudJsonRequestWithClient` — единственной
   HTTPS-точке login/verification/refresh/preferences/bind — заголовок
   идентичности устанавливается `http.setUserAgent("bambu_network_agent/02.07.01.51")`.
   Проверено по ядру 3.3.8: `addHeader("User-Agent")` отбрасывается allow-list
   (`HTTPClient.cpp:992`), `sendHeader` пишет `_userAgent` (дефолт
   `ESP32HTTPClient`, `HTTPClient.cpp:1150`). TLS-стек, CA/hostname validation,
   таймауты, порядок заголовков, classifier, credential transaction,
   single-TLS coordination — без изменений.
2. **Пустой 200 — валидный успех.** Измерено на устройстве:
   `POST /v1/user-service/user/sendemail/code` отвечает HTTP 200 с
   `Content-Length: 0` (диагностика probe-сборки: `transport200Length=0`,
   `transport200Read=0`, `transport200Error=1` = ArduinoJson `EmptyInput`).
   Ранее это давало `bad_response` и ломало verification-ветку. Фикс: пустой
   bounded 200 (code 200, 0 байт, без overflow) принимается как пустой JSON
   документ; data-bearing эндпоинты (login envelope, preference, bind,
   refresh) по-прежнему fail-closed на собственных проверках формы.
3. **`uid` принимается числом.** `GET /v1/design-user-service/my/preference`
   возвращает root-объект с ключами `uid, name, handle, avatar, bio, links,
   backgroundUrl, designIds` (захвачены только имена ключей, значения не
   сохранялись), при этом `uid` — целое число, а не строка: извлечение через
   `firstString` давало `account_identity_missing`. Фикс: fallback-извлечение
   неотрицательного целого (`uid`/`userId`/`user_id`/`accountId`/`account_id`)
   в строковый буфер.
4. **Logout фаза.** Обнаружено: `logout` полностью стирает credentials (NVS,
   RAM, recovery), но фаза «залипала» в `ready` — `service()` видел устаревший
   `source_.firstValidReport()` и каждый проход возвращал
   `BAMBU_AUTH_READY` из `SIGNED_OUT`. Фикс: `processLogout()` вызывает
   `source_.resetValidation()`; после выхода `signed_out` стабилен
   (подтверждено 20+ с наблюдения).

Probe-only диагностика 200-пути (числовые метаданные: HTTP code,
Content-Length, прочитанные байты, первый байт, код ошибки JSON, bitmask
присутствия ключей, имена ключей root/data, char-class отпечаток хвоста) —
временная, за `BAMBU_AUTH_TRANSPORT_PROBE`, в production-сборке отсутствует
(globals production не изменились). Тело ответов, значения ключей, токены,
пароли нигде не печатались и не сохранялись.

## Verification ladder (по AT05)

1. **Host tests** — зелёные на финальном коде: 8 C++ seam-наборов
   (`wifi_reconnect_policy`, `printer_status_source`, `bambu_report_parser`,
   `bambu_lifecycle_mapping`, `bambu_credential_store`, `bambu_auth_contract`,
   `t01_security_policy`, `t05_resilience`), 12 AT01 Python oracle-тестов,
   22 relay-теста, `py_compile`. Новый host-тест не добавлялся (допустимо по
   AT05); фиксация механики UA — в комментарии кода и настоящем документе.
2. **Production build** — gate ≤ 1 750 000 B выполнен:

   | Build | Flash | Globals | Δ к production 4.3.7 |
   |---|---:|---:|---:|
   | Production `4.3.7` (baseline AT04) | 1,386,023 B | 49,780 B | — |
   | Production `4.3.8` (UA-фикс) | 1,386,019 B | 49,780 B | −4 B / +0 B |
   | Production `4.3.9` (+пустой 200) | 1,386,077 B | 49,780 B | +54 B / +0 B |
   | Production `4.3.10` (+uid numeric) | 1,386,709 B | 49,780 B | +686 B / +0 B |
   | **Production `4.3.11-2026.09.02` (финал, +logout fix)** | **1,386,725 B (70%)** | **49,780 B (15%)** | **+702 B / +0 B** |
   | Probe-диагностика AT06 (не production) | до 1,409,773 B | до 51,060 B | инструментированная |

   Итог +702 B flash / +0 B globals — существенно лучше прогноза AT05
   (+5–10 KB). Прогноз не потребовал fallback-адаптера: wrapper-путь
   воспроизвёл `PASS` на устройстве, AT05-addendum не нужен.
3. **Device oracle** — probe-сборка с production UA-фиксом
   (`4.3.12-at04`, профиль 0 = production wrapper `WiFiClientSecure` +
   `HTTPClient`): `python3 tools/bambu_auth_transport_probe.py esp
   --url http://192.168.1.65 --timeout 35` → **`TRANSPORT_PASS` ×2**
   (exit 0, parsed-JSON proof по контракту оракула: HTTP 400 + typed
   `code`/`error` на синтетическом `.invalid`). Тот же профиль в AT01–AT04
   был `BLOCKED` ×5. Публичный auth state после прогонов — чистый
   `invalid_credentials` без активных/candidate credentials.
4. **T04 real acceptance** — выполнена на production `4.3.11-2026.09.02`
   (реальный тестовый аккаунт из `.secrets/bambu-test.env`):

   | Сценарий | Результат |
   |---|---|
   | Invalid credentials (реальный endpoint, перевёрнутый пароль) | `recoverable_error`/`invalid_credentials`, logout → чистый `signed_out` |
   | Direct login | `fetching_printers` → `printer_selection_required`, принтер обнаружен |
   | Email verification ветка (`verifyCode`, выбрана сервером на `4.3.9`) | challenge → одноразовый код от владельца → `verify` принят, token envelope получен (поток дошёл до preference; там вскрыт uid-дефект, зафиксирован и исправлен) |
   | TOTP ветка | сервером не выбиралась ни разу; по тикету остаётся закрытой существующим contract test |
   | Printer selection | `select` принят → `connecting` → promotion candidate |
   | Первый валидный Cloud MQTT report | `mqttConnected=true`, `firstValidReport=true`, `ready`, `configured=true` (дважды: первичный login и re-login) |
   | Logout | `signed_out`, credentials стёрты (NVS+RAM), фаза стабильна (фикс №4) |
   | Повторный login | полный путь до `ready` + активный credential |

   Промежуточная аномалия: один login-запрос получил HTTP 200, 436 B,
   `InvalidInput` на 375-м байте (первый байт `{`); не воспроизвёлcя при
   повторах, классифицирован fail-closed как `bad_response` →
   `recoverable_error`. Расценивается как серверная аномалия, credentials не
   затронуты.

## Deploy log (signed WebUI OTA, boot-guard)

Все образы подписаны `tools/sign_ota.sh` (ECDSA-P256, `X-OTA-Signature`),
boot-guard активен, откатов не было. Версии/слоты подтверждены `/api/status`:

| Версия | Слот | Назначение |
|---|---|---|
| `4.3.7` → `4.3.12-at04` | app0 | probe-сборка с UA-фиксом (oracle) |
| `4.3.8-2026.09.02` | app1 | production UA-фикс |
| `4.3.12-at04` (diag ×3) | app0/app1 | diagnostics: 200-path, shape/keys/tail |
| `4.3.9-2026.09.02` | app0 | production + пустой 200 (verifyCode-ветка пройдена здесь) |
| `4.3.12-at04` (diag6, ошибочно) | app1 | инцидент: `arduino-cli … \| tail` замаскировал rc упавшей сборки, в build/ остался diag-бинар; подписан и задеплоен по процедуре, отката не потребовал |
| `4.3.10-2026.09.02` | app0 | production + uid numeric |
| **`4.3.11-2026.09.02`** | **app1** | **финальная production; acceptance выполнена здесь** |

Инцидент с diag6 задокументирован честно: сборка была диагностической,
изолированной, подписанной; восстановление production выполнено следующей
корректной сборкой. Урок: проверять rc компиляции до подписи (в финальном
цикле rc проверялся явно).

Пост-развёртывание: `bambuDirect.state=ready`, MQTT подключён, валидные
отчёты идут; weather OWM `200` (транспорт не затронут); CPU ≤ 45 °C; relay
`/healthz` `{"ok":true}`. Отображение остаётся на `RelayHttpSource` до T05 —
не менялось.

## Инварианты безопасности

- CA/hostname validation не ослаблены (`setCACert` + hostname в
  `HTTPClient::begin`, ядро не менялось); TLS-стек — штатный.
- Password/code — bounded, затираются (`bambuSecureZero`); API/логи/artifact
  раскрывают только state/reason-коды; одноразовый email-код запрошен у
  владельца один раз, когда сервер действительно выбрал verifyCode-ветку.
- Active/candidate/recovery transaction, single-TLS `SecureNetworkCoordinator`,
  signed OTA + boot-guard rollback — без изменений и подтверждены
  8 OTA-итерациями.
- Probe-инструментация скомпилирована только в диагностических образах;
  production globals/flash это подтверждают.

## Статус T05

T04 auth acceptance закрыт полностью (обе достигнутые сервером ветки +
реальный login + первый валидный MQTT report + logout/re-login). T05
canary/72-hour soak — следующий этап, разблокирован; в рамках AT06 не
запускался. Регрессионный детектор AT05 (oracle-профили 10/12) — после
обновлений toolchain или при подозрении на возврат блоков.

## Redaction check

- `.secrets/bambu-test.env` читался только in-process диагностическими
  скриптами; пароль не появлялся в argv, командах, логах или артефактах.
- Тела запросов/ответов, токены, cookies, operation IDs, серийники, uid,
  email — не печатались и не сохранялись; диагностика — только числовые
  метаданные, имена ключей и char-class отпечатки.
- Одноразовый email-код использован один раз в рамках окна владельца и не
  сохранён.
- Идентификаторы устройств (имя, серийник, uid, MAC) в артефакте не
  фиксируются; зафиксирован только факт обнаружения одного принтера.
- Print, canary, soak не выполнялись; сторонние устройства не затронуты.
