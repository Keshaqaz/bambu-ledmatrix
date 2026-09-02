# AT04 — ClientHello-compatible auth transport

Date: 2026-09-02

Status: complete. Verdict **`PASS`**: минимальный auth-only транспорт на штатном
Mbed TLS достигает Bambu application response, `PASS` воспроизведён 13 раз
(по каждому кандидату ≥3). При этом доказано, что дискриминатором Cloudflare
был **не TLS ClientHello**, а одна HTTP-строка: заголовок
`User-Agent: ESP32HTTPClient`, который молча подставляет HTTPClient
(production `addHeader("User-Agent", …)` игнорируется обёрткой). Материал
меняет основу решения AT05.

## Oracle и input

Использован только oracle AT01
(`esp32_matrix_controller22/tools/bambu_auth_transport_probe.py`), синтетическая
невалидная учётная запись `*.invalid`. Реальный secrets-файл не читался.
Команда из каталога `esp32_matrix_controller22`:

```sh
python3 tools/bambu_auth_transport_probe.py esp \
  --url "$DEVICE_URL" --timeout 35 --profile N
```

Oracle расширен профилями 6–12 (механика AT02/AT03 без изменений; ID передаётся
в probe-only login и обязан совпасть в terminal status). Покрытие добавлено в
`tools/test_bambu_auth_transport_probe.py` (host-набор зелёный).

## Шаг 1. ESP ClientHello зафиксирован (закрыт разрыв AT01)

AT01 не содержал ESP-стороны capture (наблюдательная точка отсутствовала).
Здесь observation build (`BAMBU_AUTH_TRANSPORT_CAPTURE`) перенаправляет
transport-коннект на локальный TLS-endpoint на Mac (openssl `s_server -msg`),
сохраняя production SNI `api.bambulab.com` и байт-идентичный ClientHello.
Верификация не отключалась: capture-сборка якорится на локальный одноразовый
самоподписанный CA, а HTTP-запросы снимались python-TLS-сервером с этим же CA.
Capture-сборка с Bambu Cloud не соединяется никогда; устройства к ней — только
синтетические credentials.

Измеренный профиль ESP (stock Mbed TLS, core 3.3.8; совпадает для baseline
wrapper'а и адаптера **байт-в-байт**, кроме 32 байт random):

- record legacy version `0303`, ClientHello version `0303` (TLS 1.2 only —
  расширений `supported_versions`/`key_share` нет, ALPN нет);
- 81 cipher suite в порядке Mbed TLS
  (`c02c,c030,c0ad,c024,c028,c00a,c014,c0af,c087,c08b,c073,c077,c05d,c061,c049,c04d,c02b,c02f,c0ac,c023,c027,c009,c013,c0ae,c086,c08a,c072,c076,c05c,c060,c048,c04c,009d,c09d,003d,0035,c032,c02a,c00f,c02e,c026,c005,c0a1,c07b,00c0,0084,c08d,c079,c089,c075,c05f,c063,c051,c04b,c04f,c03d,009c,c09c,003c,002f,c031,c029,c00e,c02d,c025,c004,c0a0,c07a,00ba,0041,c08c,c078,c088,c074,c05e,c062,c050,c04a,c04e,c03c,00ff`),
  завершается SCSV `00ff` (empty renegotiation info — расширением `ff01` не
  передаётся);
- расширения, в порядке: `0000` (SNI api.bambulab.com), `000a`, `000d`, `000b`,
  `0016` (encrypt-then-mac), `0017` (extended master secret), `0023` (client
  session ticket, пустой);
- supported groups (7): `001d` (X25519), `0017`, `0018`, `0019`,
  `001a`, `001b`, `001c` (brainpoolP256/384/512);
- signature_algorithms (6, legacy-кодирование): `0603,0601,0503,0501,0403,0401`
  (ECDSA/RSA × SHA-512/384/256; RSA-PSS `0804–0806` и post-quantum отсутствуют);
- ec_point_formats: uncompressed; session_id: пустой.

Дифф против desktop-контроля AT01 (TLS 1.3, 30 cipher TLS1.3-first, 11
расширений включая `0043/0045/0051`, X25519MLKEM768, 26 signature algorithms с
`0904–0906`): обширен, но **не является причиной блокировки** — доказано
ниже. Desktop-реплики на хосте (openssl `s_client -tls1_2`, host Mbed TLS
3.6.4 с тем же 81-suite списком) проходили Cloudflare `PASS` при полном
сохранении заголовков.

## Реализация

- **Прямой stock-Mbed TLS адаптер** (probe-only,
  `mbedtlsCipherProfileLoginRequest`): тот же предустановленный Mbed TLS
  (upstream: Arduino ESP32 core 3.3.8, Mbed TLS 3.6.x, бинарные библиотеки
  без патчей), TCP через обычный `NetworkClient` + `mbedtls_ssl_set_bio`,
  `VERIFY_REQUIRED` + CA bundle + `mbedtls_ssl_set_hostname("api.bambulab.com")`
  (SNI + hostname check), TLS 1.2 min/max, контролируемый
  `mbedtls_ssl_conf_ciphersuites` (профили 6/7/8), bounded request/response
  буферы с `secureZero`, таймауты 12 с. Тот же endpoint, HTTP/1.0, порядок
  production-заголовков, payload-координаты AT01.
- **Capture-машина** (см. выше) — только diagnostic-сборки.

## Матрица профилей и вердикты (устройство, oracle)

| ID | Profile | Verdict | Bounded evidence |
|---:|---|---|---|
| 0 | production wrapper `WiFiClientSecure`+`HTTPClient` | `BLOCKED` ×3 сегодня (исторически ×2 в AT01/AT02) | `cloudflare_blocked`, application JSON false |
| 2 | AT02 `esp_transport` TLS1.2 + desktop-order cipher list | `ERROR` ×2 | соединение закрывается до записи ClientHello (локальный raw-логгер: TCP accepted, 0 байт, EOF через 0.02 с); `esp_tls_connect_failed_errno_0`, error handle пуст; plumbing-дефект транспортной обвязки, не fingerprint |
| 6 | адаптер, strong-only cipher list (12 AEAD) | `PASS` ×2, `ERROR` ×2 | `invalid_credentials`, application JSON true; ERROR — транзиентные локальные handshake-сбои, не классифицированы |
| 7 | адаптер, single cipher `c030` | `PASS` ×1, `ERROR` ×1 | аналогично |
| 8 | адаптер, default cipher list (контроль адаптера) | `PASS` ×4 | `invalid_credentials`, application JSON true |
| 9 | адаптер, default list, UA `ESP32 HTTP Client/1.1` | `PASS` ×3 | — |
| 10 | адаптер, точная эмуляция порядка заголовков wrapper'а (`Connection: close` после UA), UA `ESP32 HTTP Client/1.1`, body отдельным TLS-record | `PASS` ×2 | — |
| 11 | адаптер, bambu UA, body отдельным TLS-record | `PASS` ×1 | — |
| 12 | адаптер, точная эмуляция wrapper'а + **UA `ESP32HTTPClient`** | `BLOCKED` ×1 (+1 транзиентный `ERROR`) | `cloudflare_blocked` — блок воспроизведён на чистом пути, отличающемся от PASS-варианта только строкой UA |

## Точный wire diff, связанный с вердиктом

Захваченные HTTP-запросы (python-TLS сервер, обе траектории в одном логе):

- production wrapper (profile 0, `BLOCKED`): `User-Agent: ESP32HTTPClient`;
- адаптер (profiles 8/10, `PASS`): `User-Agent: bambu_network_agent/02.07.01.51`
  / `User-Agent: ESP32 HTTP Client/1.1`.

`HTTPClient::sendHeader` всегда пишет собственный `User-Agent` из `_userAgent`
(дефолт ядра — строка `ESP32HTTPClient`), а `HTTPClient::addHeader("User-Agent",
…)` **молча отбрасывается** (allow-list исключает User-Agent/Connection/Host/
Accept-Encoding). Production-код Bambu auth-транспорта с AT01 предполагался
отправляющим `bambu_network_agent/02.07.01.51` — фактический wire-запрос
никогда не содержал этот заголовок. Cloudflare отклоняет `User-Agent:
ESP32HTTPClient` (403 HTML challenge) и пропускает тот же самый TLS ClientHello
с любым другим UA (400 `invalid_credentials` — ожидаемый отказ Bambu API на
синтетику = `TRANSPORT_PASS`).

Вывод: **Cloudflare-блокер T04 — не TLS-fingerprint ESP32/Mbed TLS, а
подменённый HTTP User-Agent.** wolfSSL-верdict AT03 (`BLOCKED` при чистом UA
bambu) остаётся единственным TLS-зависимым блоком из наблюдённых (wolfSSL
ClientHello блокируется сам по себе).

## PASS, повторения и откат

- `PASS` (Bambu application JSON через oracle) суммарно ×13: profile 8 ×4,
  9 ×3, 10 ×2, 6 ×2, 7 ×1, 11 ×1; критерий «минимум три раза» закрыт с
  запасом на нескольких профилях.
- Все диагностические образы — signed WebUI OTA с boot-guard; production
  `4.3.7-2026.09.02` восстановлена дважды (`app0`, затем `app1`), после
  восстановления auth state `signed_out`, active/candidate credentials
  отсутствуют, post-run logout принят. Метрики production побитно равны
  AT02/AT03 (1,386,023 B / 49,780 B globals).
- CA/hostname validation и credential erasure не ослаблены: адаптер работает
  с `MBEDTLS_SSL_VERIFY_REQUIRED`, `set_hostname`, проверкой
  `mbedtls_ssl_get_verify_result`, CA bundle без изменений; capture-сборки
  якорятся на локальный CA, к Cloud не обращаются; секреты затираются.

## Image и ресурсы

| Build | Flash | Globals | Delta к production 4.3.7 |
|---|---:|---:|---:|
| Production `4.3.7` | 1,386,023 B | 49,780 B | — |
| Probe `4.3.12-at04` (все профили 0–12) | 1,406,617 B | 50,492 B | +20,594 B flash, +712 B globals |
| Capture `4.3.11-cap04` v2 | 1,407,457 B | 50,484 B | +21,434 B flash, +704 B globals |

Слот 1,966,080 B — 71–72%. Прогноз интеграции: только адаптер (без
esp_transport-пути, capture и лишних профилей) ≈ +5–10 КБ flash, +0 B globals;
точная цифра — при AT06. Ресурсы во время прогонов: minimum internal heap
43,480 B, minimum largest block 61,428 B, CPU temp ≤45.3 °C — запас достаточен,
существующие C3-геймы (heap/stack/термика) не под угрозой.

## Проверки

- production build PASS (метрики идентичны AT02/AT03);
- C++ auth seam tests PASS (9 файлов); host oracle tests PASS (12 кейсов);
- детерминированный WebUI export не затрагивался (index_html без изменений);
- all OTA iterations signed and accepted; boot-guard/rollback не затронуты.

## Основание пересмотра подхода (для AT05)

1. **Минимальный production-фикс** — одна строка в auth-transport:
   `http.setUserAgent("bambu_network_agent/02.07.01.51")` вместо отбрасываемого
   `addHeader` (или пере-порядок добавления). Замена TLS-транспорта не требуется
   для прохождения Cloudflare. Риск: неизвестен период существования UA-правила
   Cloudflare; в отличие от TLS-подмены, UA-fix сохраняет штатный стек.
2. Прямой stock-Mbed TLS адаптер (этот тикет) — запасной кандидат, если AT05
   потребует независимости от HTTPClient (например, единый TLS-пула или
   контроля ClientHello).
3. Регрессионный детектор: пара профилей oracle 10 (PASS) / 12 (BLOCKED)
   отслеживает, не начнёт ли Cloudflare блокировать и bambu UA.
4. S3 и иные «fingerprint-фиксы» не требуются и не предлагались.

## Redaction check

- Только синтетические invalid credentials; реальные secrets не читались.
- Request/response body, tokens, cookies, operation IDs не печатались и не
  сохранялись; capture-логи удалены после извлечения метаданных (остались
  только агрегаты в этом файле).
- Локальные идентификаторы (SSID, MAC/device ID) отсутствуют в артефакте.
- Capture-сертификат и ключ — одноразовые локальные, удалены после сессии;
  wolfSSL/Mbed TLS host-исходники и сборки удалены; в репозитории остался
  только probe-код.
