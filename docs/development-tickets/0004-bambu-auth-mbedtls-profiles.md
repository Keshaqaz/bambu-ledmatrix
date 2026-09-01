---
id: AT02
status: proposed
date: 2026-09-01
source: ./0002-bambu-auth-transport-spikes.md
---

# AT02. Проверить штатные профили Mbed TLS

## Цель

Определить, может ли полностью штатный ESP-TLS/Mbed TLS transport получить Bambu
application response после явной настройки поддерживаемых параметров, без форка
TLS stack и без HTTP header guessing.

## Предпосылка

AT01 должен предоставить `docs/research/bambu-auth-transport/AT01-baseline.md`
с oracle command и baseline profile. Если artifact
отсутствует или не различает Cloudflare block и transport error, сначала
остановиться и сообщить этот единственный blocker; не создавать второй harness.

## Фиксированная матрица

Проверить baseline плюс не более четырёх variants. Variants 1–3 меняют по одной
группе параметров; variant 4 является единственным bounded combined profile:

0. baseline: текущий production Mbed TLS profile;
1. явные SNI + hostname validation + ALPN `http/1.1`;
2. TLS 1.2 с session tickets и cipher-suite order из AT01 desktop control;
3. актуальный поддерживаемый ESP-IDF/Mbed TLS default profile, если его проверка
   не требует необратимой миграции проекта;
4. объединение только тех штатных параметров, которые по отдельности изменили
   oracle или приблизили wire profile к control.

Не добавлять случайный перебор User-Agent/headers: desktop control уже доказал,
что application headers не являются достаточным объяснением.

## Работа

- Реализовать profiles только внутри auth probe или узкого `CloudHttpTransport`.
- Для каждого профиля запустить AT01 oracle с синтетическими invalid credentials.
- Записать таблицу и вывод в
  `docs/research/bambu-auth-transport/AT02-mbedtls.md` по общему artifact contract.
- Проверять CA bundle и hostname во всех profiles.
- Зафиксировать image delta, minimum heap и largest block для лучшего кандидата;
  не выполнять полный T05 soak/canary.
- Если профиль проходит, подготовить минимальный integration diff, но не менять
  production default и не выполнять real-account login в этом тикете.

## Критерии приёмки

- [ ] Матрица ограничена перечисленными profiles и имеет verdict на каждый.
- [ ] `PASS` означает Bambu JSON, а не просто успешный TLS handshake.
- [ ] Ни один профиль не отключает certificate/hostname validation.
- [ ] Лучший профиль имеет воспроизводимый result и resource deltas.
- [ ] При полном `BLOCKED` зафиксирован отрицательный результат без workaround.
- [ ] Production build проходит; исходный production transport не переключён.

## Стартовый промпт

```text
Выполни AT02 из docs/development-tickets/0004-bambu-auth-mbedtls-profiles.md на основе docs/research/bambu-auth-transport/AT01-baseline.md и ADR 0001. Используй существующий oracle, не создавай новый harness. Проверь только baseline плюс четыре заданных Mbed TLS/ESP-TLS variants, синтетические invalid credentials и полную CA/hostname validation. Не выполняй real login, MQTT, canary или soak и не меняй production default. Перед единственным device deploy запроси разрешение. Запиши таблицу verdict/resource delta и бинарный вывод в docs/research/bambu-auth-transport/AT02-mbedtls.md.
```
