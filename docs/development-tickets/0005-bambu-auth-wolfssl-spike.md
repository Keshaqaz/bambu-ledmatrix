---
id: AT03
status: proposed
date: 2026-09-01
source: ./0002-bambu-auth-transport-spikes.md
---

# AT03. Проверить auth-only wolfSSL

## Цель

Проверить, проходит ли Cloudflare отдельный wolfSSL ClientHello на ESP32-C3 и
можно ли изолировать его только в Bambu pre-auth/refresh HTTPS без замены MQTT,
weather и OTA transports.

## Предпосылка

Использовать oracle и synthetic input из
`docs/research/bambu-auth-transport/AT01-baseline.md`. По short-circuit policy
device-прогон AT03 нужен только если AT02 отсутствует или не дал `PASS`. Если
oracle отсутствует, не создавать дублирующую инфраструктуру.

## Работа

1. Проверить совместимость текущей Arduino/ESP-IDF toolchain с официальным
   ESP-WolfSSL component и его лицензией/способом поставки.
2. Собрать отдельный auth-only probe с CA bundle, SNI и hostname validation.
3. Не заменять глобальный ESP-TLS backend, если это затронет MQTT/OTA; держать
   adapter за существующим `CloudHttpTransport` seam.
4. Запустить AT01 oracle с синтетическими invalid credentials.
5. Для `PASS` измерить flash delta, current/minimum heap, largest block и task
   reserve во время handshake. Для `BLOCKED` не интегрировать stack дальше.
6. Удалить загруженные build artifacts; source dependency фиксировать только с
   версией и лицензией, если кандидат проходит.
7. Записать результат в
   `docs/research/bambu-auth-transport/AT03-wolfssl.md` по общему artifact contract.

## Критерии приёмки

- [ ] Probe получает однозначный `PASS`, `BLOCKED` или `ERROR` через AT01 oracle.
- [ ] TLS certificate и hostname проверяются; response classification bounded.
- [ ] wolfSSL не становится неявным backend для MQTT/weather/OTA.
- [ ] Зафиксированы dependency provenance/license, image delta и C3 RAM metrics.
- [ ] При `PASS` подготовлен минимальный adapter plan; production default не изменён.
- [ ] При `BLOCKED` dependency не остаётся в production build.

## Стартовый промпт

```text
Выполни AT03 из docs/development-tickets/0005-bambu-auth-wolfssl-spike.md на основе docs/research/bambu-auth-transport/AT01-baseline.md и ADR 0001; если AT02 уже дал PASS, закрой AT03 как not needed без device-прогона. Иначе проверь официальный ESP-WolfSSL как изолированный Bambu auth-only transport, используя существующий oracle и synthetic invalid credentials. Не заменяй MQTT/weather/OTA backend, не отключай certificate validation и не выполняй real login/canary/soak. Перед device deploy запроси одно разрешение. Запиши verdict, dependency/license provenance и flash/RAM deltas в docs/research/bambu-auth-transport/AT03-wolfssl.md; не оставляй wolfSSL в production при провале.
```
