# ESP32 Matrix Controller: Working Context

Last updated: 2026-06-11

## Project

- Path: `/Users/kesha/Desktop/vibecoding/ledmatrix/esp32_matrix_controller22`
- Target board: `ESP32C3 Dev Module` / `esp32-c3-devkitm-1`
- Main firmware: `src/app.cpp`
- Main web UI source: `src/index_html.h`
- Browser preview export: `web/index.html`
- Web export script: `tools/export_web_ui.sh`
- Relay script: `tools/bambu_relay/relay.py`

## Build Status

- `arduino-cli compile --fqbn esp32:esp32:esp32c3:PartitionScheme=huge_app ...` passes
- `platformio run -d /Users/kesha/Desktop/vibecoding/ledmatrix/esp32_matrix_controller22` passes on 2026-04-12
- Recent Arduino size: `1408041` bytes Flash, `59988` bytes RAM
- Current PlatformIO size with `partitions_huge_app.csv`: `1084514 / 3145728` Flash, `61748 / 327680` RAM
- Project is ready for Arduino IDE from the folder root
- PlatformIO now explicitly uses `board_build.partitions = partitions_huge_app.csv` to match the Huge APP / no-OTA Arduino IDE recommendation
- `partitions_huge_app.csv` stores the project-local 3MB app + ~1MB SPIFFS partition table
- OTA updates are intentionally not supported; firmware updates are expected through USB/serial flashing from Arduino IDE or PlatformIO
- ESP32-C3 is single-core: background FreeRTOS tasks and `loop()` time-slice on the same CPU; comments should refer to tasks, not core isolation

## Local Build Tooling

- PlatformIO installed for this user with:
  - `python3 -m pip install --user platformio`
- PlatformIO executable path on this machine:
  - `/Users/kesha/Library/Python/3.9/bin/platformio`
- This directory is not currently on shell `PATH`; either call the full path or add it to PATH.
- Rosetta 2 was installed because PlatformIO `toolchain-riscv32-esp@8.4.0+2021r2-patch5` is x86_64 on this setup and failed on arm64 with `Bad CPU type in executable`.

## Current UI State

- UI stays in the compact Russian design, not the later full redesign
- `Матрица` mode is removed
- Separate `Температура`, `Часы`, and `Тест погоды` modes are removed
- Remaining display modes are `Часы + темп` (`6`), `Огонь` (`2`), `Плазма` (`4`), and `Лава` (`5`)
- Header shows Wi-Fi, IP, outside temperature, and CPU temperature with gear emoji
- Bambu section is relay-only: ESP stores only `Relay URL` and optional `Relay Key`
- Direct ESP-side Bambu Cloud auth fields were removed from the UI
- `Доступ` settings tab is removed; web auth stays disabled and AP password is fixed to `12345678`
- Brightness UI range is now `1..255`; firmware applies a quadratic calibration curve before `FastLED.setBrightness()` so low values are less harsh
- Clock separator has only show/hide control now; the removed blinking option is ignored and the separator is steady when enabled
- Weather icon visibility setting is removed; in `Часы + темп` the icon is drawn automatically only when it fits beside the temperature text
- Standalone preview is exported into `web/index.html` after UI changes

## Fonts

- Font selection is removed.
- Firmware keeps only the `F_MICRO` 3x5 digit/sign font used by `Часы + темп`.
- Latest requested `3x5` fixes in `F_MICRO`:
  - `6`: top-right pixel lit
  - `7`: middle-left diagonal pixel removed
  - `9`: bottom-left pixel lit

## Clock / Overlay Layout

- `Часы + темп` is mode `6`
- Clock separator between hours and minutes is `2x2` dots
- Central printer overlay lives on rows `7-8`
- During `printing`, there is a gray full-width track plus green progress fill
- Progress fill now lights smoothly:
  - completed columns are solid green
  - current active column ramps from dim green to full green
- During `preparing`, only the first green column breathes on top of the gray track
  - `bambuOverlayLifecycle()` must preserve `BAMBU_PREPARING`; mapping it to `BAMBU_PRINTING` makes this branch unreachable
- During `finished`, the whole green bar breathes across the full width
- During `paused`, a blinking yellow `2x2` square is shown
- During `error`, a blinking red `2x2` square is shown

## Bambu Integration: Final Direction

- Direct ESP-side Bambu Cloud login/tasks/bindings code was removed as obsolete
- Current runtime path is:
  - printer/cloud MQTT -> relay -> ESP relay polling
- Relay is the only source of truth for printer status in firmware
- Firmware no longer falls back to Bambu Cloud REST polling
- Legacy Bambu email/password/token/serial fields are kept only for NVS layout compatibility and are cleared on load/save

## Relay Deployment

- Working relay host is the OpenWrt router
- Relay endpoint:
  - `http://<relay-host>:8787/status`
- Router details:
  - `OpenWrt 25.12.1`
  - enough overlay space for Python relay
  - installed packages:
    - `python3`
    - `python3-paho-mqtt`
- Relay script copied to router:
  - `/root/bambu_relay.py`
- Relay service script on router:
  - `/etc/init.d/bambu-relay`
- Relay runtime config on router:
  - `/root/bambu_relay.conf`
- Relay service is enabled on the router and should be managed through init/procd
- Production relay should not be started manually anymore; use:
  - `/etc/init.d/bambu-relay restart`
- Local OpenWrt deployment templates are stored in:
  - `tools/bambu_relay/openwrt/`

## Relay Behavior

- Relay uses `cloud MQTT` by default; `LAN MQTT` exists only as a fallback mode in the relay tool
- Router relay authenticates with its stored relay-side credentials and subscribes to printer MQTT
- Relay returns JSON with:
  - `connected`
  - `printerOnline`
  - `lifecycle`
  - `progress`
  - `status`
  - `stage`
  - `jobName`
  - `currentLayer`
  - `totalLayers`
  - `remainingSeconds`
- Relay was fixed to ignore partial MQTT telemetry packets that would otherwise wipe live print state
- Relay now has a grace-period rule for `finished`:
  - it keeps `finished` briefly
  - then auto-demotes to `idle`
- Relay hardening (2026-06-11):
  - `X-Relay-Key` is compared with `hmac.compare_digest` (timing-safe)
  - `/healthz` answers `{"ok": true}` without auth and without print data
  - SIGTERM handler shuts the HTTP server down gracefully so MQTT disconnects cleanly under procd restart
  - keepalive thread survives publish exceptions
  - stale TTL is configurable via `BAMBU_RELAY_STALE_SECONDS` (default 70)
  - init script warns when `/root/bambu_relay.conf` is readable by group/others

## Current Working Point

- Matrix uses relay mode with:
  - `http://<relay-host>:8787/status`
- Verified live and synthetic states after the latest fixes:
  - `idle`
  - `printing`
  - `finished`
  - `paused`
  - `error`
- Overlay behavior currently verified:
  - progress bar does not look fully filled before `100%`
  - `finished` blinks as a full green bar
  - `paused` blinks as a full yellow bar
  - `error` blinks as a full red bar
- Relay also now returns `idle` instead of `unknown` when connected but no active print state exists

## Known Live Status Mapping

- `preparing`
  - breathing first green column on gray track
- `printing`
  - gray track + green fill by progress
- `paused`
  - blinking yellow `2x2`
- `error`
  - blinking red `2x2`
- `finished`
  - full-width breathing green bar
- `idle`
  - no printer overlay, normal `Часы + темп`
- `unknown`
  - treated like no overlay
- `offline`
  - no dedicated glyph, effectively no overlay

## Important Code Decisions

- OTA was deliberately dropped in favor of a single large app partition; do not preserve OTA slots in future partition/build changes unless this decision changes.
- `bambuOverlayLifecycle()` still keeps special display-state behavior
- Bambu firmware integration is relay-only:
  - `bambuHasRelayConfig()` treats `Relay URL` as the only required printer source
  - legacy direct-cloud credentials are ignored and cleared during load/save
- `showClockTemp()` contains the active printer overlay rendering logic
- `bambuFetchRelayStatus()` is the key firmware-side relay ingestion function

## ADR 2026-04-12: Prune display surface

- Context: user requested removing weather test, standalone temperature/clock modes, extra fonts, font picker, and the access settings section.
- Decision:
  - Keep the existing numeric mode ids for surviving modes (`2`, `4`, `5`, `6`) instead of renumbering them.
  - Migrate removed mode ids (`0`, `1`, `7`) to `6` (`Часы + темп`); old removed `3` remains migrated to `4` (`Плазма`).
  - Keep legacy `Config` fields such as `fontIndex`, `showDegree`, `clockFace`, `webPass`, and `apPass` only for NVS layout compatibility, but sanitize/ignore them at runtime.
  - Keep only `F_MICRO` because it is the actual `Часы + темп` font and fits the 16x16 layout.
  - Calibrate brightness with a quadratic UI-to-hardware curve and lower UI minimum to `1` rather than only changing the slider min.
  - Remove the weather-icon toggle and blinking-colon toggle from UI/API; keep legacy config fields only for NVS compatibility.
- Consequences:
  - Existing saved configs using removed modes land on the main `Часы + темп` display.
  - Extra fonts no longer consume flash/code or appear in UI.
  - Low brightness has finer control, but mid-range UI values render dimmer than the old linear FastLED scale.
  - The clock separator no longer blinks; when `Показывать разделитель` is enabled it stays visible.
  - Weather icons were removed from matrix rendering entirely; the weather feature now supplies only the numeric outdoor temperature.

## Operational Notes

- If the router relay is healthy, ESP should use:
  - `Bambu = on`
  - `Relay URL = http://<relay-host>:8787/status`
- If matrix still shows stale printer state, check relay JSON first:
  - `curl http://<relay-host>:8787/status`
- If changing web UI, always re-export `web/index.html`
- Before committing UI changes, run `sh tools/check_web_ui_export.sh`
- If changing relay logic, update both:
  - local project copy `tools/bambu_relay/relay.py`
  - router copy `/root/bambu_relay.py`
- Router IP is `192.168.1.1`, matrix is `192.168.1.65`; relay URL in matrix settings is `http://192.168.1.1:8787/status`
- SSH to the router is password-only (no key installed); deploys need an interactive scp/ssh session
- As of 2026-06-11 the router copy of relay.py predates `safe_int`/`stage_from_lifecycle`/auth hardening and must be redeployed

## Good Next Steps

- If printer overlay visuals need tuning, work only in `showClockTemp()`
- If status timing needs tuning, work in router relay state logic first
- If font tuning continues, remember `F_MICRO 3x5` is now the only firmware font

## Fixes Applied 2026-04-12

- Added repository `.gitignore` for PlatformIO artifacts, macOS files, and local assistant state.
- Added `tools/check_web_ui_export.sh` to detect stale `web/index.html`.
- Aligned PlatformIO with Huge APP layout via `board_build.partitions = partitions_huge_app.csv`.
- Fixed Bambu overlay lifecycle so `BAMBU_PREPARING` remains distinct from `BAMBU_PRINTING`.
- Updated ESP32-C3 comments to describe loop/background FreeRTOS tasks instead of dual-core isolation.
- Made `tools/bambu_relay/relay.py --help` work without importing `paho-mqtt` first and hardened numeric MQTT field parsing.
- Added `tools/bambu_relay/test_relay.py` for host-side relay lifecycle and partial-packet regression checks.
- Removed obsolete ESP-side direct Bambu Cloud UI/runtime code; firmware now polls only relay.
- Removed obsolete display modes (`Температура`, `Часы`, `Тест погоды`) and extra font families; migrated old removed modes to `Часы + темп`.
- Removed the UI access section; AP password remains fixed at `12345678`.
- Calibrated brightness to a softer 1..255 UI scale with better low-end control.
- Removed UI/API controls for the weather icon toggle and blinking clock separator; separator is steady, weather icons are not rendered on the matrix.
