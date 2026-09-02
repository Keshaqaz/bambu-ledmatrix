# AT03 — auth-only wolfSSL

Date: 2026-09-02

Status: complete; обновлено addendum от 2026-09-02. Первые две device-итерации
завершились `TRANSPORT_ERROR` (локальный сбой подтверждения цепочки); причина
устранена в addendum, кандидат доведён до валидного fingerprint verdict
`TRANSPORT_BLOCKED`. wolfSSL не передан в production, integration diff не
подготовлен.

## Oracle и input

Использован только oracle AT01:
`esp32_matrix_controller22/tools/bambu_auth_transport_probe.py`. Новый harness
не создавался. Команда из каталога `esp32_matrix_controller22`:

```sh
python3 tools/bambu_auth_transport_probe.py esp \
  --url "$DEVICE_URL" --timeout 35 --profile 5
```

Oracle дополнен отдельным `--profile 5` (probe-only ID в login body и
требование совпадения ID в terminal status — механика AT02 без изменений);
покрытие добавлено в `tools/test_bambu_auth_transport_probe.py`
(`test_at03_wolfssl_profile_is_sent`), host-набор — 12 тестов, все зелёные.
stdout содержит ровно один из `TRANSPORT_PASS`, `TRANSPORT_BLOCKED`,
`TRANSPORT_ERROR`.

Во всех запусках использованы только фиксированные synthetic `.invalid`
credentials AT01. Реальный secrets-файл не читался.

## Toolchain совместимость и способ поставки

- Официальный компонент `espressif/esp-wolfssl` в актуальном master
  интегрируется как custom esp-tls stack через `CONFIG_ESP_TLS_CUSTOM_STACK`,
  который доступен только с **ESP-IDF v6.x+**. Проект собирается через
  arduino-cli на Arduino ESP32 core `3.3.8` (ESP-IDF 5.5.x), поэтому путь
  IDF-компонента для production toolchain недоступен.
- Использован официальный Arduino-library дистрибутив wolfSSL `5.8.4`,
  распакованный во временную директорию вне репозитория и подключённый
  только к diagnostic-сборке. В репозиторий зависимость не добавлялась.
- Лицензия wolfSSL — **GPLv3** (исключение для GPLv2 действует только для
  перечисленного ПО MariaDB); коммерческая лицензия доступна у wolfSSL Inc.
  Кандидат `NO PASS`, поэтому вопрос совместимости лицензии с проектом
  дальше не прорабатывался — dependency не фиксируется.
- `user_settings.h` профиль: TLS 1.3, TLS extensions, supported curves,
  `HAVE_SNI`; DTLS/DTLS 1.3 выключены как вне области auth-only HTTPS probe.

## Реализация

Изменения изолированы `BAMBU_AUTH_TRANSPORT_PROBE` +
`BAMBU_AUTH_WOLFSSL_PROBE`; production transport остаётся `WiFiClientSecure`
и production source selector не переключался. wolfSSL-адаптер — отдельный
`Cloud`-клиент за существующим `cloudJsonRequest` seam: тот же endpoint,
HTTP/1.0, header order, payload координаты AT01, bounded request/response
буферы с затиранием, 12-секундные таймауты.

Certificate и hostname validation включены и не отключались: CA bundle
Bambu Cloud загружен как PEM (`wolfSSL_CTX_load_verify_buffer`),
`wolfSSL_CTX_set_verify(WOLFSSL_VERIFY_PEER, nullptr)`,
`wolfSSL_check_domain_name(ssl_, "api.bambulab.com")`, SNI через
`wolfSSL_UseSNI(WOLFSSL_SNI_HOST_NAME)` (кластер собран с `HAVE_SNI`).

MQTT, weather, OTA, credential storage и production build (`4.3.7`,
метрики побитно равны AT02) не затронуты: guards компиляционные, обычная
production-сборка не содержит wolfSSL.

## Вердикты

| Итерация | Build | Verdict | Bounded evidence |
|---|---|---|---|
| 1 | `4.3.8-at03-2026.09.02` | `TRANSPORT_ERROR` | `http_-1`, application JSON false: HTTP ответ не получен; не OOM |
| 2 (финал) | `4.3.9-at03-2026.09.02` | `TRANSPORT_ERROR` | exit 3 за ~3.6 с; bounded firmware-диагностика `wolfssl_io_1_-188`, application JSON false |

Расшифровка финальной ошибки: stage `1` — `wolfSSL_connect()`; код `-188` —
`ASN_NO_SIGNER_E` («ASN no signer to confirm failure») из
`wolfssl/wolfcrypt/error-crypt.h` wolfSSL 5.8.4, то есть подтверждение
подписанта цепочки сертификатов сервера не нашло доверенного подписанта в
загруженном CA bundle. Handshake прерван fail-closed при включённых
certificate/hostname validation — до отправки HTTP запроса и какой-либо
classifier-оценки wire-ответа. Диагностика выведена только в bounded
публичный error-код; тела, credentials, cookies, tokens и operation IDs не
печатались и не сохранялись.

Ошибка не интерпретируется как Cloudflare fingerprint verdict: wolfSSL
ClientHello не дошёл до HTTP-слоя, поэтому сравнение с AT01/AT02
`TRANSPORT_BLOCKED` невозможно. Ограничение измерения: точная причина
несовпадения цепочки (формат CA bundle, отсутствующая промежуточная
сертификация, особенности wolfSSL-парсера) за пределами фиксированной матрицы
AT03; дальнейший перебор не выполнялся согласно контракту «ERROR — не
интегрировать stack дальше».

## Image и ресурсы

| Build | Flash | Globals | Delta к production `4.3.7` |
|---|---:|---:|---:|
| Production `4.3.7` | 1,386,023 B | 49,780 B | — |
| Probe `4.3.8-at03` | 1,617,265 B | 50,700 B | +231,242 B flash, +920 B globals |
| Probe `4.3.9-at03` (финал) | 1,617,319 B | 50,700 B | +231,296 B flash, +920 B globals |

Slot limit — 1,966,080 bytes; probe-сборки занимали 82% слота, production —
70%. Production image delta к AT02 — 0 B.

Ресурсы в момент handshake (после device-прогона, bounded):
- итерация 1: minimum heap 88,820 B; minimum largest block 114,676 B;
  printer task stack reserve 3,084 B;
- итерация 2: минимумы с загрузки `4.3.9` (окно включает прогон):
  heap 96,540 B, minimum largest block 114,676 B, printer reserve 3,084 B.

Failure не связан с исчерпанием heap/stack. Полный soak не выполнялся.

## Проверки и deploy

- production build: PASS, 1,386,023 B / 49,780 B (побитно по метрикам = AT02);
- final probe build: PASS, 1,617,319 B / 50,700 B;
- host oracle tests: PASS (12 cases, включая новый profile-5 тест);
- C++ auth seam tests: PASS;
- детерминированный WebUI export check: PASS;
- две signed OTA diagnostic-итерации (`4.3.8` → `app1`, `4.3.9` → `app0`)
  приняты, rollback/boot-guard не затронуты;
- восстановление production: signed OTA чистой `4.3.7-2026.09.02` принята
  дважды (сначала `app1`, затем `app0`), оба слота теперь содержат
  production-образ без wolfSSL; после восстановления устройство здорово,
  auth state `signed_out`, active/candidate credentials отсутствуют,
  post-run logout принят;
- загруженные build/исходные артефакты wolfSSL (`5.8.4` Arduino copy и
  клон `espressif/esp-wolfssl`) удалены; в репозитории остался только
  probe-код и этот отчёт.

## Бинарный вывод

`NO PASS`. Auth-only wolfSSL 5.8.4 на ESP32-C3 не получает HTTP-ответа от
Bambu Cloud endpoint: handshake завершается локальным
`ASN_NO_SIGNER_E` при включённых CA/hostname/SNI validation, поэтому oracle
классифицирует `TRANSPORT_ERROR`. Flash-цена кандидата — ~+231 KB
(82% слота), static RAM +920 B. Production default не изменён; минимальный
adapter plan не готовится (требование только для `PASS`). wolfSSL не остался
ни в production build, ни как зафиксированная зависимость.

По общему контракту transport spikes исчерпаны в пользу отказа: дальнейший
шаг — решение в `0007-bambu-auth-transport-decision.md`.

## Redaction check

- Использованы только synthetic invalid credentials; реальные credentials и
  ignored secrets не читались.
- Request/response body, account, password, cookies, tokens и operation IDs
  не печатались и не сохранялись.
- В артефакте нет SSID, MAC/device ID или иных локальных идентификаторов.
- Packet capture, real login, verification, MQTT, print, canary и soak не
  выполнялись.

## Addendum 2026-09-02 — ERROR устранён, получен fingerprint verdict

Статус ERROR-ветки закрыт по контракту «сессия обязана либо устранить локальную
ошибку, либо документировать несовместимость». Локальная ошибка устранена;
кандидат получил первый валидный wolfSSL fingerprint verdict: `TRANSPORT_BLOCKED`.

### Точная причина ASN_NO_SIGNER_E

CA bundle прошивки (`kBambuCloudCa`) — это **cross-signed** сертификат
`CN=DigiCert Global Root G2` (issuer: `CN=DigiCert Global Root CA`,
2024-01-18…2031-11-09), идентичный третьему сертификату, который сервер
отдаёт в цепочке. `openssl x509`/`s_client` на живой цепочке
`api.bambulab.com:443` зафиксировали состав: leaf `*.bambulab.com` ←
`Encryption Everywhere DV TLS CA - G2` ← cross-signed G2. Mbed TLS
останавливает построение цепочки на доверенном (даже не self-signed) якоре —
поэтому baseline проходит. wolfSSL продолжает разрешать цепочку до
self-signed root: issuer якоря (`DigiCert Global Root CA`) в store
отсутствует → подписант не найден → `ASN_NO_SIGNER_E` на
`wolfSSL_connect()`. Ошибка воспроизведена и на уровне `openssl verify`
(`unable to get issuer certificate`) с тем же якорем.

### Исправление (только probe)

Добавлен probe-only `kWolfSslProbeCa` из двух публичных self-signed корней:

- `DigiCert Global Root G2` (2013-08-01…2038-01-15, SHA-256
  `CB:3C:CB:B7:60:31:E5:E0:13:8F:8D:D3:9A:23:F9:DE:47:FF:C3:5E:43:C1:14:4C:EA:27:D4:6A:5A:B1:CB:5F`);
- `DigiCert Global Root CA` (2006-11-10…2031-11-10, SHA-256
  `43:48:A0:E9:44:4C:78:CB:26:5E:05:8D:5E:89:44:B4:D8:4F:96:62:BD:26:DB:25:7F:89:34:A4:43:C7:01:61`).

Корни получены из поддерживаемых источников (Mozilla curl CA extract; DigiCert
cacerts endpoint) и проверены криптографически: pubkey self-signed G2 == pubkey
cross-сертификата (`bedf5f65…`), сигнатура intermediate верифицируется
self-signed G2, сигнатура cross-сертификата — self-signed Root CA; полная
цепочка сервера `openssl verify` → `OK`. Production `kBambuCloudCa` и все
production-пути не изменены (wolfSSL-адаптер переключён на новый bundle,
`#ifdef BAMBU_AUTH_WOLFSSL_PROBE`). wolfSSL 5.8.4 Arduino-дистрибутив
восстановлен во временную директорию и удалён после прогона; профиль
`user_settings.h` v2: TLS 1.3 (+`WC_RSA_PSS`, `HAVE_HKDF` — обязательные
для TLS 1.3 с RSA), TLS extensions, supported curves, Curve25519, client
session tickets, SNI, `NO_FILESYSTEM`, DTLS выключен, debug выключен.

### Вердикты

| Итерация | Build | Verdict | Bounded evidence |
|---|---|---|---|
| 3 (фикс CA) | `4.3.10-at03-2026.09.02` | `TRANSPORT_BLOCKED` | handshake с включённой CA/hostname/SNI validation прошёл, HTTP-слой достигнут; публичный статус `recoverable_error`, `cloudflare_blocked`, application JSON false |
| 4 (повтор) | `4.3.10-at03-2026.09.02` | `TRANSPORT_BLOCKED` | воспроизводимо, второй прогон oracle |

Oracle: `python3 tools/bambu_auth_transport_probe.py esp --url … --timeout 35
--profile 5`, exit 2, synthetic `.invalid` credentials. Это первый валидный
fingerprint verdict для wolfSSL: ClientHello wolfSSL блокируется Cloudflare
так же, как штатные Mbed TLS profiles AT02. Ошибки больше нет — «провал
wolfSSL» теперь доказан на уровне fingerprint, а не локального сбоя.

### Image и ресурсы

| Build | Flash | Globals | Delta к AT03 final probe |
|---|---:|---:|---:|
| Probe `4.3.10-at03` | 1,600,931 B (81%) | 50,876 B | −16,388 B flash, +176 B globals |

Во время прогонов: minimum heap 78,916 B; minimum largest block 114,676 B;
CPU temp 45.3 °C — failure не связан с heap/stack.

### Проверки и восстановление

- C++ auth seam tests: PASS (9 файлов); host oracle tests: PASS (12 cases);
- production build метрики побитно равны AT02/AT03 (1,386,023 B / 49,780 B);
- signed OTA `4.3.10-at03` принята (`app1`); после прогонов production
  `4.3.7-2026.09.02` принята дважды (`app0`, затем `app1`) — оба слота снова
  production; post-run logout принят, auth state `signed_out`, active/candidate
  credentials отсутствуют;
- wolfSSL-библиотека из sketchbook удалена, исходники и build-артефакты из
  временной директории удалены; в репозитории остался только probe-код и CA
  литерал как reviewable data.

### Красный флаг снят, но кандидат не интегрируется

`BLOCKED` означает: даже исправленный wolfSSL не проходит Cloudflare. Возврат к
wolfSSL по-прежнему закрыт (GPLv3 + отсутствие официального компонента для
ESP-IDF 5.5.x — см. основной отчёт). По short-circuit контракта ERROR-gate
исчерпан корректно: AT02 — воспроизводимый `BLOCKED`, AT03 — воспроизводимый
`BLOCKED`; переход к AT04 разрешён и теперь опирается на доказанный провал
обоих стеков на уровне fingerprint.
