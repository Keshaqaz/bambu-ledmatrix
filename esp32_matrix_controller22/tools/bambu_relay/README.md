# Bambu Relay

HTTP relay over Bambu MQTT for the ESP32 matrix firmware. Two modes:

- `cloud MQTT` (текущий deployment): сохраняет Bambu Cloud/Handy
- `LAN MQTT`: убирает зависимость от Cloud token, но требует `LAN only` и
  отключает облачные функции принтера

## Files

- `relay.py`: production relay
- `mock_relay.py`: tiny synthetic relay for local/UI testing
- `admin.py`: безопасная проверка и атомарная установка Cloud token
- `test_admin.py`: тесты парсинга, установки, проверки и rollback
- `openwrt/bambu-relay.init`: OpenWrt `procd` service template
- `openwrt/bambu_relay.conf.example`: shell-style config example
- `openwrt/bambu-relay-token-check`: cron-wrapper с transition-based syslog
- `TOKEN_ROTATION_RUNBOOK.md`: рабочая процедура проверки и ротации токена
- `BAMBU_AUTH_ROTATION_RESEARCH.md`: границы поддерживаемой автоматизации и
  сравнение Cloud/LAN вариантов

## Local Run

Cloud MQTT mode needs a Bambu `Access Token` and the printer `serial`
(the account `uid` is resolved automatically via cloud API, or pass `--cloud-uid`):

```bash
python3 -m pip install -r tools/bambu_relay/requirements.txt
read -r -s -p 'Bambu cloud token: ' BAMBU_CLOUD_TOKEN; echo
export BAMBU_CLOUD_TOKEN
python3 tools/bambu_relay/relay.py \
  --printer-serial '<PRINTER_SERIAL>' \
  --listen-host 0.0.0.0 \
  --listen-port 8787
unset BAMBU_CLOUD_TOKEN
```

Не вставляйте токен прямо в аргумент команды: он попадёт в shell history и
может быть виден в списке процессов. На роутере токен хранится только в
`/root/bambu_relay.conf` с правами `0600`.

A different regional broker can be set with `--mqtt-host us.mqtt.bambulab.com`.

LAN MQTT mode (printer in `LAN only`, needs the access code):

```bash
python3 tools/bambu_relay/relay.py \
  --printer-host 192.168.1.50 \
  --printer-serial '<PRINTER_SERIAL>' \
  --printer-access-code '<LAN_ACCESS_CODE>' \
  --listen-host 0.0.0.0 \
  --listen-port 8787
```

## OpenWrt Deploy

Copy these files to the router:

- `relay.py` -> `/root/bambu_relay.py`
- `admin.py` -> `/root/bambu_relay_admin.py`
- `openwrt/bambu-relay.init` -> `/etc/init.d/bambu-relay`
- `openwrt/bambu-relay-token-check` -> `/usr/bin/bambu-relay-token-check`
- `openwrt/bambu_relay.conf.example` -> `/root/bambu_relay.conf`

Then fill `/root/bambu_relay.conf`, enable the service, and start it:

```sh
chmod 755 /root/bambu_relay.py /etc/init.d/bambu-relay /usr/bin/bambu-relay-token-check
chmod 700 /root/bambu_relay_admin.py
chmod 600 /root/bambu_relay.conf   # holds cloud token / access code
/etc/init.d/bambu-relay enable
/etc/init.d/bambu-relay restart
```

The init script warns at startup if the conf is readable by group/others.
The service handles SIGTERM gracefully (clean MQTT disconnect on procd restart).

## Cloud token: проверка и ротация

На текущем роутере установлено:

- `/root/bambu_relay_admin.py` (`0700`)
- `/usr/bin/bambu-relay-token-check` (`0755`)
- cron `17 */6 * * * /usr/bin/bambu-relay-token-check`

Cron валидирует токен каждые 6 часов. Код `2` означает, что подтверждена
необходимость ротации (`401`, `403` или осталось меньше 7 дней); код `3` —
временная ошибка API/сети, при которой заменять токен ещё не следует. В syslog
пишутся только переходы состояния, поэтому постоянная ошибка не создаёт flood.

Получение нового токена остаётся ручным действием через официальный login
Bambu. Поддерживаемого публичного headless refresh API нет; пароль, refresh
token и browser cookies на роутере не сохраняются. После получения access token
или полного login JSON используйте процедуру из
[`TOKEN_ROTATION_RUNBOOK.md`](TOKEN_ROTATION_RUNBOOK.md). Installer сначала
валидирует новый токен, затем атомарно обновляет конфиг, перезапускает relay и
ждёт `source=cloud_mqtt, connected=true`; при неудаче возвращает прежний конфиг.

## HTTP Endpoints

- `GET /status` (also `/`) — full printer state JSON; requires the `X-Relay-Key`
  header when `BAMBU_RELAY_KEY` is set (timing-safe comparison, `401` otherwise)
- `GET /healthz` — `{"ok": true}` without auth and without print data; use for monitoring

```text
http://<router-ip>:8787/status
```

Основные поля, которые использует прошивка:

- `lifecycle`: `unknown`, `idle`, `preparing`, `printing`, `paused`, `finished`,
  `error` или `offline`
- `progress`: проценты `0..100`; значение `1` означает 1%, а не долю единицы
- `connected` и `printerOnline`: состояние relay/MQTT и самого принтера

Relay удерживает `finished` 30 секунд, затем переводит состояние в явный
`idle`. Частичные MQTT-пакеты не очищают уже известное состояние печати.
Прошивка сохраняет последний видимый оверлей при временном `unknown/offline`
или недоступности relay и очищает его только после явного `idle` либо отключения
интеграции.

## Config Options

All options are env variables in `/root/bambu_relay.conf` (or CLI flags, see `--help`):

- `BAMBU_RELAY_KEY` — optional auth key for `/status`
- `BAMBU_RELAY_STALE_SECONDS` — seconds without printer MQTT reports before
  `/status` degrades to `unknown` (default 70); firmware keeps the last visible
  print overlay during this transient state
- After restart, MQTT reconnect to Bambu Cloud takes ~1–2 minutes; until the first
  printer report `/status` shows `connected: true, printerOnline: false`

## Mock Relay

`mock_relay.py` serves a JSON payload from a file and is useful for synthetic
testing of matrix overlays without a live print.

## Quick Checks

Show help without requiring MQTT dependencies to be installed:

```sh
python3 tools/bambu_relay/relay.py --help
```

Syntax-check relay scripts:

```sh
python3 -m py_compile \
  tools/bambu_relay/relay.py \
  tools/bambu_relay/mock_relay.py \
  tools/bambu_relay/admin.py
python3 -m unittest discover -s tools/bambu_relay -p "test_*.py"
sh -n tools/bambu_relay/openwrt/bambu-relay-token-check
```

Текущий набор содержит 15 тестов: семантика MQTT-состояний, защита от очистки
частичным пакетом, переход `finished → idle`, разбор login JSON, атомарная
установка токена, проверка срока и rollback при неуспешном reconnect.
