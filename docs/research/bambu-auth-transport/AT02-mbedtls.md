# AT02 — штатные профили Mbed TLS / ESP-TLS

Date: 2026-09-02

Status: complete with one documented toolchain infeasibility. Ни один допустимый
штатный профиль не получил Bambu application JSON. Baseline и единственный
реализуемый independent variant воспроизвели Cloudflare block; explicit
TLS/cipher/ticket profile невозможно выразить публичным API установленного core
без его миграции/rebuild. Неуспешный adapter experiment не считается verdict
TLS profile и удалён из итогового кода.

## Oracle и фиксированный input

Использован только существующий AT01 oracle:
`esp32_matrix_controller22/tools/bambu_auth_transport_probe.py`. Новый harness
не создавался. Команда из каталога `esp32_matrix_controller22`:

```sh
python3 tools/bambu_auth_transport_probe.py esp \
  --url http://192.168.1.65 --timeout 25 --profile N
```

`N` принимает только значения `0..4`. Oracle передаёт profile ID в
probe-only login request и принимает terminal status только с тем же ID; это
не позволяет stale operation выдать verdict другого профиля. stdout каждого
запуска по-прежнему содержит ровно одну строку `TRANSPORT_PASS`,
`TRANSPORT_BLOCKED` или `TRANSPORT_ERROR`.

Во всех запусках использовались фиксированные synthetic `.invalid` account и
invalid password из AT01. Endpoint, HTTP/1.0, payload coordinates, header set и
order, timeouts и classifier не менялись. Файл
`esp32_matrix_controller22/.secrets/bambu-test.env` не читался.

## Реализация profiles

Все изменения изолированы `BAMBU_AUTH_TRANSPORT_PROBE`. Production default
остаётся исходным `WiFiClientSecure` с `setCACert(kBambuCloudCa)`, hostname из
URL и обязательной hostname validation.

| ID | Tested profile | CA / hostname validation |
|---|---|---|
| 0 | AT01 production baseline: `WiFiClientSecure`, штатный Mbed TLS default | CA через `setCACert`; `api.bambulab.com` передан в `HTTPClient::begin` |
| 1 | Profile 0 плюс ALPN `http/1.1`; SNI и hostname остаются явным hostname подключения | То же, validation не отключалась |
| 2 | Infeasible в установленном core: требуется одновременно задать TLS 1.2, desktop cipher order и session tickets | Не запускается; validation не ослабляется |
| 3 | Актуальный поддерживаемый default — тот же Arduino core 3.3.8 / `WiFiClientSecure` default, уже проверенный profile 0; migration не требуется | Та же CA/hostname validation, что в profile 0 |
| 4 | Not eligible: ни один независимый новый параметр не изменил oracle и не приблизил wire profile к desktop control | Не запускается; недопустимая комбинация не создаётся |

Arduino ESP32 core 3.3.8 собран с
`CONFIG_MBEDTLS_SSL_PROTO_TLS1_2=1`,
`CONFIG_MBEDTLS_SSL_ALPN=1` и
`CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS=1`. TLS 1.3 в C3 core profile не
включён. Однако публичный `WiFiClientSecure` предоставляет ALPN, CA и timeout,
но не TLS-version/cipher/ticket setters. `esp_transport_ssl` предоставляет
version и cipher list, но его session-ticket operation полностью исключён,
поскольку `CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS` в core не определён. Поэтому
profile 2 нельзя реализовать как заданную атомарную группу без rebuild/migration
core; наличие внутреннего Mbed TLS feature macro не подменяет явную настройку и
не является handshake evidence.

В device image был диагностически проверен узкий `esp_transport`/Arduino
`HTTPClient` adapter. Он вернул одинаковый `http_-1` даже на default settings,
не изолировал TLS parameters и потому был отвергнут review. Итоговый код fail
closed возвращает `transport_profile_unsupported` для IDs 2/4, отображает ID 3
на unchanged baseline transport и не содержит этот adapter.

## Матрица verdict

Device: ESP32-C3, firmware `4.3.4-2026.09.02`, partition `app0`. Выполнен один
signed OTA deploy. До него live baseline AT01 был `4.3.3-2026.09.01` на
`app1`.

| ID | Oracle verdict | Bounded evidence |
|---|---|---|
| 0 | `TRANSPORT_BLOCKED` | Два контрольных повтора AT02 после устранения фонового WebUI polling, плюс отдельный post-reboot repeat: public error `cloudflare_blocked`, application proof false |
| 1 | `TRANSPORT_BLOCKED` | public error `cloudflare_blocked`, application proof false; ALPN не приблизил outcome к desktop control |
| 2 | `INFEASIBLE` | public API split точен: `WiFiClientSecure` не задаёт version/ciphers/tickets, а `esp_transport_ssl_session_ticket_operation` скомпилирован только при отсутствующем `CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS`; необратимая core migration запрещена profile 3 condition и не оправдана AT02 |
| 3 | `TRANSPORT_BLOCKED` | установленный core 3.3.8 уже является актуальным поддерживаемым default и совпадает с profile 0; отдельной настройки или migration нет, поэтому применим воспроизводимый profile 0 oracle |
| 4 | `NOT_APPLICABLE` | profile 1 сохранил `BLOCKED`, ALPN отсутствует у desktop control; profile 2 infeasible, profile 3 дублирует baseline — параметров, удовлетворяющих bounded-combination gate, нет |

Первый profile 0 invocation сразу после OTA вернул `TRANSPORT_ERROR`, потому
что оставленная открытой WebUI вкладка продолжала polling однопоточного
WebServer. Login operation при этом не была принята, status оставался чистым
`signed_out`. После закрытия вкладки profile 0 воспроизводимо дал
`TRANSPORT_BLOCKED`; инфраструктурный запуск не включён в matrix verdict.

Диагностические `http_-1` runs IDs 2–4 не включены как profile verdicts:
`TRANSPORT_ERROR` не переименован в `BLOCKED`, а invalid profile 4 не объявлен
bounded combination. Они лишь отвергли промежуточный adapter. HTTP headers не
перебирались, validation не ослаблялась, workaround не добавлялся.

## Image и RAM

| Build | Flash | Globals | Delta к AT01 соответствующему build |
|---|---:|---:|---:|
| Production `4.3.4` | 1,386,023 B | 49,780 B | +172 B flash, +0 B globals |
| Final AT02 probe | 1,387,221 B | 49,788 B | +572 B flash, +8 B globals |

Production app slot — 1,966,080 bytes; обе сборки занимают 70%. Production
source selector, Cloud transport default, MQTT, weather и OTA backend не
переключались.

Device resource run выполнялся на первоначальном probe image размером
1,388,689 B до удаления отвергнутого adapter. Его static RAM совпадает с
финальным probe image. После отдельного reboot и единственного isolated profile
0 run:

- current internal heap: 141,356 B;
- minimum internal heap: 52,416 B;
- current largest internal block: 106,484 B;
- minimum largest internal block: 55,284 B;
- printer task stack reserve: 3,236 B.

Это метрики лучшего доступного кандидата — неизменённого baseline. Он остаётся
ресурсно допустимым, но transport verdict остаётся `BLOCKED`. Полный T05
soak/canary не выполнялся.

## Проверки

- production build: PASS;
- probe build: PASS;
- девять C++ seam tests: PASS;
- 11 AT01/AT02 oracle tests: PASS;
- 22 relay Python tests под Python 3.14: PASS;
- Python compilation, shell syntax и deterministic WebUI export: PASS;
- signed OTA response: `{"ok":true}`; post-boot firmware/partition подтверждены;
- post-run auth state: clean `signed_out`, active/candidate credentials отсутствуют.

Системный Python 3.9 не подходит для уже существующего relay `admin.py` с
`X | None`; canonical relay suite поэтому, как и в AT01, запускался под локальным
Python 3.14. В sandbox loopback bind потребовал разрешённый локальный запуск.

## Бинарный вывод

`NO PASS`. Штатные `WiFiClientSecure` profiles 0/1 достигают Cloudflare, но
остаются `TRANSPORT_BLOCKED`; актуальный default profile 3 эквивалентен profile
0. Profile 2 точно infeasible через публичный API установленного core, поэтому
не имеет fingerprint verdict. Profile 4 не имеет eligible parameters и не
формируется. Production integration diff не подготовлен и production default
не изменён.

По short-circuit policy следующий обязательный шаг — AT03 auth-only wolfSSL.
AT02 не разрешает переход прямо к AT04: сначала AT03 должен получить собственный
канонический verdict.

## Redaction check

- Использованы только фиксированные synthetic invalid credentials.
- Реальные credentials и ignored secrets-файл не читались.
- Request/response body, account, password, cookie, token и operation ID не
  печатались и не сохранялись.
- В artifact нет SSID, MAC/device ID или иных локальных идентификаторов.
- Packet capture, raw TLS trace, real login, verification, MQTT, canary и soak
  не выполнялись.
