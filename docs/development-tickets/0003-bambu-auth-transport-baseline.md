---
id: AT01
status: proposed
date: 2026-09-01
source: ./0002-bambu-auth-transport-spikes.md
---

# AT01. Зафиксировать transport oracle и wire baseline

## Цель

Получить быстрый воспроизводимый red/green сигнал именно для Cloudflare-блокировки
Bambu login и сравнимые wire-метаданные текущего ESP transport с одним успешным
desktop OpenSSL запросом. Этот тикет ничего не исправляет.

## Контекст для отдельной сессии

Проект находится в `esp32_matrix_controller22`. Текущий ESP32-C3 direct login
классифицирует Bambu Cloudflare HTML/`403` как `cloudflare_blocked`. Relay на
OpenWrt не выполняет password-login: bootstrap исторически выполнялся на
компьютере. ADR требует полностью автономную первичную авторизацию на ESP.

## Работа

1. Проверить production build и существующий `cloudflare_blocked` classifier.
2. Добавить минимальный agent-runnable probe/harness поверх реального auth
   transport. Использовать только синтетический invalid account/password.
3. Зафиксировать три исхода `TRANSPORT_PASS`, `TRANSPORT_BLOCKED`,
   `TRANSPORT_ERROR` из общего oracle без сохранения body/secrets.
4. Выполнить desktop OpenSSL и ESP запрос с одинаковым application payload.
5. Если доступен packet capture, сохранить только обезличенные ClientHello
   метаданные: TLS version, cipher/extension/group/signature/ALPN lists и
   JA3/JA4-подобное описание. Не сохранять application data или cookies.
6. Записать результат строго в
   `docs/research/bambu-auth-transport/AT01-baseline.md`: oracle command,
   firmware/toolchain versions, synthetic input class, desktop/ESP verdicts,
   classifier inputs/outputs, wire metadata или причину отсутствия capture и
   явный redaction check. Временную instrumentation удалить либо явно
   изолировать как probe.

## Критерии приёмки

- [ ] Одна команда за секунды/десятки секунд выдаёт ровно один oracle verdict.
- [ ] Текущий ESP path воспроизводит `TRANSPORT_BLOCKED` на устройстве.
- [ ] Desktop control достигает Bambu application response и даёт
  `TRANSPORT_PASS` без реальных credentials.
- [ ] Captured artifact не содержит account, password, token, cookie или body.
- [ ] Production firmware не меняет поведение и продолжает собираться.
- [ ] Записаны точные входы для AT02/AT03; если packet capture невозможен,
  перечислена недостающая физическая точка наблюдения без выдуманного diff.

## Пользовательское окно

Сначала подготовить локальную часть. Deploy/capture на устройстве выполнять
только после одного конкретного запроса разрешения с командами и оценкой времени.
Авторизация в Bambu не требуется.

## Стартовый промпт

```text
Выполни AT01 из docs/development-tickets/0003-bambu-auth-transport-baseline.md. Следуй ADR 0001 и общим ограничениям docs/development-tickets/0002-bambu-auth-transport-spikes.md. Построй минимальный red/green transport oracle для текущего Cloudflare 403, используя только синтетические invalid credentials. Не исправляй transport и не расширяй тесты до verification/MQTT/canary. Подготовь всё локально, а перед deploy или packet capture задай один конкретный вопрос с длительностью. В конце запиши полный обезличенный контракт и результаты в docs/research/bambu-auth-transport/AT01-baseline.md, приложи production build result и однозначные входы для AT02/AT03.
```
