# Bambu Relay

HTTP relay over Bambu MQTT for the ESP32 matrix firmware.

## Files

- `relay.py`: production relay
- `mock_relay.py`: tiny synthetic relay for local/UI testing
- `openwrt/bambu-relay.init`: OpenWrt `procd` service template
- `openwrt/bambu_relay.conf.example`: shell-style config example

## OpenWrt Deploy

Copy these files to the router:

- `relay.py` -> `/root/bambu_relay.py`
- `openwrt/bambu-relay.init` -> `/etc/init.d/bambu-relay`
- `openwrt/bambu_relay.conf.example` -> `/root/bambu_relay.conf`

Then fill `/root/bambu_relay.conf`, enable the service, and start it:

```sh
chmod 755 /root/bambu_relay.py /etc/init.d/bambu-relay
chmod 600 /root/bambu_relay.conf   # holds cloud token / access code
/etc/init.d/bambu-relay enable
/etc/init.d/bambu-relay restart
```

The init script warns at startup if the conf is readable by group/others.
The service handles SIGTERM gracefully (clean MQTT disconnect on procd restart).

## HTTP Endpoints

- `GET /status` (also `/`) — full printer state JSON; requires the `X-Relay-Key`
  header when `BAMBU_RELAY_KEY` is set (timing-safe comparison, `401` otherwise)
- `GET /healthz` — `{"ok": true}` without auth and without print data; use for monitoring

```text
http://<router-ip>:8787/status
```

## Config Options

All options are env variables in `/root/bambu_relay.conf` (or CLI flags, see `--help`):

- `BAMBU_RELAY_KEY` — optional auth key for `/status`
- `BAMBU_RELAY_STALE_SECONDS` — seconds without printer MQTT reports before
  `/status` degrades to `unknown` (default 70)
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
python3 -m py_compile tools/bambu_relay/relay.py tools/bambu_relay/mock_relay.py
python3 -m unittest discover -s tools/bambu_relay -p "test_*.py"
```
