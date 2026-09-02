---
id: AT04
status: complete
date: 2026-09-01
source: ./0002-bambu-auth-transport-spikes.md
---

# AT04. Проверить ClientHello-compatible auth transport

## Цель

После доказанного провала стандартных Mbed TLS profiles и wolfSSL создать
минимальный Bambu-auth-only transport, чей wire profile достигает Bambu
application response, сохраняя полноценную проверку server certificate.

## Жёсткая предпосылка

Начинать только если `AT02-mbedtls.md` и, когда он выполнялся,
`AT03-wolfssl.md` не содержат `PASS`. Допустимы воспроизводимый `BLOCKED` либо
документированная toolchain/resource incompatibility после устранения обычных
локальных ошибок. Необъяснённый `ERROR` сначала устраняется и не является
основанием для более сложного transport. При `PASS` стандартного stack тикет
закрывается как `not needed`. При отсутствии `AT01-baseline.md` сообщить blocker.

## Работа

1. Сопоставить обезличенные ClientHello control/ESP из AT01: version, cipher
   order, extensions, groups, signature algorithms, ALPN и session tickets.
2. Выбрать минимальный способ: scoped patch Mbed TLS либо custom ESP-TLS stack
   registration. Не писать TLS cryptography самостоятельно.
3. Изменять по одной wire-группе и запускать тот же AT01 oracle; ограничить
   эксперименты различиями из capture, а не произвольным fingerprint guessing.
4. Изолировать implementation только в Bambu pre-auth/refresh transport.
5. Сохранить CA/hostname validation, bounded response reader, secret erasure и
   `SecureNetworkCoordinator` ownership.
6. Зафиксировать profile как reviewable data/code, upstream base/version,
   maintenance risk и детектор Cloudflare regression.
7. При `PASS` измерить production image projection и C3 handshake RAM. Полный
   T04 real login и T05 canary/soak остаются следующими этапами.
8. Записать результат в
   `docs/research/bambu-auth-transport/AT04-clienthello.md` по общему contract.

## Запрещено

- Копировать закрытый Bambu network plugin, private slicer key/certificate или
  browser cookies.
- Отключать TLS verification или принимать любой сертификат.
- Реализовывать JavaScript/browser challenge engine на C3.
- Подменять успешный oracle ручной установкой token или внешним proxy.

## Критерии приёмки

- [ ] Есть минимальный wire diff, связывающий изменение с oracle result.
- [ ] `PASS` воспроизводится минимум три раза и означает Bambu application JSON.
- [ ] Certificate/hostname validation и credential erasure не ослаблены.
- [ ] Изменение ограничено auth transport и имеет OTA rollback path.
- [ ] Записаны image/RAM deltas и прогноз прохождения существующих C3 gates.
- [ ] При провале зафиксирован отрицательный результат и основание пересмотреть
  ADR; функции/security не удалены и S3 не объявлен фиксом fingerprint без опыта.

## Стартовый промпт

```text
Выполни AT04 из docs/development-tickets/0006-bambu-auth-clienthello-spike.md только если AT02 и выполненный AT03 не дали PASS, а BLOCKED или toolchain/resource incompatibility документированы в канонических artifacts. Используй docs/research/bambu-auth-transport/AT01-baseline.md и существующий oracle, не создавай новый mock framework и не гадай вне зафиксированных ClientHello differences. Реализуй минимальный auth-only adapter поверх проверенного TLS stack, сохрани CA/hostname validation, secret erasure, single-TLS coordination, signed OTA и rollback. Не используй реальные credentials и не выполняй MQTT/canary/soak. Перед device deploy запроси одно разрешение. Запиши verdict, точный wire delta и resource projection в docs/research/bambu-auth-transport/AT04-clienthello.md.
```
