# LED Matrix: Working Context

Last updated: 2026-08-28

This file is the operational context for the whole repo (firmware + relay).
User-facing docs: `README.md` (repo map), `esp32_matrix_controller22/README.md`
(firmware), `esp32_matrix_controller22/tools/bambu_relay/README.md` (relay), and
`esp32_matrix_controller22/tools/bambu_relay/TOKEN_ROTATION_RUNBOOK.md` (Cloud
token operations).

## Project

- Firmware project: `esp32_matrix_controller22/`
- Deployed firmware: `4.2.0-2026.08.28`, partition `app1`; OTA from `4.0.11` succeeded on 2026-08-28 and the new session endpoint remained available after the boot-guard window
- Target board: `ESP32C3 Dev Module` / FQBN `esp32:esp32:esp32c3:PartitionScheme=min_spiffs`
- Main firmware: `src/app.cpp`; web UI source: `src/index_html.h`; browser preview export: `web/index.html` (re-export via `tools/export_web_ui.sh`, verify via `tools/check_web_ui_export.sh`)
- Relay: `tools/bambu_relay/relay.py` (deployed on the OpenWrt router)

## Network Layout

- Matrix: `192.168.1.65`
- Router (OpenWrt 25.12.1, relay host): `192.168.1.1`
- Relay endpoint used by the matrix: `http://192.168.1.1:8787/status`; health check: `/healthz` (no auth)
- SSH to the router is password-only (no key installed); deploys need an interactive scp/ssh session
- Wi-Fi reconnect is application-managed because fallback AP mode is part of the
  state machine. After boot failure or a live disconnect, retries use bounded
  backoff `5 → 10 → 20 → 40 → 60 s`, then remain capped at 60 s. Successful STA
  connection resets the backoff. The policy lives in
  `src/wifi_reconnect_policy.hpp` and has a host regression test under `tests/`.

## Build & Flash

- **Local access policy:** [ADR 0002](docs/adr/0002-use-security-level-zero-for-local-access.md) defines security level 0: open AP and WebUI/API without admin password, session or CSRF. Deployed `4.2.0` still contains the superseded T01 onboarding/session gate; remove it in the next firmware change before continuing live T04 onboarding.
- Working toolchain: `arduino-cli compile --fqbn esp32:esp32:esp32c3:PartitionScheme=min_spiffs --export-binaries .` from the project root; flash over USB with `-u -p /dev/cu.usbmodem101`. Network updates retain signed WebUI OTA for image integrity; sign with `sh tools/sign_ota.sh build/.../*.ino.bin`. ArduinoOTA/espota is disabled.
- Libraries installed via `arduino-cli lib install`: `FastLED@3.10.3`, `ArduinoJson@7.4.3`
- PlatformIO is currently broken on this machine: `~/.platformio/penv/bin/platformio` has a missing Python interpreter, while `/Users/kesha/Library/Python/3.9/bin/platformio` runs under unsupported Python 3.9. Use `arduino-cli`; `platformio.ini` is retained for future repair.
- arduino-cli uses esp32 core 3.3.8. **Core 3.x gotcha:** `NetworkClient::setTimeout()` takes milliseconds (core 2.x, which old PlatformIO builds used, took seconds); `setHandshakeTimeout()` still takes seconds. `setTimeout(8)` = 8 ms killed all weather TLS connections until fixed (commit 932e290). Audit client timeouts when porting code written for core 2.x.
- **OTA:** `min_spiffs` scheme, two 1.875MB app slots (binary limit 1 966 080 bytes). Target security-level-0 `/api/update` has no user/session gate but still requires the ECDSA-P256 `X-OTA-Signature` for image integrity before `Update.end()` commits the boot slot. Deployed `4.2.0` still has the superseded session/CSRF gate. ArduinoOTA/espota is disabled. Bump `FW_VERSION` in `src/app.cpp`; verify `fwVersion`/`fwPartition` after OTA.
- **Standing deploy authorization (owner, 2026-09-02):** firmware/device deploys to project-owned LED Matrix hardware are pre-authorized; do not ask again before signed WebUI OTA or necessary diagnostic flashing. Resolve the exact target, use signed images for network OTA, retain rollback/boot-guard, and verify version, partition, and health afterward. This does not authorize deploying to third-party devices, starting prints, mutating accounts, deleting data, or other destructive actions.
- **arduino-cli gotcha:** `compile --output-dir <dir>` also silently overwrites the sketch-local `build/` export — a test build (e.g. a deliberately crashing one) can poison the `build/.../*.ino.bin` you later OTA. Verify `build/` freshness (or recompile with `--export-binaries`) before uploading.
- **Boot-guard anti-brick:** NVS namespace `bootguard` counts boots, cleared after 60 s uptime; 3 consecutive failures → `esp_ota_set_boot_partition()` rollback to the other slot (refuses cleanly if that slot is empty/corrupt). Known tradeoff: 3 manual resets within the first 60 s also trigger rollback.
- ESP32-C3 is single-core: background FreeRTOS tasks and `loop()` time-slice on the same CPU. During OTA `otaInProgress` freezes weatherTask/bambuTask (no TLS handshakes mid flash write) and Wi-Fi power save is switched off.

## Display / UI State

- Single display mode: `Часы + темп` (id `6`); fire/plasma/lava render code deleted (freed ~17 KB static RAM), `normalizeMode()` always returns 6. `mode`/`lavaHue`/`showColon`/`colonBright` stay in `Config` for NVS layout only and are forced by `sanitizeLoadedConfig()`
- Clock colon is gone entirely (drawColon2x2/colonOn deleted, `showColon` forced `false`); `colonBright` is NOT dead — it scales the printer-overlay track brightness, keep it at 90
- Web UI: flat 5-section bottom nav, autosave, open local access per security level 0, secrets via `***` sentinel + `data-clear`, and `file://` preview mocks. Firmware serves deterministic gzip from `src/index_html_gz.h`; after editing `src/index_html.h` run `sh tools/export_web_ui.sh && sh tools/generate_web_ui_gzip.sh`.
- Boot/Wi-Fi/OTA animations: amber ignition ring (`showBoot`), Wi-Fi icon with arcs (`showConnecting`), amber bottom-up progress column + green fill / red cross (`showOtaProgress`/`Done`/`Error`); all draw through `px()` so they follow rotation/flip (snapshot is refreshed in `setup()` before `showBoot`)
- Only font is `F_MICRO` 3x5; font picker, weather icons, blinking colon, and the `Доступ` settings tab are removed
- Brightness UI range `1..255` with a quadratic calibration curve before `FastLED.setBrightness()`
- Legacy `Config` fields (`fontIndex`, `showDegree`, `clockFace`, `webPass`, `apPass`) exist only for NVS layout compatibility and are sanitized/ignored at runtime. Old Bambu credential buffers are read only through the explicit v1 NVS blob and are removed from runtime/current storage.
- **NVS padding gotcha:** new `Config` fields appended into former struct padding don't change `sizeof(Config)`, so old blobs load fine but the new fields read as `0` (not their compiled defaults) — happened with `tzFromWeather`/`weatherProvider` in 4.0.0; fixed by a one-time POST after deploy
- Target onboarding AP is open and WebUI/API have no admin/session/CSRF under ADR 0002. The unique 12-digit AP password and admin provisioning present in deployed `4.2.0` are superseded implementation to remove, not product requirements.
- `showClockTemp()` owns the printer overlay rendering; overlay glyphs per lifecycle:
  - `preparing`: breathing first green column on gray track
  - `printing`: gray track + green progress fill (active column ramps smoothly)
  - `paused` / `error`: blinking yellow / red bar
  - `finished`: full-width breathing green bar
  - explicit `idle`: no overlay
  - transient `unknown` / `offline` or HTTP/Wi-Fi failure: preserve the last visible overlay and progress until relay reports an explicit `idle`
- Temperature layout was checked on 2026-06-15: `+17` at auto-centered X and `ct6TempY=10` stays inside 16x16 for all 16 rotation/flip combinations. `rotation=2 + flipH=true` is equivalent to a vertical frame reflection and does not hide the temperature.
- Display scheduling since 4.0.5: static clock/weather/Bambu-idle state is evaluated once per second and the frame cache skips `FastLED.show()` unless visible data changed. The 250 ms cadence is used only for animated Bambu states (`preparing`, `paused`, `finished`, `error`). The main loop sleeps 50 ms between service passes (20 Hz is sufficient for WebServer and the Wi-Fi connect state machine).
- Thermal regression fixed in 4.0.5: 4.0.4 reached `71.3 C` after 22 minutes; after deploying 4.0.5 the internal sensor reported `49.3 C` at 4 minutes and `51.3 C` at 5 minutes. Do not restore unconditional 4 FPS display checks or sub-20 ms idle polling without measuring CPU temperature.

## Bambu Integration

- T04 direct Cloud shadow path owns email/password auth, email/SMS verification,
  printer selection, refresh/logout and direct MQTT, while
  renderer and primary status remain on `RelayHttpSource` until T05.
- TOTP follows the first-party portal sequence: `/api/sign-in/tfa` establishes
  a bounded, transient session cookie, then `/api/auth/token` returns the full
  `token`, `refreshToken`, `expiresIn`, `refreshExpiresIn` envelope. The cookie
  is reduced to one `name=value` pair and wiped after the second request; an
  incomplete envelope fails closed without changing the active credential.
- `BambuCloud` uses versioned checksum-protected active/candidate credential
  slots. Candidate is promoted only after account/directory checks, MQTT and the
  first valid report; reboot/error removes candidate and preserves active.
- Direct Cloud refresh is autonomous; there is no manual UI/API rotation path.
  It refreshes inside a 7-day window (deferring while printing until the last
  24 hours), bootstraps missing expiry after 5 minutes, retries at
  1m/5m/15m/1h/6h, and reacts immediately to confirmed MQTT auth rejection.
  The newest rotated refresh token and account UID are persisted before candidate
  validation in checksum-protected NVS `refreshNext`; reboot/rollback retries it
  only for the matching account and clears it after successful active promotion.
  Persisting this credential
  is an explicit reliability-over-security decision under security level 0.
- Password and verification code are bounded to one operation and explicitly
  wiped. API/UI/diagnostics expose state/reason codes, never tokens or password.
- `BambuReportParser` consumes MQTT fragments without allocating
  `total_data_len`; unknown/malformed/oversized reports cannot clear the last
  valid direct snapshot.
- `SecureNetworkCoordinator` serializes Cloud HTTP/MQTT, weather HTTPS,
  sunrise and OTA so C3 never owns two TLS sessions. Relay remains the active
  renderer and rollback source.
- `bambuHasRelayConfig()` treats `Relay URL` as the only required renderer source.
- **Progress semantics:** `mc_percent: 1` means 1%, not 100% (`< 1.0` scaling guard, not `<=`); `finished` forces progress to 100. Regression test: `test_progress_one_means_one_percent_while_finished_is_complete`. Do not "fix" this back.
- Relay keeps `finished` briefly (grace period), then auto-demotes to `idle`; partial MQTT telemetry packets are ignored so they cannot wipe live print state
- Relay returns `idle` instead of `unknown` when connected with no active print

## Relay Deployment (router)

- Files on the router: `/root/bambu_relay.py`, `/root/bambu_relay_admin.py`,
  `/etc/init.d/bambu-relay`, `/usr/bin/bambu-relay-token-check`, and
  `/root/bambu_relay.conf` (`0600` — holds Cloud token / access code)
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
- Cloud token rotation (deployed and verified 2026-07-16):
  - `/usr/bin/bambu-relay-token-check` runs from cron at minute 17 every 6 hours;
    state is stored in `/tmp/bambu-relay-token.state`, and syslog records only
    failure/recovery transitions
  - exit `2` means confirmed rotation needed (`401/403`, empty token, or within
    the 7-day warning window); exit `3` is a transient validation failure
  - install only through `/root/bambu_relay_admin.py install-cloud-token` via
    stdin; it validates first, updates the config atomically, waits for connected
    `cloud_mqtt`, and rolls back the exact prior config on failure
  - token acquisition remains official-login mediated: no supported public
    headless refresh contract was found, and no password, refresh token, or
    browser cookie is stored on the router
  - full procedure and security boundary:
    `tools/bambu_relay/TOKEN_ROTATION_RUNBOOK.md`; source research:
    `tools/bambu_relay/BAMBU_AUTH_ROTATION_RESEARCH.md`

## Weather

- Provider selectable via `weatherProvider` (0=auto OWM→Open-Meteo on any failure, 1=OWM only, 2=Open-Meteo only); diagnostics (`weatherCode`, `weatherError`, `weatherSource`, `weatherKeyLen`, `gateway`, `dns0`, `dns1`) exposed in `/api/status`
- Since 4.0.10, OWM and Open-Meteo attempt HTTP first, then HTTPS as a sequential fallback; `fetchWeather()` temporarily disables Wi-Fi power save during the request and restores `WIFI_PS_MIN_MODEM` on exit. Do not add simultaneous HTTPS/HTTP client objects in `fetchWeather()`: the 4.0.2 experiment overflowed the 20 KB weather task stack and caused a boot loop. Known tradeoff: OWM HTTP exposes the API key in transit.
- 4.0.10 OTA was deployed after diagnosing a reboot-time weather miss. `/api/status` confirmed `4.0.10-2026.06.15` on `app1`; gateway/DNS were correct (`192.168.1.1`), OWM resolved to `176.9.63.49`, and final auto + manual refreshes returned OWM `200` with `temp=17.19`.
- Open-Meteo works by coordinates only; in city mode coords are cached into `cfg.owmLat/Lon` from OWM responses
- `tzFromWeather` (default on): UTC offset taken from the weather payload (OWM `timezone`, Open-Meteo `utc_offset_seconds` via `timezone=auto`) → `cfg.tzOffset` + `ntpPending`; UI sends manual `tzOffset` only when the toggle is off
- NTP: `cfg.ntpServer` primary with hardcoded fallbacks `time.google.com` / `time.cloudflare.com` in both `configTime()` call sites
- Sunrise/sunset is fetched only when `autoBrSunMode` is enabled; otherwise `sunTimeValid` stays `false` by design

## Operational Notes

- Last deployed build (2026-08-28): `4.2.0-2026.08.28`, 1,381,447 bytes
  linked image (70%), 49,236 bytes globals (15%); exported OTA image SHA-256
  `42323f84fbcc19dc8c152e2f687291c23cfa529c98c49b9a0b40a4a4d0255da8`.
- Current T04 autonomous-refresh source build `4.2.2-2026.08.31` (not deployed):
  1,389,979 bytes linked image (70%), 49,828 bytes globals (15%); exported OTA
  image is 1,390,128 bytes, SHA-256
  `47c5889250823504f59523c878be7a4bc732faa8b0e3832e3f9985afa5ab9af9`.
- If the matrix shows stale printer state, check process health first with
  `curl http://192.168.1.1:8787/healthz`; `/status` requires the configured
  `X-Relay-Key` and must not be queried by putting that key in shell history
- If relay fails with Cloud authorization errors, run
  `ssh root@192.168.1.1 /root/bambu_relay_admin.py check-cloud-token`; do not
  overwrite the token on exit `3`
- Do not infer a device failure from sandbox/browser timeouts to `192.168.1.65`; local-network access from automation is intermittent. Confirm through the physical matrix or a browser that can reach the LAN.
- After web UI changes always run `sh tools/export_web_ui.sh`; before committing run `sh tools/check_web_ui_export.sh`
- If changing relay logic, update both the local `tools/bambu_relay/relay.py` and the router copy `/root/bambu_relay.py`, then run `python3 tools/bambu_relay/test_relay.py`
- Do not keep router backup snapshots in the repo — git history already has every deployed version; take pre-deploy backups into a temp dir instead
