# AT01 — Bambu auth transport baseline

Date: 2026-09-01

Status: complete. Device and desktop oracle results are recorded. Exact desktop
ClientHello metadata is recorded; the unavailable ESP observation point is
documented without an inferred wire diff.

## Oracle command

The isolated probe is
`esp32_matrix_controller22/tools/bambu_auth_transport_probe.py`. It uses a
fixed synthetic `.invalid` account and a fixed synthetic invalid password. It
does not read `esp32_matrix_controller22/.secrets/bambu-test.env`.

Run from `esp32_matrix_controller22`:

```sh
python3 tools/bambu_auth_transport_probe.py desktop --timeout 20
python3 tools/bambu_auth_transport_probe.py esp \
  --url http://192.168.1.65 --timeout 25
```

Each invocation writes exactly one line to stdout:

- `TRANSPORT_PASS` (exit `0`): Bambu application JSON was reached;
- `TRANSPORT_BLOCKED` (exit `2`): bounded classification found the current
  Cloudflare HTML/403 block;
- `TRANSPORT_ERROR` (exit `3`): TLS, DNS, timeout, local HTTP, oversized
  response, or unclassified non-application response failed.

The classifier inspects at most the first 2,048 body bytes. The OpenSSL process
accepts at most 65,536 response bytes. Neither response body nor request body,
credentials, cookies, operation ID, or tokens are printed or persisted.

Before the ESP request, the probe requires an unconfigured `signed_out` state.
It refuses to run if active/candidate credentials or another operation exist.
It may reset a prior `recoverable_error` through `/api/bambu/logout` only after
confirming that no active/candidate credential exists. The accepted operation
ID stays in memory and every non-empty polled operation ID must match it; a
foreign operation produces `TRANSPORT_ERROR`.

## Versions and production build

| Item | Baseline |
|---|---|
| Firmware | `4.3.3-2026.09.01`, live partition `app1` |
| Board / partition | ESP32-C3 / `min_spiffs`, app slot 1,966,080 bytes |
| Arduino CLI | `1.5.1` |
| Arduino ESP32 core | `3.3.8` |
| RISC-V compiler | `riscv32-esp-elf-g++ 14.2.0`, crosstool-NG `esp-14.2.0_20260121` |
| Desktop TLS control | OpenSSL `3.6.4` |

Production build command:

```sh
arduino-cli compile \
  --fqbn esp32:esp32:esp32c3:PartitionScheme=min_spiffs .
```

Result: PASS. The linked sketch is 1,385,857 bytes (70% of the app slot), with
49,780 bytes of globals and 277,900 bytes left for local allocations. No
production firmware source or production build flags were changed by AT01.

Host verification also passed: all C++ seam tests, 10 AT01 Python oracle tests,
22 existing relay Python tests under Python 3.14, Python compilation, shell
syntax checks, and the deterministic WebUI export check.

## Fixed application request

Both targets use `POST /v1/user-service/user/login`, global region, HTTP/1.0,
`Content-Type: application/json`, and the same compact JSON application payload
schema and values. The payload is not recorded here; its non-secret comparison
coordinates are:

- byte length: `109`;
- SHA-256: `802646fbf2d3a2158d60e832d945f90a7f36b74a3262886eb267a25f460c5aa9`;
- keys in wire order: `account`, `password`, `apiError`;
- input class: fixed synthetic `.invalid` account plus fixed synthetic invalid
  password; no real account, verification code, token, or cookie.

The desktop control sends the same production identity header order:

```text
Host
User-Agent
X-BBL-Client-Name
X-BBL-Client-Type
X-BBL-Client-Version
X-BBL-Device-ID
X-BBL-Language
X-BBL-OS-Type
X-BBL-OS-Version
X-BBL-Agent-Version
X-BBL-Executable-info
X-BBL-Agent-OS-Type
X-BBL-Executable-Env
Accept
Content-Type
Content-Length
Connection
```

The desktop probe uses a fixed synthetic UUID-shaped device ID. Production ESP
uses its existing UUID-shaped ID derived from the chip MAC. That header value is
not part of the application payload and is not printed.

## Verdicts

| Target | Result | Bounded evidence |
|---|---|---|
| Desktop OpenSSL control | `TRANSPORT_PASS` | parsed JSON, HTTP 400, observed Bambu top-level coordinates `code` + `error`; values/body discarded |
| Live ESP32-C3 production path | `TRANSPORT_BLOCKED` | public auth state `recoverable_error`, code `cloudflare_blocked`; body discarded |

The live post-run diagnostic confirmed firmware `4.3.3-2026.09.01` on `app1`.
The existing production classifier therefore reproduces the blocker through the
same `BambuCloud::postLogin` / `cloudJsonRequest` path used by WebUI login.

## Classifier contract

| Input | Output |
|---|---|
| HTTP 403 and `cloudflare` in the bounded, case-insensitive prefix | `TRANSPORT_BLOCKED` |
| Parsed JSON, HTTP 400/401, observed Bambu top-level coordinates `code` + `error` | `TRANSPORT_PASS` |
| Parsed HTTP 200 JSON with Bambu auth `data` coordinates (`loginType` or token-envelope key) | `TRANSPORT_PASS` |
| Generic, empty, malformed, proxy-status, or unrecognized JSON | `TRANSPORT_ERROR` |
| ESP public error `cloudflare_blocked` | `TRANSPORT_BLOCKED` |
| ESP public error `invalid_credentials` | `TRANSPORT_PASS` |
| ESP public error `csrf_missing_cookie` | `TRANSPORT_ERROR` |
| ESP verification/fetch/select/connect/ready state | `TRANSPORT_PASS` |
| ESP recoverable transport error, timeout, malformed/oversized response | `TRANSPORT_ERROR` |

The ESP result is accepted only after the clean, credential-free baseline above.
The operation ID returned by the login POST is never printed; any non-empty
status operation ID must equal it. This prevents a concurrent/recent auth
operation from supplying the verdict. A fast failure may clear the ID before the
first status poll; it remains correlated by the immediately preceding clean
`signed_out` baseline and rejection of any observed foreign ID.

The production C++ classifier remains covered by
`tests/bambu_auth_contract_test.cpp`; AT01 adds agent-level coverage in
`tools/test_bambu_auth_transport_probe.py`.

## Wire metadata

No raw capture is stored in the repository. Certificate validation and hostname
validation were enabled for both targets.

The permitted desktop capture used OpenSSL's handshake trace with stdin closed,
so it contained no HTTP request or application body. The ClientHello was:

- record legacy version TLS 1.0 (`0x0301`), ClientHello legacy version TLS 1.2
  (`0x0303`), message length 1,538 bytes;
- cipher IDs, in order:
  `1302,1303,1301,c02c,c030,009f,cca9,cca8,ccaa,c02b,c02f,009e,c024,c028,006b,c023,c027,0067,c00a,c014,0039,c009,c013,0033,009d,009c,003d,003c,0035,002f`;
- extension IDs, in order:
  `65281,0,11,10,35,22,23,13,43,45,51`;
- supported groups, in order:
  `4588,29,23,30,24,25,256,257` (`X25519MLKEM768`, X25519, P-256,
  X448, P-384, P-521, FFDHE2048, FFDHE3072);
- signature algorithm IDs, in order:
  `0905,0906,0904,0403,0503,0603,0807,0808,081a,081b,081c,0809,080a,080b,0804,0805,0806,0401,0501,0601,0303,0301,0302,0402,0502,0602`;
- supported versions: TLS 1.3, TLS 1.2; key shares:
  `X25519MLKEM768`, X25519; EC point formats: uncompressed;
- ALPN extension: absent.

JA3-like coordinates (GREASE filtering was unnecessary because this trace had
no GREASE values):

```text
771,4866-4867-4865-49196-49200-159-52393-52392-52394-49195-49199-158-49188-49192-107-49187-49191-103-49162-49172-57-49161-49171-51-157-156-61-60-53-47,65281-0-11-10-35-22-23-13-43-45-51,4588-29-23-30-24-25-256-257,0
```

MD5: `0b85eb0d4981e69064e40753e4f0ac5f`.

The desktop control negotiated:

- TLS 1.3;
- `TLS_AES_256_GCM_SHA384`;
- group `X25519MLKEM768`;
- peer `CN=*.bambulab.com`, hostname verified;
- certificate verification `OK`.

The live ESP ClientHello could not be captured without changing infrastructure:

- macOS has `tcpdump`, but the current automation process cannot open `/dev/bpf0`
  (`Permission denied`) and cannot observe the matrix's switched unicast traffic
  in any case;
- the correct OpenWrt gateway observation point is reachable and routes Bambu
  traffic through `pppoe-wan`, but its image does not contain `tcpdump`;
- no USB serial device is attached, and the Mac has no `tshark`, `scapy`,
  `dpkt`, or `pyshark`.

Installing a router package or deploying temporary firmware would exceed AT01's
capture-only scope. Therefore no ESP cipher/extension/group/JA3 diff is claimed.
AT02 starts from the exact production stack/profile below and must record its
own tested profile rather than treating the desktop list as an ESP expectation.

## Inputs for AT02 and AT03

AT02 must start from the unchanged production ESP transport:

- `WiFiClientSecure` / ESP-TLS / Mbed TLS supplied by Arduino ESP32 core `3.3.8`;
- CA set through `setCACert`, hostname `api.bambulab.com` supplied to
  `HTTPClient::begin`, 12-second connect/handshake/read timeouts;
- HTTP/1.0, connection reuse disabled;
- endpoint, payload hash/length/schema, header names/order, classifier, and
  oracle commands above;
- baseline verdict `TRANSPORT_BLOCKED`.

AT03, if reached by the short-circuit policy, must reuse the same endpoint,
payload coordinates, headers, timeouts, classifier and commands. It may replace
only the auth-only TLS transport. MQTT, weather, OTA, credential storage,
certificate/hostname validation, and the production source selector remain out
of scope.

## Redaction check

- Only fixed synthetic invalid credentials were used.
- The ignored real-account secrets file was not read.
- stdout contains one verdict line only.
- No request/response body, account, password, token, cookie, operation ID, or
  verification value is present in this artifact or any retained capture.
- The permitted capture used no real credentials despite the broader permission;
  AT01's synthetic-only contract was kept.
- No raw pcap, OpenSSL trace, HTTP response, device response, router helper, or
  TLS key material was retained after metadata extraction.
