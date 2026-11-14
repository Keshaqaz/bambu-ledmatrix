---
status: proposed
date: 2026-08-27
amended: 2026-09-03
---

# Оптимизировать ESP32-C3 и перенести Bambu Cloud relay в прошивку

> Локальные access-control требования этого ADR (пароль AP, WebUI session,
> CSRF и обязательная авторизация OTA) заменены
> [ADR 0002](./0002-use-security-level-zero-for-local-access.md). Остальные решения о
> Bambu credentials, TLS, ресурсах, rollback и поведении сохраняются.

Устройство должно сохранить Cloud Mode принтера и все отображаемые данные Bambu, но перестать зависеть от постоянно работающего relay на роутере. Мы сначала оптимизируем и инструментируем текущую ESP32-C3, затем переносим облачную авторизацию, MQTT и нормализацию состояния за единый интерфейс в прошивку; ESP32-S3 остаётся запасным аппаратным вариантом только при провале измеримых ресурсных ворот C3.

## Контекст

Текущая система состоит из двух исполняемых частей:

```text
Bambu Cloud MQTT
        │
        ▼
Python relay на OpenWrt
        │ HTTP /status, polling 5–30 секунд
        ▼
ESP32-C3 → BambuState → рендер матрицы и WebUI
```

Relay выполняет четыре существенные обязанности:

1. Получает или принимает Bambu Cloud credentials, определяет account UID и serial принтера.
2. Поддерживает TLS/MQTT-сеанс с Bambu Cloud и подписку на `device/<serial>/report`.
3. Извлекает состояние печати из полных и частичных MQTT reports.
4. Нормализует жизненный цикл, stale/offline и переход `finished → idle`, после чего отдаёт снимок по HTTP.

Прошивка повторно нормализует часть статусов relay и удерживает последний видимый оверлей при краткой потере связи. Поэтому сегодняшняя архитектура не только требует внешнего процесса, но и разделяет владение состоянием принтера между двумя местами.

## Обязательные ограничения

Решение считается допустимым только при одновременном выполнении всех условий:

- Принтер остаётся в Cloud Mode. LAN Only и Developer Mode не являются допустимыми режимами работы.
- Bambu Handy и штатные облачные функции принтера продолжают работать.
- Устройство сохраняет все используемые сейчас данные: online/connected, lifecycle, progress, job name, stage, current/total layers, remaining time и error state.
- Первичная авторизация и выбор принтера выполняются через встроенный WebUI.
- Пароль Bambu не сохраняется после завершения попытки входа.
- После первичной настройки устройство не зависит от скриптов, daemon-процессов или relay вне него.
- Сохраняются существующие часы, погода, автояркость, captive portal, WebUI, OTA с двумя слотами, boot guard и удержание оверлея.
- Миграция должна иметь обратимый период совместимости с текущим relay.

## Исходный ресурсный бюджет

Измерения относятся к прошивке `4.0.11-2026.08.20` и разметке `min_spiffs` с двумя OTA-слотами по `1 966 080` байт.

| Метрика | Текущее значение |
|---|---:|
| Размер приложения ESP32-C3 | 1 352 287 байт |
| Свободно в одном OTA-слоте | 613 793 байта |
| Статическая dynamic memory | 46 596 байт |
| Свободный heap живого устройства во время печати | около 133 KB |
| Стек weather task | 20 480 байт |
| Стек Wi-Fi connect task | 8 192 байта |
| Стек relay polling task | 12 288 байт |
| Несжатый WebUI | 54 469 байт |
| WebUI после gzip `-9` | 15 834 байта |

Тестовая линковка текущей прошивки с принудительно подключённым нативным `esp_mqtt_client`, TLS URI, subscribe и publish дала:

| Метрика | MQTT probe |
|---|---:|
| Размер приложения | 1 400 991 байт |
| Прирост flash относительно baseline | 48 704 байта |
| Прирост статической RAM | 24 байта |
| Остаток OTA-слота | 565 089 байт |

Следовательно, flash не является первичным ограничением. Риск находится в пиковом использовании внутреннего heap: TLS buffers, стеки FreeRTOS, временные строки и разбор крупных MQTT reports.

## Решение

### 1. ESP32-C3 остаётся первой целевой платформой

Аппаратная замена не выполняется до завершения оптимизаций и измерений. Прямая облачная интеграция активируется на C3 только после прохождения ресурсных и регрессионных ворот этого ADR.

ESP32-S3 с PSRAM остаётся переносимым fallback: интерфейс и тесты не должны зависеть от C3, но увеличение аппаратных ресурсов не используется как замена дисциплине памяти.

### 2. Состояние принтера получает единственного владельца

В прошивке вводится глубокий модуль `PrinterStatusSource`. Его внешний интерфейс возвращает только нормализованное состояние принтера и качество источника:

```cpp
struct PrinterStateSnapshot {
  bool configured;
  bool transportConnected;
  bool printerOnline;
  PrinterLifecycle lifecycle;
  PrinterLifecycle visibleLifecycle;
  uint8_t progress;
  uint16_t currentLayer;
  uint16_t totalLayers;
  uint32_t remainingSeconds;
  char jobName[96];
  char stage[48];
  char detail[96];
};

PrinterStateSnapshot snapshot();
```

Интерфейс не раскрывает MQTT topics, HTTP endpoints, токены, JSON или правила reconnect. Рендер матрицы и WebUI читают один и тот же snapshot.

На период миграции у интерфейса два production-адаптера:

- `RelayHttpSource` — существующий путь и быстрый rollback;
- `DirectCloudSource` — целевой путь без внешнего relay.

Тестовый replay-адаптер воспроизводит зафиксированные обезличенные MQTT reports. После удаления relay этот seam сохраняется как поверхность тестирования и возможного аварийного возврата в течение переходного окна.

### 3. Прямая облачная интеграция скрывается внутри `BambuCloud`

`DirectCloudSource` использует внутренний модуль `BambuCloud`, который владеет полным жизненным циклом интеграции:

- первичная авторизация;
- challenge `verifyCode` и TOTP;
- email/password login с обработкой verification/TOTP через WebUI;
- получение UID и списка принтеров;
- выбор и сохранение serial;
- access/refresh token lifecycle;
- TLS/MQTT connect, subscribe, pushall, keepalive и reconnect;
- фрагментированный разбор reports;
- stale/offline и нормализация lifecycle;
- атомарная замена credentials с rollback.

Зависимости Bambu Cloud являются истинно внешними. Внутренние seams `CloudHttpTransport`, `CloudMqttTransport`, `CredentialStore` и `Clock` получают production-адаптеры и scripted test adapters; наружу они не экспортируются.

### 4. Первичная авторизация выполняется только через WebUI

WebUI представляет конечный автомат, а не отдельные токены или сетевые операции:

```text
signed_out
  → submitting_credentials
  → verification_required(email | sms | totp)
  → fetching_printers
  → printer_selection_required
  → connecting
  → ready

Любой переход может завершиться recoverable_error.
Выход пользователя переводит состояние в signed_out и удаляет облачную сессию.
```

Минимальный интерфейс WebUI:

```text
GET  /api/bambu/auth/status
POST /api/bambu/auth/login
POST /api/bambu/auth/verify
GET  /api/bambu/printers
POST /api/bambu/printer/select
POST /api/bambu/logout
```

Правила безопасности интерфейса:

- GET-ответы никогда не содержат access token, refresh token, пароль или полный Bambu response.
- Пароль существует только в памяти активной login operation, не логируется и затирается после результата или timeout.
- Verification code имеет одноразовый operation ID и ограниченный срок жизни, чтобы старый код нельзя было применить к новой попытке.
- Credentials сохраняются только после проверки account UID и успешного получения списка принтеров.
- Выбранная облачная сессия считается рабочей только после успешного MQTT connect и первого валидного printer report.

Email/password, email/SMS verification и TOTP реализуются непосредственно устройством. Google OAuth/browser ticket намеренно не поддерживается: T03 подтвердил, что официальный Studio/Suite callback ограничен loopback браузерного хоста, а generic redirect на WebUI устройства отклоняется. Companion script, ручная вставка токена и постоянно работающий внешний callback не являются принятым UX этого ADR.

Для аккаунта, первоначально созданного через Google, обязательна одноразовая подготовка аккаунта: владелец задаёт Bambu-пароль через официальный `Forgot password`, затем авторизуется на устройстве по email/password. Это не посредник в работе устройства и не перенос Cloud session вручную. На 2026-08-28 пользователь подтвердил на существующем Google-created account успешную установку Bambu-пароля и последующий email/password login; secrets проекту не передавались и не сохранялись.

#### Совместимость транспорта первичной авторизации

Проверка на ESP32-C3 2026-09-01 обнаружила отдельный от verification контракт:
`POST /v1/user-service/user/login` с устройства завершается Cloudflare `403`
до Bambu JSON-ответа и до выбора `loginType`. Поэтому наличие реализованных
веток email/SMS/TOTP само по себе не завершает первичную авторизацию.

Существующий OpenWrt relay не опровергает этот результат. Его постоянный процесс
не выполняет password-login: он принимает заранее выданный access token, а
первичный login с verification выполнялся одноразовым Python-инструментом на
компьютере через системный TLS stack. После bootstrap relay использует token для
REST/MQTT и при наличии refresh token автономно вызывает `refreshtoken`.

Требование автономности не меняется: password-login, verification и refresh
должны выполняться устройством без ручной установки token, companion script,
роутера или постоянного callback. Для квалификации транспорта вводится отдельный
короткий экспериментальный поток с отдельной production-интеграцией
[Bambu auth transport](../development-tickets/0002-bambu-auth-transport-spikes.md).
До его завершения direct login считается заблокированным на уровне Cloud
transport, а не credentials, verification logic или ресурсов C3.

Кандидаты проверяются в таком порядке:

1. стандартный Mbed TLS/ESP-TLS с явно заданными поддерживаемыми параметрами;
2. auth-only wolfSSL transport с полной проверкой server certificate;
3. минимальный изолированный ClientHello-compatible transport, если стандартные
   stacks не дают Bambu application response.

Критерий успеха transport spike — Bambu application JSON для синтетической
невалидной попытки вместо Cloudflare HTML/`403`. Реальные credentials для этого
не нужны. Успех transport ещё не означает принятие реализации: кандидат должен
затем пройти существующие credential-erasure, single-TLS, signed image и C3
resource gates. Подмена только HTTP headers не считается решением, а отключение
certificate validation запрещено.

Решение transport-потока (AT05, 2026-09-02) зафиксировано в
[AT05-decision](../research/bambu-auth-transport/AT05-decision.md). Статусы
AT01–AT05:

- **AT01** complete — desktop контроль `PASS`, production ESP
  `TRANSPORT_BLOCKED`; oracle и wire baseline зафиксированы.
- **AT02** complete, `NO PASS` — все применимые штатные профили Mbed TLS
  `BLOCKED`; TLS 1.2 desktop-order профиль — локальный `TRANSPORT_ERROR` до
  HTTP-ответа.
- **AT03** complete, `NO PASS` — wolfSSL 5.8.4 после исправления CA-цепочки
  воспроизводимо `BLOCKED` на уровне fingerprint; независимо от вердикта
  кандидат отклонён по GPLv3 и отсутствию официального компонента для
  ESP-IDF 5.5.x.
- **AT04** complete, `PASS` ×13 — и доказано, что дискриминатором Cloudflare
  был не TLS ClientHello, а заголовок `User-Agent: ESP32HTTPClient`, который
  `HTTPClient` молча подставляет вместо отбрасываемого ядром `addHeader`;
  production-идентичность `bambu_network_agent/02.07.01.51` фактически никогда
  не отправлялась.
- **AT05** decided 2026-09-02 — выбран кандидат, пересмотр ADR не требуется.

Выбранный production transport — действующий штатный Mbed TLS/HTTPClient стек
без замены TLS: заявленная идентичность клиента восстанавливается через
`HTTPClient::setUserAgent("bambu_network_agent/02.07.01.51")` вместо молча
отбрасываемого `addHeader`. Это минимальная форма кандидата AT04: profiles 10 и
12 доказали на байт-идентичном ClientHello, что вердикт определяется только
этой строкой. Уточнение к правилу выше: коррекция заголовка принята не как
замена transport-квалификации, а потому что (а) она восстанавливает
идентичность, уже заявленную остальными `X-BBL-*` заголовками, а не подменяет
произвольное значение; (б) `PASS` измерен оракулом 13 раз; (в) реализация
всё равно проходит полные ворота T04 acceptance (AT06), затем T05 canary/soak
и C3 resource gates. Отключение certificate validation по-прежнему запрещено.

Fallback (не является target'ом AT06): прямой stock-Mbed TLS адаптер AT04
(измеренный `PASS` ×13) применяется только если wrapper-путь не воспроизведёт
`PASS` на устройстве; такой исход требует addendum к AT05-decision, после чего
адаптер становится единственным target'ом. Регрессионный детектор — пара
oracle-профилей AT04 10 (`PASS`) / 12 (`BLOCKED`) — отслеживает, не начнёт ли
Cloudflare блокировать и заявленный UA. T05 остаётся заблокированным до
завершения AT06; матрица T05 и численные C3 gates не расширены и не ослаблены.

### 5. Credentials заменяются атомарно

Credential store содержит active и candidate записи с версией, checksum и состоянием транзакции:

```text
получить candidate tokens
  → проверить HTTPS account identity
  → получить список принтеров
  → сохранить candidate
  → подключить MQTT с candidate
  → получить валидный report выбранного принтера
  → атомарно сделать candidate active
  → удалить прежний active после commit
```

При любой ошибке candidate удаляется, прежняя active запись остаётся рабочей.
Refresh полностью автономен: WebUI не предлагает ручную ротацию. Устройство
обновляет credentials в семидневном warning window, откладывая не срочную
операцию во время печати; при остатке не более 24 часов обновляет и во время
печати. Отсутствующие expiry сначала проверяются через 5 минут после boot, затем
не реже чем раз в 30 дней uptime. Подтверждённый MQTT `not authorized` запускает
refresh немедленно. Временные ошибки повторяются с bounded backoff
`1 min → 5 min → 15 min → 1 h → 6 h` без ручного вмешательства. При
подтверждённом истечении во время печати последний видимый оверлей удерживается
до восстановления MQTT.

Поскольку Cloud может инвалидировать старый refresh token уже в момент выдачи
ротированного, самый новый token вместе с account UID записывается в отдельный
versioned, checksum-protected NVS recovery slot до сохранения candidate и
MQTT-проверки.
Rollback сохраняет прежнюю active сессию, но следующая попытка всегда использует
recovery token; после reboot незавершённый refresh продолжается сразу. Recovery
slot удаляется только после успешного promotion candidate. Это осознанный выбор
надёжности: security level 0 допускает долговременное хранение Cloud credentials
во flash ESP. Штатная ротация не требует повторного login; он нужен только после
внешнего отзыва/инвалидации аккаунта либо несовместимого изменения Bambu contract.

Пароль аккаунта не хранится. Refresh token считается долгоживущим credential
уровня аккаунта, но в принятой модели security level 0 хранится во внутреннем
NVS без отдельного user password/encryption gate; этот риск принят ради
автономного восстановления.

### 6. Одновременно существует не более одного TLS-сеанса

C3 не должен одновременно держать Cloud MQTT TLS и запускать другой TLS handshake.

- Login, verification, UID lookup и printer discovery происходят до запуска MQTT.
- Для refresh MQTT-клиент останавливается и полностью освобождает transport/TLS, затем выполняется HTTPS refresh и создаётся новый MQTT-клиент.
- Обычные погодные запросы сохраняют текущий HTTP-first путь.
- HTTPS fallback погоды и запрос sunrise не стартуют во время активной печати; они откладываются до idle либо выполняются после контролируемого MQTT reconnect с удержанием оверлея.
- OTA запрещает начало новых сетевых операций и корректно останавливает MQTT до записи flash.

Сериализация реализуется одним `SecureNetworkCoordinator`, а не набором независимых mutex вокруг каждого вызова.

### 7. MQTT reports разбираются потоково и с жёстким бюджетом

`esp_mqtt_client` передаёт крупный report фрагментами через `data_len`, `total_data_len` и `current_data_offset`. Прошивка не собирает полный payload в `String` и не создаёт полный JSON DOM.

Инкрементальный parser сохраняет только поля, необходимые существующему интерфейсу:

```text
gcode_state
mc_percent
stg_cur / stage
print_error
hms
subtask_name / gcode_file
layer_num
total_layer_num
mc_remaining_time / remaining_time
```

Неизвестные поля и поддеревья пропускаются. Parser обязан корректно переживать границу MQTT fragment внутри имени ключа, строки, escape sequence и числа. Память parser/ring buffer ограничивается заранее; oversized или malformed report отбрасывается без очистки последнего валидного состояния.

### 8. Перед Cloud migration выполняются оптимизации без изменения поведения

Обязательный набор:

1. WebUI экспортируется как детерминированный gzip asset и отдаётся из flash с `Content-Encoding: gzip`. Это экономит около 38 635 байт flash без изменения HTML/JS.
2. Ответы `/api/settings`, `/api/status` и auth endpoints сериализуются напрямую в HTTP stream без промежуточного `String` с полной копией JSON.
3. Wi-Fi connect task заменяется неблокирующей state machine в Arduino `loop()`. Фактические вызовы Wi-Fi уже выполняются там; отдельная задача преимущественно координирует флаги и резервирует 8 KB стека.
4. После переключения на direct Cloud удаляется relay polling task со стеком 12 KB. Нативный MQTT task получает измеренный, а не максимальный «на глаз» stack budget.
5. Legacy runtime buffers для старых ESP-side Bambu credentials удаляются после отдельной миграции NVS; старые layouts читаются через явные legacy structs.
6. Все динамические JSON allocations получают фильтр, верхний предел и обработку `NoMemory`.
7. FastLED сохраняется: его измеренный вклад около 13.6 KB flash несопоставим с риском замены LED transport и цветовой математики.

Сокращение weather/MQTT stack допускается только после измерения high-water mark на живом устройстве. Значение стека не уменьшается только на основании успешной компиляции.

### 9. Локальный доступ использует security level 0

Согласно [ADR 0002](./0002-use-security-level-zero-for-local-access.md), onboarding AP,
WebUI и локальный API открыты без password/session/CSRF. Это принятый risk
profile домашнего устройства и не является воротами для хранения Cloud
credentials. При этом Bambu-пароль не сохраняется, токены не возвращаются через
API и не логируются, Cloud TLS остаётся обязательным, а signed OTA, два app-слота
и boot guard сохраняют целостность и recoverability firmware.

## Целевая архитектура

```text
WebUI ── login / verify / select / logout ─┐
                                          ▼
                                  ┌──────────────┐
                                  │ BambuCloud  │
                                  │ auth/token  │
                                  │ directory   │
                                  │ MQTT/TLS    │
                                  └──────┬───────┘
                                         │ normalized reports
                                         ▼
                                ┌─────────────────────┐
                                │ PrinterStatusSource │
                                └─────────┬───────────┘
                                          │ PrinterStateSnapshot
                         ┌────────────────┴────────────────┐
                         ▼                                 ▼
                  Matrix renderer                    Status WebUI
```

После завершения миграции printer не входит в эту схему как LAN endpoint: данные всегда приходят через Bambu Cloud, а принтер продолжает работать в Cloud Mode.

## Этапы поставки

### Этап 0 — наблюдаемость

Добавить в диагностику, не раскрывая credentials:

- current/minimum internal heap;
- largest free internal block;
- high-water mark loop, weather, Wi-Fi и printer tasks;
- MQTT fragment total/max size;
- reconnect count и причина последнего disconnect;
- auth/refresh state без токенов;
- число отвергнутых malformed/oversized reports.

Снять baseline на текущей relay-прошивке: boot, AP+STA, WebUI load/save, weather HTTP/HTTPS fallback, OTA и активная печать.

### Этап 1 — безопасная оптимизация

Ввести gzip WebUI, streaming JSON responses, Wi-Fi state machine без отдельной задачи и bounded JSON parsing. Каждое изменение поставляется отдельно и проверяется существующими сценариями до следующего шага.

### Этап 2 — единый владелец состояния принтера

Извлечь lifecycle/stale/overlay rules в `PrinterStatusSource`. Перенести тесты relay semantics на его внешний интерфейс и проигрывать полные, частичные, malformed и stale reports без сети.

### Этап 3 — авторизация через WebUI

Реализовать email/password login state machine, verification branches, список принтеров, выбор serial, logout и атомарный credential store. Для Google-created account WebUI показывает одноразовую предпосылку и ссылку на официальный `Forgot password`; OAuth/browser ticket branch отсутствует.

Перед real-account acceptance квалифицировать автономный auth transport по
экспериментальным тикетам: получить Bambu application response на ESP, выбрать
наименьший проходящий стандартный stack/profile и только при их провале
переходить к изолированной ClientHello-совместимости. Эксперименты не меняют
production source и не расширяют T05 canary/soak matrix.

### Этап 4 — direct Cloud MQTT в shadow mode

Одновременно получать состояния через relay и direct Cloud, но продолжать рендерить relay snapshot. Сравнивать нормализованные состояния локально без отправки credentials или print data наружу. Расхождения записывать как коды/счётчики, а не полные payloads.

### Этап 5 — canary с мгновенным rollback

Добавить настройку источника `relay | cloud_direct`. Включить direct Cloud на реальном устройстве, сохранив relay и возможность переключения через WebUI или OTA rollback.

### Этап 6 — автономная эксплуатация

После прохождения ворот сделать `cloud_direct` значением по умолчанию. Relay на роутере перестаёт быть эксплуатационной зависимостью. Код, конфигурация и runbook relay сохраняются в репозитории минимум на один стабильный release как средство аварийного возврата.

*Поправка от 2026-09-03 (решение владельца, не молчаливое ослабление ворот):*
перевод `cloud_direct` в production-default (`4.4.0-2026.09.03`) выполнен после
T05 canary (живая проверка source switching и resilience 2026-09-03), но с
отложенным вердиктом 72-часового soak — он продолжается на устройстве
(автономный журнал `/api/soak`) и передан в техдолг вместе с расследованием
двух `int_wdt`-сбросов в soak-окне и калибровкой `heapInternalMinimum` gate
(TLS low-water легитимно уводит минимум ниже 50 KB). Откат доступен двумя
уровнями: WebUI-переключение на relay без перезагрузки и OTA boot-rollback;
relay-код и runbook сохранены. Формальное закрытие RAM-gate вердикта soak
остаётся обязательным до удаления relay-кода.

## Ворота принятия C3

Прямая облачная интеграция считается пригодной для ESP32-C3 только при выполнении всех критериев:

### Flash

- Размер production image не более `1 750 000` байт при OTA-слоте `1 966 080` байт.
- Сжатый WebUI воспроизводится побайтно после распаковки и проходит существующую export-проверку.

### RAM

- Minimum free internal heap не ниже 50 KB во всех тестовых сценариях.
- Largest free internal block не ниже 32 KB.
- High-water reserve каждого task не ниже 2 KB.
- Ни один MQTT report не требует аллокации полного `total_data_len`.
- После 72 часов и нескольких reconnect нет монотонного падения free heap или largest block.

### Поведение

- Принтер остаётся в Cloud Mode, одновременно доступен в Bambu Handy.
- Progress и lifecycle совпадают с relay на representative corpus и реальных печатях.
- Частичный MQTT report не очищает ранее известные поля.
- `unknown/offline` не снимает видимый оверлей; подтверждённый `idle` снимает.
- `finished` удерживается и переходит в `idle` по прежнему контракту.
- Потеря Wi-Fi, Cloud MQTT, DNS и Bambu API восстанавливается без ручного reboot.
- Weather, clock, brightness, AP setup, WebUI save, signed OTA и boot rollback проходят регрессию.

### Авторизация

- Синтетический invalid-login transport probe с ESP получает Bambu application
  JSON, а не Cloudflare block page; server certificate проверяется.
- Успешны direct login, invalid credentials, email/SMS verification, TOTP, logout и повторный login.
- Google-created account после одноразовой установки Bambu-пароля проходит тот же WebUI email/password flow без ручного токена, companion script или внешнего callback.
- Refresh проверяется с token rotation и без неё.
- Ошибка candidate token возвращает точную предыдущую active запись.
- Устройство с несколькими принтерами требует явного выбора и сохраняет его.

### Безопасность

- Secrets отсутствуют в Serial, API responses, diagnostics, crash logs и browser storage.
- AP, WebUI и локальный API намеренно открыты по security level 0 из ADR 0002.
- Нельзя установить неподписанный firmware image.

Если RAM gate не может быть выполнен на C3 без удаления обязательной функции,
целевой hardware меняется на ESP32-S3 с PSRAM; локальная авторизация не является
gate по принятой security level 0 модели.

## Рассмотренные варианты

### Оставить relay на роутере

Отклонено как целевая архитектура: сохраняет внешнюю эксплуатационную зависимость, усложняет установку и разделяет владение состоянием принтера. Сохраняется временно для shadow comparison и rollback.

### Перейти на LAN MQTT

Отклонено: нарушает обязательное требование Cloud Mode и может лишить пользователя штатного облачного сценария и Bambu Handy.

### Сразу перейти на ESP32-S3

Пока отклонено: измеренная стоимость нативного MQTT по flash мала, а часть RAM можно вернуть без удаления функций. S3 остаётся заранее определённым fallback, если живые измерения C3 не проходят ворота.

### Перенести текущий Python relay буквально

Отклонено: буквальный перенос сохранил бы дублирование нормализации, полные JSON allocations и несколько независимых сетевых жизненных циклов. Выбран единый владелец состояния и потоковый transport-aware implementation за малым интерфейсом.

### Использовать другой TLS stack только для Bambu auth

Принято к ограниченной проверке. ESP-TLS допускает альтернативный stack, поэтому
wolfSSL проверяется как auth-only adapter без замены MQTT/weather/OTA transport.
Кандидат принимается только с server certificate validation и после измерения
flash, minimum heap и largest block на C3.

### Настроить стандартный Mbed TLS профиль

Принято как первый и самый дешёвый эксперимент. Проверяются только параметры,
поддерживаемые штатным API: TLS version, SNI, ALPN, session tickets и ограниченный
список cipher suites. Матрица фиксирована тикетом и не превращается в перебор
случайных HTTP headers.

### Реализовать ClientHello-совместимый auth transport

Условно принято только после документированного провала стандартного Mbed TLS и
wolfSSL. Изменение ограничивается Bambu pre-auth HTTPS, не отключает проверку
сертификата и не копирует закрытые Bambu credentials или proprietary plugin.
Решение обязано иметь зафиксированный wire-profile, детектор Cloudflare block и
безопасный OTA rollback, потому что Cloud-side fingerprint policy нестабильна.

### Выполнять Cloudflare browser challenge на C3

Отклонено. Полноценный browser/JavaScript runtime несоразмерен C3, расширяет
attack surface и не даёт стабильного контракта. Официальный browser-ticket или
device-code flow станет предпочтительным только если Bambu предоставит callback,
достижимый устройством, либо поддерживаемый polling contract.

### Использовать официальный партнёрский контракт Bambu

Не исключено как будущее предпочтительное направление. Текущий проект может продолжать direct Cloud только с пониманием, что используемые Cloud endpoints и MQTT credentials не являются гарантированным публичным контрактом. Если Bambu предоставит поддерживаемый способ авторизации и telemetry, он заменяет внутренние Cloud transport adapters без изменения `PrinterStatusSource`.

## Последствия

Положительные:

- конечному пользователю нужен только принтер в Cloud Mode, Wi-Fi и WebUI устройства;
- исчезает обязательный OpenWrt/Python deployment;
- рендер и WebUI получают одно согласованное состояние;
- C3 сохраняется, если измерения подтверждают запас;
- transport и auth можно тестировать через scripted adapters;
- переход имеет shadow mode и быстрый rollback.

Отрицательные:

- firmware начинает хранить credential уровня Bambu account и требует серьёзного усиления WebUI/OTA;
- недокументированный Cloud contract может измениться независимо от релиза устройства;
- Google-created account требует одноразовой установки отдельного Bambu-пароля;
- строгая сериализация TLS усложняет планирование refresh и HTTPS fallback;
- C3 требует постоянных memory gates и не допускает неограниченных JSON/String allocations.

## Условия пересмотра

ADR пересматривается, если происходит хотя бы одно событие:

- Bambu предоставляет поддерживаемую device authorization или telemetry integration;
- Bambu перестаёт позволять Google-created account задать Bambu-пароль или использовать email/password login;
- production firmware не проходит RAM/flash gates C3;
- Bambu вводит обязательный новый Cloud authentication/signing contract;
- ни один автономный auth transport candidate не получает Bambu application
  response без ослабления TLS или нарушения C3 resource gates;
- требования продукта расширяются до камеры, управления печатью или нескольких одновременных принтеров;
- безопасное хранение credentials требует новой partition table или производственного provisioning.

До принятия нового ADR ни одно из этих событий не должно молча приводить к ослаблению обязательных условий, отключению Cloud Mode или возврату скрытой зависимости от внешнего relay.
