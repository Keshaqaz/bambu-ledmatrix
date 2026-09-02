---
id: AT06
status: complete
date: 2026-09-01
completed: 2026-09-02
source: ./0002-bambu-auth-transport-spikes.md
---

# AT06. Интегрировать transport и закрыть T04 auth acceptance

## Цель

Интегрировать выбранный AT05 autonomous transport в production Bambu
login/verification/refresh path и завершить только незакрытые auth acceptance
сценарии T04. T05 shadow/canary/72-hour soak остаются отдельным этапом.

## Входы

- `docs/research/bambu-auth-transport/AT01-baseline.md`;
- `docs/research/bambu-auth-transport/AT05-decision.md`;
- artifact выбранного AT02, AT03 или AT04 кандидата;
- существующий T04 `CloudHttpTransport`, credential transaction,
  `SecureNetworkCoordinator` и WebUI auth state machine.
- локальный ignored credential file
  `esp32_matrix_controller22/.secrets/bambu-test.env` с mode `0600`.

При отсутствии выбранного `PASS` не начинать integration и сообщить один
blocker. Не переносить probe-only shortcuts в production.

## Работа

1. Подключить выбранный adapter только к Bambu pre-auth/verification/refresh
   HTTPS; не менять MQTT/weather/OTA transport без необходимости кандидата.
2. Сохранить CA/hostname validation, bounded response classification, password
   erasure, active/candidate/recovery transaction и single-TLS ownership.
3. Запустить существующие T04 host tests и production build; не дублировать
   parser/credential test matrix.
4. Зафиксировать image size и локальные resource deltas до device-прогона.
5. Подготовить одно окно владельца для signed OTA и T04 real scenarios: invalid
   credentials, direct login, выбранная сервером verification/TOTP ветка,
   printer selection, первый MQTT report, logout и повторный login. Не требовать
   искусственно обе verification ветки, если аккаунт/сервер выдаёт только одну;
   недостигнутую ветку оставить подтверждённой существующим contract test.
6. После успешного прогона записать sanitized result в
   `docs/research/bambu-auth-transport/AT06-integration.md` и разблокировать T05.

Для Bambu login отдельное разрешение не запрашивать: оно постоянно дано в
общем ticket contract. Не выводить credentials и не передавать их через command
arguments. Отдельно спросить только разрешение на deploy/OTA и физическое окно;
если сервер запросит одноразовый code, попросить его в рамках этого же окна.

## Критерии приёмки

- [x] Production path использует выбранный autonomous transport без external
  relay, companion, proxy или manual token bootstrap.
- [x] Certificate/hostname validation, credential erasure, signed OTA и rollback
  не ослаблены; одновременно существует не более одного TLS-сеанса.
- [x] Existing T04 host tests и production build проходят; image ≤ `1 750 000`.
- [x] Реальный login достигает Bambu application state, завершает необходимую
  verification и получает первый валидный Cloud MQTT report.
- [x] Logout и повторный login проходят; API/logs/artifact не раскрывают secrets.
- [x] T05 остаётся неизменным и начинается только после этого acceptance.

Sanitized результат: `docs/research/bambu-auth-transport/AT06-integration.md`
(production `4.3.11-2026.09.02`, oracle `TRANSPORT_PASS` ×2, image
1,386,725 B / 49,780 B globals, полный T04 acceptance на устройстве).

## Стартовый промпт

```text
Выполни AT06 из docs/development-tickets/0008-bambu-auth-transport-integration.md строго по выбранному кандидату из docs/research/bambu-auth-transport/AT05-decision.md. Интегрируй его в production Bambu pre-auth/verification/refresh path, сохрани CA/hostname validation, password erasure, active/candidate/recovery rollback, single-TLS coordination, signed OTA и boot rollback. Для разрешённых Bambu diagnostic login/verification/refresh используй только локальный ignored esp32_matrix_controller22/.secrets/bambu-test.env и не запрашивай повторного разрешения; никогда не печатай и не передавай credentials в arguments. Не меняй MQTT/weather/OTA transport без требования выбранного adapter, не дублируй T04 parser/credential tests и не запускай T05 canary/soak. Сначала заверши код, host tests и production build; затем задай один конкретный вопрос только на signed OTA/physical window. Запиши sanitized результаты в docs/research/bambu-auth-transport/AT06-integration.md и только после успеха разблокируй T05.
```
