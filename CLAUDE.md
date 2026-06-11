# LED Matrix: Working Context

Last updated: 2026-06-11

This file is the operational context for the whole repo (firmware + relay).
User-facing docs: `README.md` (repo map), `esp32_matrix_controller22/README.md`
(firmware), `esp32_matrix_controller22/tools/bambu_relay/README.md` (relay).

## Project

- Firmware project: `esp32_matrix_controller22/`
- Target board: `ESP32C3 Dev Module` / FQBN `esp32:esp32:esp32c3:PartitionScheme=min_spiffs`
- Main firmware: `src/app.cpp`; web UI source: `src/index_html.h`; browser preview export: `web/index.html` (re-export via `tools/export_web_ui.sh`, verify via `tools/check_web_ui_export.sh`)
- Relay: `tools/bambu_relay/relay.py` (deployed on the OpenWrt router)

## Network Layout

- Matrix: `192.168.1.65`
- Router (OpenWrt 25.12.1, relay host): `192.168.1.1`
- Relay endpoint used by the matrix: `http://192.168.1.1:8787/status`; health check: `/healthz` (no auth)
- SSH to the router is password-only (no key installed); deploys need an interactive scp/ssh session

## Build & Flash

- Working toolchain: `arduino-cli compile --fqbn esp32:esp32:esp32c3:PartitionScheme=min_spiffs --export-binaries .` from the project root; flash over USB with `-u -p /dev/cu.usbmodem101`. Network flash: `espota.py -i 192.168.1.65 -p 3232 -f build/.../*.ino.bin` directly (the `arduino-cli -l network` route needs mDNS discovery, which is disabled on the device)
- Libraries installed via `arduino-cli lib install`: `FastLED@3.10.3`, `ArduinoJson@7.4.3`
- PlatformIO is currently broken on this machine: it requires Python ≥3.10, only system 3.9.6 is available. `platformio.ini` is kept for when that is fixed.
- arduino-cli uses esp32 core 3.3.8. **Core 3.x gotcha:** `NetworkClient::setTimeout()` takes milliseconds (core 2.x, which old PlatformIO builds used, took seconds); `setHandshakeTimeout()` still takes seconds. `setTimeout(8)` = 8 ms killed all weather TLS connections until fixed (commit 932e290). Audit client timeouts when porting code written for core 2.x.
- **OTA (since 2026-06-11):** `min_spiffs` scheme, two 1.875MB app slots (binary limit 1 966 080 bytes, currently ~68%). The stock `default` scheme is forbidden — its 1.25MB slots don't fit the binary (that's why the original OTA attempt failed). Two upload paths: web UI `/api/update` (multipart, no auth, rejects non-app images via `esp_app_desc_t` magic — catches `merged.bin`) and ArduinoOTA/espota on port 3232 (no password, mDNS off). Bump `FW_VERSION` in `src/app.cpp` per iteration; verify via `/api/status` `fwVersion`/`fwPartition` (slot flips app0↔app1 on every successful OTA).
- **arduino-cli gotcha:** `compile --output-dir <dir>` also silently overwrites the sketch-local `build/` export — a test build (e.g. a deliberately crashing one) can poison the `build/.../*.ino.bin` you later OTA. Verify `build/` freshness (or recompile with `--export-binaries`) before uploading.
- **Boot-guard anti-brick:** NVS namespace `bootguard` counts boots, cleared after 60 s uptime; 3 consecutive failures → `esp_ota_set_boot_partition()` rollback to the other slot (refuses cleanly if that slot is empty/corrupt). Known tradeoff: 3 manual resets within the first 60 s also trigger rollback.
- ESP32-C3 is single-core: background FreeRTOS tasks and `loop()` time-slice on the same CPU. During OTA `otaInProgress` freezes weatherTask/bambuTask (no TLS handshakes mid flash write) and Wi-Fi power save is switched off.

## Display / UI State

- Single display mode: `Часы + темп` (id `6`); fire/plasma/lava render code deleted (freed ~17 KB static RAM), `normalizeMode()` always returns 6. `mode`/`lavaHue`/`showColon`/`colonBright` stay in `Config` for NVS layout only and are forced by `sanitizeLoadedConfig()`
- Clock colon is gone entirely (drawColon2x2/colonOn deleted, `showColon` forced `false`); `colonBright` is NOT dead — it scales the printer-overlay track brightness, keep it at 90
- Web UI (since 4.0.0): flat 5-section bottom nav (Экран/Сеть/Погода/Принтер/Система), autosave, secrets via `***` sentinel + `data-clear`; preview mode via `file://` mocks. After editing `src/index_html.h` run `sh tools/export_web_ui.sh`
- Boot/Wi-Fi/OTA animations: amber ignition ring (`showBoot`), Wi-Fi icon with arcs (`showConnecting`), amber bottom-up progress column + green fill / red cross (`showOtaProgress`/`Done`/`Error`); all draw through `px()` so they follow rotation/flip (snapshot is refreshed in `setup()` before `showBoot`)
- Only font is `F_MICRO` 3x5; font picker, weather icons, blinking colon, and the `Доступ` settings tab are removed
- Brightness UI range `1..255` with a quadratic calibration curve before `FastLED.setBrightness()`
- Legacy `Config` fields (`fontIndex`, `showDegree`, `clockFace`, `webPass`, `apPass`, direct Bambu credentials) exist only for NVS layout compatibility and are sanitized/ignored at runtime
- **NVS padding gotcha:** new `Config` fields appended into former struct padding don't change `sizeof(Config)`, so old blobs load fine but the new fields read as `0` (not their compiled defaults) — happened with `tzFromWeather`/`weatherProvider` in 4.0.0; fixed by a one-time POST after deploy
- AP password is fixed to `12345678`; web auth stays disabled
- `showClockTemp()` owns the printer overlay rendering; overlay glyphs per lifecycle:
  - `preparing`: breathing first green column on gray track
  - `printing`: gray track + green progress fill (active column ramps smoothly)
  - `paused` / `error`: blinking yellow / red bar
  - `finished`: full-width breathing green bar
  - `idle` / `unknown` / `offline`: no overlay

## Bambu Integration

- Relay-only: printer/cloud MQTT → relay on the router → ESP polls relay over HTTP. No ESP-side Bambu Cloud auth, no REST fallback.
- `bambuFetchRelayStatus()` is the firmware-side ingestion point; `bambuHasRelayConfig()` treats `Relay URL` as the only required source
- **Progress semantics:** `mc_percent: 1` means 1%, not 100% (`< 1.0` scaling guard, not `<=`); `finished` forces progress to 100. Regression test: `test_progress_one_means_one_percent_while_finished_is_complete`. Do not "fix" this back.
- Relay keeps `finished` briefly (grace period), then auto-demotes to `idle`; partial MQTT telemetry packets are ignored so they cannot wipe live print state
- Relay returns `idle` instead of `unknown` when connected with no active print

## Relay Deployment (router)

- Files on the router: `/root/bambu_relay.py`, `/etc/init.d/bambu-relay`, `/root/bambu_relay.conf` (chmod 600 — holds cloud token / access code)
- Manage only via procd: `/etc/init.d/bambu-relay restart`
- Deploy procedure: take a temporary backup of the router copy, `scp -O` the new `relay.py` and init template, `chmod 600` the conf, restart, then verify `/healthz`, `/status` (MQTT reconnect takes ~1–2 min) and the matrix `/api/status` `bambu*` fields
- Relay hardening (2026-06-11, deployed and verified live):
  - `X-Relay-Key` compared with `hmac.compare_digest`
  - `/healthz` answers `{"ok": true}` without auth and without print data
  - SIGTERM handler shuts the HTTP server down gracefully under procd restart
  - keepalive thread survives publish exceptions; malformed MQTT payloads are logged
  - stale TTL configurable via `BAMBU_RELAY_STALE_SECONDS` (default 70)
  - init script warns when the conf is readable by group/others
- Relay uses cloud MQTT by default; LAN MQTT exists only as a fallback mode

## Weather

- Provider selectable via `weatherProvider` (0=auto OWM→Open-Meteo on any failure, 1=OWM only, 2=Open-Meteo only); diagnostics (`weatherCode`, `weatherError`, `weatherSource`, `weatherKeyLen`) exposed in `/api/status`
- Open-Meteo works by coordinates only; in city mode coords are cached into `cfg.owmLat/Lon` from OWM responses
- `tzFromWeather` (default on): UTC offset taken from the weather payload (OWM `timezone`, Open-Meteo `utc_offset_seconds` via `timezone=auto`) → `cfg.tzOffset` + `ntpPending`; UI sends manual `tzOffset` only when the toggle is off
- NTP: `cfg.ntpServer` primary with hardcoded fallbacks `time.google.com` / `time.cloudflare.com` in both `configTime()` call sites
- Sunrise/sunset is fetched only when `autoBrSunMode` is enabled; otherwise `sunTimeValid` stays `false` by design

## Operational Notes

- If the matrix shows stale printer state, check relay JSON first: `curl http://192.168.1.1:8787/status`
- After web UI changes always run `sh tools/export_web_ui.sh`; before committing run `sh tools/check_web_ui_export.sh`
- If changing relay logic, update both the local `tools/bambu_relay/relay.py` and the router copy `/root/bambu_relay.py`, then run `python3 tools/bambu_relay/test_relay.py`
- Do not keep router backup snapshots in the repo — git history already has every deployed version; take pre-deploy backups into a temp dir instead
