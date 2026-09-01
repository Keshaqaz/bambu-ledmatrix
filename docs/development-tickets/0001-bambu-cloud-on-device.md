---
status: draft
date: 2026-08-27
amended: 2026-08-28
source: ../adr/0001-optimize-esp32-c3-and-move-bambu-cloud-on-device.md
---

# Тикеты: оптимизация ESP32-C3 и перенос Bambu Cloud на устройство

Укрупнённый план реализации [ADR 0001](../adr/0001-optimize-esp32-c3-and-move-bambu-cloud-on-device.md). Шесть тикетов образуют законченные этапы, которые можно отдавать отдельным агентским сессиям. Детали реализации намеренно не дублируют ADR.

Локальные security-требования плана заменены
[ADR 0002](../adr/0002-use-security-level-zero-for-local-access.md): AP, WebUI и
локальный API работают без password/session/CSRF. Это принятое продуктовое
решение, а не незакрытый gate.

## Общие ограничения

- Принтер остаётся в Cloud Mode и доступен в Bambu Handy.
- Сохраняются часы, погода, автояркость, captive portal, WebUI, OTA с двумя слотами, boot guard и удержание printer overlay.
- Пароль Bambu не сохраняется. Secrets не попадают в Serial, API, diagnostics, crash output или browser storage.
- До завершения миграции relay остаётся рабочим rollback path. LAN MQTT не является целевым вариантом.
- Production image ESP32-C3 не превышает `1 750 000` байт. Финальные RAM gates: minimum free internal heap ≥ 50 KB, largest free internal block ≥ 32 KB, task high-water reserve ≥ 2 KB.
- Если обязательные RAM gates не проходят на C3 без удаления функций, целевой hardware меняется на ESP32-S3; локальная авторизация не является gate.

## Политика проверок

Чтобы не расходовать время и токены на избыточную тестовую инфраструктуру:

- запускать существующие проверки, относящиеся к изменённому поведению;
- добавлять только узкие host-тесты для логики, где регрессия опасна и плохо ловится на устройстве: printer lifecycle, MQTT fragment parser и atomic credential transaction;
- не строить универсальные mock-фреймворки и не дублировать одну проверку на нескольких уровнях;
- после firmware-изменений обязательно собирать production-конфигурацию и фиксировать размер image;
- проверки на живом устройстве объединять в один финальный прогон тикета, кроме случаев, когда раннее измерение памяти определяет возможность продолжать работу;
- 72-часовой soak и численные C3 resource gates не сокращать: это условия самого ADR, а не дополнительное тестирование.

## Карта зависимостей

| ID | Тикет | Заблокирован |
|---|---|---|
| T01 | Подготовить ESP32-C3 к хранению Cloud credentials и MQTT | — |
| T02 | Передать состояние принтера единому `PrinterStatusSource` | — |
| T03 | Подтвердить локальный auth flow для Google-created account | — |
| T04 | Реализовать direct Bambu Cloud end to end | T01, T02, T03 |
| AT01–AT06 | Квалифицировать и интегрировать автономный Bambu auth transport | T04 implementation; [отдельный поток](./0002-bambu-auth-transport-spikes.md) |
| T05 | Доказать устойчивость C3 и провести shadow/canary | T04 и AT06 |
| T06 | Сделать direct Cloud автономным production-default | T05 |

## Фактический статус на 2026-09-01

| Этап | Статус | Доказательство/блокер |
|---|---|---|
| T01 | Реализован | `c040b56`; production baseline, signed OTA и diagnostics находятся в firmware |
| T02 | Реализован | `e46595d`, `0f89d38`, `081d880`; единый snapshot seam используется production code |
| T03 | Завершён | browser callback исключён; принят on-device email/password flow |
| T04 | Локальная реализация готова, real acceptance заблокирован | `dfe1708`, `47b1590`, `4091552`; ESP login получает `cloudflare_blocked` до Bambu JSON |
| T05 | Local hardening и первый C3 device gate готовы; canary/soak не начаты | `554a1b7`, `85a6b2a`; auth transport должен пройти AT01–AT06 |
| T06 | Не начат | Требует полного T05 canary и 72-hour soak |

Подготовленный image `4.3.3-2026.09.01` имеет linked size `1 385 857` байт
(`1 386 000` байт экспортированный image). После boot guard измерены minimum
internal heap `114 388`, minimum largest block `114 676`, reserve loop `3 024`,
weather `17 236`, printer `7 488` байт. Доступные C3 gates проходят; MQTT task
reserve, полный direct canary и отсутствие утечки за 72 часа ещё не измерены,
поскольку Cloud session не создана. Актуальный последовательный device flow
находится в `esp32_matrix_controller22/T05_DEVICE_CANARY_RUNBOOK.md`.

---

## T01. Подготовить ESP32-C3 к хранению Cloud credentials и MQTT

### Что построить

Подготовить текущую relay-прошивку к добавлению Cloud-клиента одним этапом:

- добавить безопасные метрики current/minimum internal heap, largest block и task high-water marks и снять baseline на основных сценариях;
- отдавать WebUI как детерминированный gzip asset, сериализовать крупные API-ответы напрямую в HTTP stream, заменить Wi-Fi connect task неблокирующей state machine и ограничить dynamic JSON allocations;
- удалить legacy runtime buffers старых Bambu credentials через явное чтение старого NVS layout;
- сохранить открытый onboarding AP и WebUI/API без password/session/CSRF по ADR 0002;
- отключить ArduinoOTA, добавить проверку подписи web OTA до commit и единый redaction secrets.

Изменения должны сохранить текущее relay-поведение. Размеры стеков уменьшаются только по high-water measurements. FastLED, partition table, eFuse и производственный provisioning не меняются.

### Критерии приёмки

- [ ] Зафиксирован воспроизводимый baseline: boot, AP+STA, WebUI load/save, weather HTTP/HTTPS fallback, OTA и активная печать через relay.
- [ ] Gzip WebUI побайтно совпадает с исходником после распаковки; API не создаёт полные промежуточные JSON-копии; Wi-Fi backoff и AP fallback не изменились.
- [ ] Malformed/oversized JSON даёт контролируемую ошибку; старые NVS settings мигрируют без потери пользовательских настроек.
- [ ] Onboarding AP открыт, WebUI/API не требуют admin password, session cookie или CSRF.
- [ ] Неподписанный web OTA и ArduinoOTA недоступны; валидный signed OTA и boot rollback работают.
- [ ] Production build проходит, image укладывается в лимит ADR; записаны flash/RAM deltas и финальные метрики устройства.
- [ ] Existing WebUI export, Wi-Fi policy и релевантные smoke checks проходят без функциональных регрессий.

### Заблокирован

Не заблокирован — можно начинать сразу.

---

## T02. Передать состояние принтера единому `PrinterStatusSource`

### Что построить

Ввести единый источник нормализованного `PrinterStateSnapshot`, который читают matrix renderer и WebUI. Transport details, JSON и reconnect rules остаются внутри адаптеров. Текущий путь оформить как `RelayHttpSource`; добавить лёгкий replay/scripted adapter для узких host-тестов.

Перенести к единственному владельцу правила lifecycle, stale/offline, partial updates, `finished → idle` и удержания overlay. На этом этапе production продолжает работать только через relay, а видимое поведение не меняется.

### Критерии приёмки

- [ ] Renderer и status WebUI читают один snapshot с configured/transport/printer quality, lifecycle/visible lifecycle, progress, layers, remaining time, job, stage и detail.
- [ ] `RelayHttpSource` сохраняет текущие значения и reconnect behavior; transport-specific детали не выходят через интерфейс.
- [ ] `mc_percent: 1` остаётся 1%; partial update не очищает поля; unknown/offline удерживает overlay; explicit idle снимает; finished сохраняет прежний grace contract.
- [ ] Один компактный replay-test suite покрывает перечисленные переходы и malformed/stale input без сети.
- [ ] Production build и реальный relay smoke test проходят без визуальной или WebUI-регрессии.

### Заблокирован

Не заблокирован — может выполняться параллельно с T01 и T03.

---

## T03. Подтвердить локальный auth flow для Google-created account

### Что построить

Провести ограниченный spike для аккаунта Bambu, первоначально созданного через Google. Spike подтвердил, что официальный Google/browser ticket нельзя вернуть прямо в WebUI устройства: Studio/Suite принимает только browser-host loopback, а generic redirect на локальный адрес устройства отклоняется. Ручной токен, browser cookie, companion script и постоянно работающий внешний callback по-прежнему не считаются успехом.

Принятый локальный flow не использует Google OAuth: владелец такого аккаунта один раз задаёт Bambu-пароль через официальный `Forgot password`, после чего устройство выполняет обычный email/password login и обрабатывает verification/TOTP через WebUI. Пользователь подтвердил 2026-08-28, что для существующего Google-created account пароль установился и последующий email/password login прошёл. Пароль или иные secrets в spike не запрашивались и не сохранялись.

### Критерии приёмки

- [x] По deployed first-party portal и текущему Bambu Studio доказано, что Google/browser callback не может напрямую достичь ESP32 WebUI.
- [x] Реальным наблюдением подтверждено, что generic redirect на WebUI устройства отклоняется.
- [x] Для Google-created account через официальный `Forgot password` успешно задан Bambu-пароль и затем выполнен email/password login без раскрытия secrets.
- [x] Минимальный prototype и browser-ticket/callback contract документированы; OAuth branch исключён из T04.

### Заблокирован

Завершён. Принятый email/password flow выполним; Google OAuth callback намеренно не поддерживается.

---

## T04. Реализовать direct Bambu Cloud end to end

### Что построить

Реализовать полный пользовательский путь внутри `BambuCloud`: WebUI email/password login, email/SMS verification, TOTP, UID и список принтеров, явный выбор serial, MQTT connect/subscribe/pushall, первый валидный report, refresh, logout и повторный login. Для Google-created account WebUI объясняет одноразовую предпосылку — сначала задать Bambu-пароль через официальный `Forgot password`. Google OAuth/browser ticket не реализуется. WebUI показывает конечный автомат из ADR, а не токены и сетевые детали.

Credentials хранятся как versioned active/candidate transaction с checksum. Candidate становится active только после проверки account identity, получения списка принтеров, MQTT connect и первого валидного report; любая ошибка сохраняет прежнюю active запись. Пароль живёт только в одной login operation и стирается после результата/timeout.

Refresh выполняется только автоматически: заранее по expiry, немедленно после
подтверждённого MQTT auth reject и с bounded backoff после временных ошибок.
Ротированный refresh token до MQTT validation сохраняется вместе с account UID
в отдельный checksum-protected recovery slot и после reboot используется раньше
старого active token только для того же аккаунта. Ручной rotation flow и ручная
установка токена не входят в UX.

`DirectCloudSource` отдаёт snapshot через `PrinterStatusSource`. MQTT reports разбираются потоково с фиксированным бюджетом без полного payload/DOM; сохраняются только поля из ADR. `SecureNetworkCoordinator` не допускает параллельных TLS handshakes. До T05 renderer продолжает использовать relay, direct snapshot доступен только для безопасной диагностики.

Локальный WebUI/API этого flow не требуют admin password, session cookie или
CSRF по ADR 0002. Bambu password остаётся одноразовым входным значением Cloud
login и не сохраняется.

### Критерии приёмки

- [ ] Все обязательные email/password auth branches проходят через WebUI, включая invalid credentials, verification/TOTP, recoverable errors, printer selection, logout и повторный login; для Google-created account показана ссылка/инструкция по одноразовой установке Bambu-пароля.
- [ ] Verification result связан с одноразовым operation ID/TTL; пароль никогда не сохраняется; открытый локальный API, UI storage и logs не раскрывают secrets.
- [ ] Active/candidate transaction переживает ошибку или reboot без смешанного состояния; refresh с rotation и без неё сохраняет рабочий rollback.
- [x] Автоматический refresh policy покрывает expiry/unknown-expiry, печать, MQTT auth reject, bounded retry и crash-safe recovery последнего ротированного refresh token; ручной refresh UI/API отсутствует.
- [ ] MQTT parser корректно обрабатывает fragment boundary внутри key/string/escape/number, partial reports и oversized/malformed input без аллокации `total_data_len` и без потери последнего snapshot.
- [ ] Direct snapshot содержит все используемые сейчас printer fields и совпадает с relay на компактном representative corpus.
- [ ] Одновременно существует не более одного TLS-сеанса; auth завершается до MQTT, refresh полностью освобождает MQTT transport.
- [ ] Узкие host-тесты покрывают parser boundary, critical lifecycle mapping и credential commit/rollback; production build проходит и укладывается в flash gate.
- [ ] На реальном устройстве happy path до первого direct snapshot проходит, пока renderer продолжает использовать relay.

### Заблокирован

- T01 — credentials и TLS/MQTT добавляются только после resource/security baseline.
- T02 — direct transport подключается к единому state seam.
- T03 — зафиксировал email/password-only scope и одноразовую предпосылку для Google-created account.
- Реальный ESP32-C3 login 2026-09-01 получает Cloudflare `403` до Bambu
  application response и до `loginType`. State machine, verification/TOTP,
  credential transaction и refresh implementation существуют, но real-account
  acceptance этого тикета заблокирован до выбора автономного transport в
  [AT01–AT06](./0002-bambu-auth-transport-spikes.md). Ручной token, companion или
  router login не закрывают критерий.

---

## T05. Доказать устойчивость C3 и провести shadow/canary

### Что построить

Довести direct Cloud до production-устойчивости и проверить его на C3. `SecureNetworkCoordinator` должен сериализовать MQTT, refresh, weather HTTPS/sunrise и OTA. Добавить bounded recovery после Wi-Fi, DNS, MQTT и Bambu API failures. Затем включить shadow mode: relay остаётся renderer source, direct snapshot сравнивается локально по нормализованным полям, а diagnostics хранит только mismatch counters/reason codes.

После устранения расхождений добавить WebUI-настройку `relay | cloud_direct`, выполнить canary и 72-часовой soak. Переключение источника работает без reboot; relay и OTA rollback остаются доступными. Если численные или обязательные функциональные ворота C3 не проходят, зафиксировать измерения и перевести дальнейшую реализацию на ESP32-S3 без удаления функций.

Постоянное разрешение владельца на диагностический Bambu login, verification,
refresh, directory и MQTT telemetry и локальный ignored credential source
зафиксированы в [auth transport contract](./0002-bambu-auth-transport-spikes.md).
Повторно спрашивать разрешение на эти account operations не нужно. Deploy/OTA,
запуск реальной печати, bind/unbind и изменение аккаунта этим разрешением не
покрыты.

### Критерии приёмки

- [ ] Weather HTTPS/sunrise не конкурирует с MQTT во время печати; OTA блокирует новые операции и останавливает MQTT до flash write.
- [ ] Wi-Fi, DNS, MQTT и API failures восстанавливаются без reboot, secrets или unbounded allocations; последний overlay удерживается до валидного состояния.
- [ ] Shadow сравнивает все поля snapshot; diagnostics не хранит job/payload/secrets; representative corpus и реальные печати не имеют необъяснённых расхождений.
- [ ] WebUI переключает `relay | cloud_direct` без reboot; ручной rollback и OTA boot rollback проверены.
- [ ] Canary подтверждает Cloud Mode, Bambu Handy и отсутствие регрессий часов, погоды, яркости, AP, WebUI, signed OTA и overlay semantics.
- [ ] Image ≤ `1 750 000`, minimum heap ≥ 50 KB, largest block ≥ 32 KB, каждый task reserve ≥ 2 KB.
- [ ] 72-часовой soak с несколькими reconnect не показывает монотонной утечки heap/largest block.
- [ ] При провале ресурсных ворот принято явное решение C3 или S3; security level 0 остаётся осознанной локальной моделью доступа.

### Заблокирован

T04 и AT06 — локальный hardening реализуем независимо, но shadow/canary, real
direct login и 72-hour soak нельзя считать завершёнными, пока автономный auth
transport не получает Bambu application response и не проходит полный T04 flow.
Transport spikes не добавляют новых сценариев к T05 и не заменяют его численные
ворота.

---

## T06. Сделать direct Cloud автономным production-default

### Что построить

После успешного canary сделать `cloud_direct` источником по умолчанию для новых и мигрированных устройств. Проверить холодный старт, штатный refresh и восстановление после сети без OpenWrt/Python relay или другого внешнего процесса. Обновить пользовательскую и эксплуатационную документацию: onboarding, миграция, диагностика и rollback.

`RelayHttpSource`, relay deployment/config и runbook сохраняются рабочими минимум один стабильный release. Удаление relay-кода после переходного окна требует отдельного решения и не входит в этот тикет.

### Критерии приёмки

- [ ] После нового onboarding или migration устройство использует direct Cloud и показывает все printer fields без relay host.
- [ ] Cold boot, reconnect и token refresh проходят автономно; принтер остаётся в Cloud Mode и Bambu Handy.
- [ ] Финальный release-candidate smoke повторяет resource, behavior и security-level-0 access checks T05 без новых регрессий.
- [ ] Документированы onboarding, migration, диагностика, WebUI rollback, OTA rollback и известный риск недокументированного Cloud contract.
- [ ] Relay source и runbook сохранены и проверены как аварийный fallback минимум на один стабильный release.
- [ ] Нет скрытого fallback на LAN MQTT, companion script или постоянно работающий внешний relay.

### Заблокирован

T05 — production default меняется только после успешного shadow, canary и C3/S3 decision.
