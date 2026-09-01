---
id: AT05
status: proposed
date: 2026-09-01
source: ./0002-bambu-auth-transport-spikes.md
---

# AT05. Выбрать и зафиксировать production auth transport

## Цель

На основании завершённых spikes выбрать наименьший автономный transport,
достигший Bambu application response, либо документированно открыть пересмотр
ADR, если ни один кандидат не проходит без ослабления обязательных защит.

## Входы

- AT01 oracle и wire baseline;
- первый `PASS` по short-circuit order из AT02, AT03 или AT04;
- artifacts пропущенных более сложных кандидатов не требуются.

Этот тикет не повторяет эксперименты и не выполняет real-account login.

## Правило выбора

Предпочитать в порядке: штатный Mbed TLS profile → официальный auth-only wolfSSL
adapter → минимальный ClientHello-compatible adapter. Более сложный кандидат не
выбирается только из-за более близкого fingerprint, если более простой уже даёт
стабильный Bambu application response и проходит resource projection.

## Работа

1. Проверить полноту artifacts и отсутствие secrets.
2. Свести verdict, certificate validation, scope, flash/RAM и maintenance risk.
3. Обновить ADR 0001 фактическим решением и статусами AT01–AT05.
4. Записать решение в `docs/research/bambu-auth-transport/AT05-decision.md`.
5. Передать выбранный adapter в AT06; T05 остаётся заблокирован до production
   integration и полного login/verification acceptance T04.
5. Если кандидата нет, не ослаблять TLS/credentials/signed OTA/rollback и не
   объявлять S3 решением fingerprint; сформулировать конкретное условие
   пересмотра автономного Cloud requirement.

## Критерии приёмки

- [ ] Выбор или отказ следует воспроизводимым artifacts, а не предположению.
- [ ] ADR и master ticket map согласованы.
- [ ] AT06 имеет один точный production implementation/acceptance target.
- [ ] T05 matrix и численные C3 gates не расширены и не ослаблены.
- [ ] В документах и Git diff отсутствуют credentials, cookies и raw bodies.

## Стартовый промпт

```text
Выполни AT05 из docs/development-tickets/0007-bambu-auth-transport-decision.md. Не повторяй spikes: проверь канонические artifacts по short-circuit order, выбери первый прошедший автономный transport и запиши решение в docs/research/bambu-auth-transport/AT05-decision.md, затем синхронизируй ADR 0001 и master map. Не выполняй deploy, real login, MQTT, canary или soak. Если кандидата нет, зафиксируй пересмотр ADR без ослабления TLS, Bambu credential protection, signed OTA или rollback и без ложного утверждения, что S3 исправит Cloudflare fingerprint. При выборе кандидата передай один точный production target в AT06.
```
