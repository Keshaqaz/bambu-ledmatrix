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
/etc/init.d/bambu-relay enable
/etc/init.d/bambu-relay restart
```

The default HTTP endpoint is:

```text
http://<router-ip>:8787/status
```

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
