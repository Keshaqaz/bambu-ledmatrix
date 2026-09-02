# AT02 — штатные профили Mbed TLS / ESP-TLS

Date: 2026-09-02

Status: complete. Ни один допустимый профиль не получил Bambu application JSON.
Profiles 0, 1 и эквивалентный им current-default profile 3 воспроизводимо
заблокированы Cloudflare. Profile 2 удалось выразить штатным ESP-TLS API, но его
TLS 1.2 handshake завершился локальным `TRANSPORT_ERROR`. Profile 4 не имеет
допустимой комбинации параметров.

## Oracle и input

Использован только oracle AT01:
`esp32_matrix_controller22/tools/bambu_auth_transport_probe.py`. Новый harness
не создавался. Команда из каталога `esp32_matrix_controller22`:

```sh
python3 tools/bambu_auth_transport_probe.py esp \
  --url "$DEVICE_URL" --timeout 30 --profile N
```

Oracle принимает только `N=0..4`, передаёт ID в probe-only login и принимает
terminal status только с тем же ID. stdout содержит ровно один из
`TRANSPORT_PASS`, `TRANSPORT_BLOCKED`, `TRANSPORT_ERROR`. Для синхронного прямого
ESP-TLS profile 2 host-side login timeout увеличен с 5 до bounded 20 секунд;
payload, classifier и terminal deadline не менялись.

Во всех запусках использованы только фиксированные synthetic `.invalid`
credentials AT01. Реальный secrets-файл не читался.

## Реализация и фиксированная матрица

Изменения изолированы `BAMBU_AUTH_TRANSPORT_PROBE`; production transport остаётся
`WiFiClientSecure`. Во всех применимых профилях CA и hostname validation включены.

| ID | Tested profile | Verdict | Bounded evidence |
|---:|---|---|---|
| 0 | AT01 production Mbed TLS baseline | `TRANSPORT_BLOCKED` | Повторён после OTA и отдельно после reboot: `cloudflare_blocked`, application JSON false |
| 1 | Baseline + явный ALPN `http/1.1`; hostname задаёт SNI | `TRANSPORT_BLOCKED` | `cloudflare_blocked`, application JSON false |
| 2 | Stock `esp_transport_ssl`: TLS 1.2, enabled client tickets, поддерживаемое пересечение TLS 1.2 cipher order из desktop AT01 | `TRANSPORT_ERROR` | `esp_transport_connect()` вернул `-1`; public error `esp_tls_connect_failed`, application JSON false; ESP-TLS error handle не предоставил более узкого кода |
| 3 | Актуальный supported Arduino ESP32 core 3.3.8 / Mbed TLS default | `TRANSPORT_BLOCKED` | Это тот же current default, уже измеренный profile 0; необратимая migration не требовалась |
| 4 | Bounded combined profile | `NOT_APPLICABLE` | ALPN profile 1 не изменил oracle и отсутствует в desktop control; profile 2 не дошёл до wire response; profile 3 дублирует baseline — eligible независимой комбинации нет |

Profile 2 использует публичные `esp_transport_ssl_set_tls_version()` и
`esp_transport_ssl_set_ciphersuites_list()`. Список строится как пересечение
точного TLS 1.2 порядка desktop AT01 с `esp_tls_get_ciphersuites_list()`, сохраняя
desktop order. Core собран с `CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS=y`, то
есть client tickets доступны штатно; session resumption не требовалась и не
выдаётся за часть transport verdict. CA PEM передан transport, а hostname
`api.bambulab.com` — в connect, поэтому SNI и hostname verification не отключены.

Прямой HTTP/1.0 request сохраняет endpoint, header order и synthetic JSON AT01.
Secret-bearing request размещён в bounded heap buffer, затирается перед free;
response bounded 2,048 bytes и также очищается. Ранее диагностические adapter
runs, не изолировавшие TLS transport, не считаются matrix verdicts.

`TRANSPORT_ERROR` profile 2 не переименован в fingerprint failure. Локальная
граница установлена точно: DNS/transport initialization и cipher intersection
прошли, ошибка возникла внутри stock ESP-TLS connect/handshake до HTTP response;
установленный binary ESP-IDF component не вернул дополнительный last-error code.
Дальнейший перебор или rebuild TLS stack вышел бы за фиксированную матрицу AT02.

## Image и ресурсы

| Build | Flash | Globals | Delta к AT01 соответствующему build |
|---|---:|---:|---:|
| Production `4.3.7` | 1,386,023 B | 49,780 B | +172 B flash, +0 B globals |
| Final AT02 probe | 1,401,703 B | 49,796 B | +15,054 B flash, +16 B globals |

Slot limit — 1,966,080 bytes; обе сборки помещаются (70–71%). Production source
selector, MQTT, weather и OTA backend не переключались.

После isolated profile 2 run на `4.3.6` были сняты bounded resource metrics:

- current internal heap: 141,380 B;
- minimum internal heap: 111,252 B;
- current largest internal block: 114,676 B;
- minimum largest internal block: 114,676 B;
- printer task stack reserve: 4,324 B.

Profile 2 не является кандидатом для integration из-за `ERROR`; значения лишь
подтверждают, что failure не вызван исчерпанием heap/stack. Для лучшего
fingerprint-verdict профиля 0 предыдущий isolated run дал minimum heap 52,416 B,
minimum largest block 55,284 B и printer stack reserve 3,236 B. Полный soak не
выполнялся.

## Проверки и deploy

- production build: PASS, 1,386,023 B / 49,780 B;
- final probe build: PASS, 1,401,703 B / 49,796 B;
- C++ auth seam tests: PASS (9 cases);
- AT01/AT02 Python oracle tests: PASS (11 cases);
- четыре последовательно разрешённые signed OTA diagnostic iterations приняты;
  последний device boot подтверждён как `4.3.7-2026.09.02`, `app0`. Финальная
  fail-closed response-cap правка собрана локально и не требовала ещё одного
  deploy, поскольку измеренный failure произошёл раньше, на TLS connect;
- post-run logout принят; active/candidate credentials не создавались.

## Бинарный вывод

`NO PASS`. Штатные baseline/ALPN/current-default profiles достигают Cloudflare,
но остаются `TRANSPORT_BLOCKED`. Desktop-order TLS 1.2 profile технически
реализуем штатным ESP-TLS, однако завершается `TRANSPORT_ERROR` до HTTP response.
Production default не изменён; integration diff не подготовлен.

По short-circuit policy следующий шаг — AT03 auth-only wolfSSL. Переход прямо к
AT04 пока не разрешён общим контрактом.

## Redaction check

- Использованы только synthetic invalid credentials; реальные credentials и
  ignored secrets не читались.
- Request/response body, account, password, cookies, tokens и operation IDs не
  печатались и не сохранялись.
- В artifact нет SSID, MAC/device ID или иных локальных идентификаторов.
- Packet capture, real login, verification, MQTT, print, canary и soak не
  выполнялись.
