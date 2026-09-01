# AT02 — штатные профили Mbed TLS / ESP-TLS

Date: 2026-09-02

Status: complete. Ни один штатный профиль не получил Bambu application JSON.
Два профиля воспроизвели Cloudflare block, а три профиля на узком штатном
`esp_transport` adapter завершились локальной ошибкой до HTTP response. Это не
считается доказательством изменения Cloudflare fingerprint.

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
| 2 | Штатный `esp_transport_ssl`: TLS 1.2, client-ticket support из core config и TLS 1.2 cipher IDs в порядке desktop control | Тот же CA; hostname передан в `esp_transport_connect`, `skip_common_name` не используется |
| 3 | Штатный актуальный ESP-IDF/ESP-TLS default core 3.3.8: version `ANY`, default cipher list, без project migration | Тот же CA и hostname; `skip_common_name` не используется |
| 4 | Bounded combination: profile 2 плюс явный `common_name=api.bambulab.com`; ALPN исключён, потому что profile 1 не изменил verdict, а desktop control не имел ALPN | Тот же CA, hostname подключения и дополнительный explicit common name |

Arduino ESP32 core 3.3.8 собран с
`CONFIG_MBEDTLS_SSL_PROTO_TLS1_2=1`,
`CONFIG_MBEDTLS_SSL_ALPN=1` и
`CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS=1`. TLS 1.3 в C3 core profile не
включён. Probe build fail-fast проверяет наличие TLS 1.2 и client ticket
support. Отдельный saved-session API ESP-TLS в этой сборке не включён; AT02 не
утверждает, что выполнял resumption второго соединения.

## Матрица verdict

Device: ESP32-C3, firmware `4.3.4-2026.09.02`, partition `app0`. Выполнен один
signed OTA deploy. До него live baseline AT01 был `4.3.3-2026.09.01` на
`app1`.

| ID | Oracle verdict | Bounded evidence |
|---|---|---|
| 0 | `TRANSPORT_BLOCKED` | Два контрольных повтора AT02 после устранения фонового WebUI polling, плюс отдельный post-reboot repeat: public error `cloudflare_blocked`, application proof false |
| 1 | `TRANSPORT_BLOCKED` | public error `cloudflare_blocked`, application proof false; ALPN не приблизил outcome к desktop control |
| 2 | `TRANSPORT_ERROR` | public error `http_-1`, application proof false; соединение через probe-only `esp_transport` adapter не дошло до классифицируемого HTTP response |
| 3 | `TRANSPORT_ERROR` | тот же `http_-1` на штатном ESP-TLS default; это локализует ошибку в узком adapter path, а не в заданном cipher order |
| 4 | `TRANSPORT_ERROR` | тот же `http_-1`; bounded combination не дал application response |

Первый profile 0 invocation сразу после OTA вернул `TRANSPORT_ERROR`, потому
что оставленная открытой WebUI вкладка продолжала polling однопоточного
WebServer. Login operation при этом не была принята, status оставался чистым
`signed_out`. После закрытия вкладки profile 0 воспроизводимо дал
`TRANSPORT_BLOCKED`; инфраструктурный запуск не включён в matrix verdict.

Profiles 2–4 не имеют fingerprint verdict: `TRANSPORT_ERROR` не переименован в
`BLOCKED`. Их одинаковая ошибка на default и configured ESP-TLS profiles
документирует несовместимость узкого `esp_transport`/Arduino `HTTPClient`
adapter в этой probe-сборке. HTTP headers не перебирались, validation не
ослаблялась, workaround не добавлялся.

## Image и RAM

| Build | Flash | Globals | Delta к AT01 соответствующему build |
|---|---:|---:|---:|
| Production `4.3.4` | 1,386,023 B | 49,780 B | +172 B flash, +0 B globals |
| AT02 probe, profiles 0–4 | 1,388,689 B | 49,788 B | +2,040 B flash, +8 B globals |

Production app slot — 1,966,080 bytes; обе сборки занимают 70%. Production
source selector, Cloud transport default, MQTT, weather и OTA backend не
переключались.

После отдельного reboot и единственного isolated profile 0 run:

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
остаются `TRANSPORT_BLOCKED`. Profiles 2–4 не квалифицированы по fingerprint,
потому что probe-only ESP-TLS adapter возвращает локальный `http_-1` до HTTP
response. Production integration diff не подготовлен и production default не
изменён.

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
