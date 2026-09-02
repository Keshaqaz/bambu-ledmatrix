# Bambu Cloud authentication for a Google-created account

Research date: 2026-07-16
Original scope: how to bootstrap a Bambu Cloud token envelope for an account
created with **"Sign in with Google"**. The 2026-08-28 T03 amendment below
narrows the device decision to the browser callback contract and the accepted
Bambu-password prerequisite. Older relay-oriented alternatives remain only
as historical context.

> Extraction and citation only. No daemon code, no design. Mirrors the
> citation discipline of `BAMBU_REFRESH_CONTRACT_RESEARCH.md`. Every URL, file
> path, and wire claim carries a repo / file / commit / URL citation. Where a
> claim cannot be pinned from source, it is stated as a **Gap** rather than
> fabricated.

## T03 feasibility decision — browser callback to device WebUI

Spike date: 2026-08-28

**Binary verdict for the revised ADR flow: FEASIBLE.** The original
Google/browser callback requirement is **BLOCKED**, but it is no longer a
requirement: the accepted device flow uses Bambu email/password login after
the account owner sets a Bambu password once through the official `Forgot
password` page. The user completed that setup on 2026-08-28 for an existing
Google-created account and then successfully logged in with email + password.
No password, token, cookie, or other secret was requested or recorded.

The official Google/browser flow can issue a short-lived ticket, but its
supported desktop callback cannot target the ESP32 WebUI. The current
first-party portal accepts the Studio/Suite callback
destination only when it exactly matches `^http://localhost:\d+$`; current
Bambu Studio supplies `http://localhost:13618` and binds the receiver to
`127.0.0.1:13618`. In a browser opened from the device WebUI, `localhost`
means the user's computer or phone, not the ESP32. A callback that actually
reaches the ESP32 (for example `http://192.168.4.1/...` or
`http://ledmatrix.local/...`) fails the official gate.

The remaining generic browser route was closed by a real authenticated
observation on 2026-08-28. The user opened the first-party sign-in page with
`to=http://192.168.1.65/` (the live matrix WebUI), completed Google sign-in,
and the browser remained on a Bambu page instead of navigating to the matrix.
The deployed auth context calls `/api/checkHost` before following `to` and
returns to `/` when `allow` is false, so this observation confirms that the
local device origin is not accepted by that alternative path. The probe URL
contained no ticket, token, cookie, password, or callback secret; none was
copied or stored by the spike.

This supersedes the older Path 1A uncertainty below. Ticket issuance is no
longer the unknown; the load-bearing blocker is the callback destination.
Path 1B remains usable for the existing relay's one-off operator procedure,
but it is a companion/desktop flow and therefore is **not** success for ADR
0001 T03.

### Confirmed browser-ticket/callback contract

Observed from the content-addressed production bundles served by
`https://bambulab.com/en/sign-in` on 2026-08-28:

1. The social-login page opens
   `/api/v1/user-service/user/tplogin?source=google&...&callback=...`.
2. After sign-in, the first-party page obtains the one-time ticket with
   `GET /api/v1/user-service/user/ticket`.
3. Before following a generic `to`, the auth context posts it to
   `/api/checkHost`; the authenticated `to=http://192.168.1.65/` observation
   was rejected and stayed on Bambu.
4. `/sign-in/callback` enters the desktop delivery branch only for
   `from=studio|suite`, `openBy=studio|suite`, and a `redirect_url` matching
   `^http://localhost:\d+$`.
5. With `slicerLoginType=ticket`, it navigates to that loopback base with
   `ticket=<opaque ticket>` and a first-party `redirect_url` in the query.
   The desktop listener consumes the ticket and redirects the browser to the
   first-party Studio callback page with only `result=success|fail`.
6. Cancel goes to the first-party callback-cancel/error path; no ticket is
   delivered.

The relevant first-party runtime artifacts are the
[`_app` bundle](https://bambulab.com/_next/static/chunks/pages/_app-3f10e8568bed811e.js)
(SHA-256 `8b32cffbbaa6b48dfbdbd328ae71777a10f802f56629ecd7e0c290f37d801f75`;
modules `22132`, `71792`, `89902`) and the
[`/sign-in/callback` bundle](https://bambulab.com/_next/static/chunks/pages/sign-in/callback-287fb981aafe67f8.js)
(SHA-256 `122c8bfad38e8deab7a98c625ad6e81281347631915cff48dbd9bd9428bcd323`).
The latter contains the exact localhost regular expression and the ticket
query construction. These are direct observations of deployed first-party
code, not a claim of a supported public API.

### T04 follow-up: TOTP produces a complete credential envelope

The same pinned first-party `_app` bundle closes the TOTP/refresh gap without
changing the T03 decision. Its TFA form posts `tfaKey` and `tfaCode` to
`/api/sign-in/tfa`; after HTTP 200 the shared signed-in handler does not treat
that response or its cookie as an access token. In the slicer bridge branch it
instead performs `GET /api/auth/token` with the newly established portal
session and reads `token`, `refreshToken`, `expiresIn` and
`refreshExpiresIn`. Those four values are then passed to the native client as
`token`, `refresh_token`, `expires_in` and `refresh_expires_in`.

T04 therefore follows this exact two-request sequence. The session cookie is
transient transport state, not a stored credential: firmware keeps only one
bounded `name=value` pair, allows the first TLS request to finish before the
second begins, and wipes the cookie immediately after `/api/auth/token`.
Missing cookie or either token fails closed before the active/candidate
transaction is started. A live TOTP-enabled account/device remains a final
integration check, not an unknown wire-contract dependency.

The current official Bambu Studio source independently fixes both sides of
the desktop contract at commit
[`926a719`](https://github.com/bambulab/BambuStudio/commit/926a7192574bcb9b3a732e1ec59a46d79cb45466):

- [`HttpServer.hpp`](https://github.com/bambulab/BambuStudio/blob/926a7192574bcb9b3a732e1ec59a46d79cb45466/src/slic3r/GUI/HttpServer.hpp#L19-L20)
  defines port `13618` and base `http://localhost:`;
- [`HttpServer.cpp`](https://github.com/bambulab/BambuStudio/blob/926a7192574bcb9b3a732e1ec59a46d79cb45466/src/slic3r/GUI/HttpServer.cpp#L187-L197)
  binds only `127.0.0.1`;
- [`WebUserLoginDialog.cpp`](https://github.com/bambulab/BambuStudio/blob/926a7192574bcb9b3a732e1ec59a46d79cb45466/src/slic3r/GUI/WebUserLoginDialog.cpp#L301-L321)
  starts that listener, returns its base URL to the portal, and launches the
  third-party flow in the default browser;
- [`HttpServer.cpp`](https://github.com/bambulab/BambuStudio/blob/926a7192574bcb9b3a732e1ec59a46d79cb45466/src/slic3r/GUI/HttpServer.cpp#L268-L298)
  accepts `ticket` plus `redirect_url`, and
  [`handle_login`](https://github.com/bambulab/BambuStudio/blob/926a7192574bcb9b3a732e1ec59a46d79cb45466/src/slic3r/GUI/HttpServer.cpp#L349-L379)
  exchanges the ticket locally before reporting success/failure.

### Minimal reproducible prototype

[`t03_browser_callback_contract_spike.py`](./t03_browser_callback_contract_spike.py)
is a no-network, no-secret executable model of the load-bearing predicate.
It shows that the set of URLs accepted by the official gate and the set of
URLs reaching a network ESP32 do not intersect:

```sh
cd esp32_matrix_controller22
python3 tools/bambu_relay/t03_browser_callback_contract_spike.py --matrix
```

Because the flow is rejected before the device receives any operation or
ticket, T03 does not expand into exchange/error implementation. In
particular, expired, reused, and wrong-operation handling cannot make the
unreachable callback feasible. A companion listener on the browser host,
manual token transfer, or permanent external callback would bridge the gap,
but all three are explicitly disallowed by the ticket and ADR.

**Required consequence:** T04 implements only email/password plus
verification/TOTP through the device WebUI. For a Google-created account,
onboarding points the owner to the official Bambu password-reset page first.
It does not implement or emulate the blocked OAuth callback.

## Relay-oriented TL;DR (historical scope)

For the existing external relay only, the original recommendation was Path
1B: drive Bambu Studio's ticket exchange once and capture its token envelope.
It works at the wire level and returns `refreshToken`, but the T03 decision
above supersedes it for device onboarding because it requires a desktop
loopback receiver.

The ticket-exchange endpoint (`POST /v1/user-service/user/ticket/<T>`,
documented in `BAMBU_REFRESH_CONTRACT_RESEARCH.md` §"Path A") returns the full
four-field envelope for *any* account that can complete a browser sign-in on
`bambulab.com`, including Google-OAuth accounts — Bambu Studio itself uses
exactly this path for Google sign-ins. The hard part is intercepting the
short-lived ticket that `bambulab.com` emits after the Google round-trip,
because that ticket is delivered via a `postMessage` to an embedded webview
(or an HTTP redirect to a Studio-local loopback server), not via a URL you
can copy-paste from a normal browser.

| # | Path | Works for Google-OAuth? | Returns `refreshToken`? | Verdict |
|---|------|-------------------------|-------------------------|---------|
| 1A | Browser ticket delivery to device WebUI | Issuer visible; device `to` rejected | ✅ (if delivered) | **OAuth callback blocked** |
| 1B | Bambu Studio ticket-exchange with loopback interception | ✅ | ✅ | Relay-only; not T03 success |
| 2  | Read persisted tokens from Bambu Studio's macOS data dir | ✅ (Studio is logged in) | ⚠️ Access token confirmed on disk; **refreshToken persistence not pinned from open source** | Secondary / verify-locally |
| 3  | Home Assistant `bambu_lab` integration | ❌ No Google-OAuth login flow | ❌ Does not persist `refreshToken` | **Dead end** |
| 4  | Set a Bambu password on the Google-created account, then use email/password login | ✅ Confirmed for the target account | ✅ | **Accepted device path** |

**Is `refreshToken` obtainable for a Google-auth account at all?** **Yes.**
The ticket-exchange response (path 1B) is the *same* response shape regardless
of whether the underlying `bambulab.com` sign-in used Google OAuth or email +
password — confirmed in Bambu Studio source
([`src/slic3r/GUI/HttpServer.cpp`](https://github.com/bambulab/BambuStudio/blob/master/src/slic3r/GUI/HttpServer.cpp)
`TokenResp` struct and `do_request_login_info()`, which parses both
`accessToken` and `refreshToken` out of the ticket-exchange body). So
autonomous refresh *can* be bootstrapped for this account — the open question
is purely operational (how to capture the ticket once), not whether the
refresh token exists.

**T03 uncertainty resolved:** the issuer exists, but both observed delivery
surfaces are closed to a network ESP32: the Studio/Suite path is exact
localhost-only and the authenticated generic `to` probe rejected the live
matrix origin.

---

## Background — what "Google-OAuth account" means here

A Bambu account created via "Sign in with Google" initially has **no Bambu
password**. The email/password login endpoint
(`POST /v1/user-service/user/login` with body
`{"account":"<email>","password":"<password>","apiError":""}`, documented in
`BAMBU_REFRESH_CONTRACT_RESEARCH.md` §"Path B") cannot be used before a
Bambu password exists.

After a Bambu password is set through the official `Forgot password` page,
the same account can use the ordinary email/password flow; this was observed
successfully for the target account on 2026-08-28. Without that preparation,
the available browser path is the **ticket-exchange flow** that Bambu Studio
itself uses, where the actual Google OAuth round-trip happens in a browser on
`bambulab.com` and only a short-lived ticket is handed to the desktop client.
Source for the ticket-exchange endpoint and response shape:
[`BAMBU_REFRESH_CONTRACT_RESEARCH.md` §"Path A"](./BAMBU_REFRESH_CONTRACT_RESEARCH.md)
and [`ClusterM/open-bamboo-networking` `NETWORK_PLUGIN.md`
§6.5.1](https://github.com/ClusterM/open-bamboo-networking/blob/master/NETWORK_PLUGIN.md).

The rest of this doc evaluates four ways to drive that ticket flow (or an
equivalent) for a Google-auth account, ranked by simplicity.

---

## Path 1A — Browser-only ticket extraction (without Bambu Studio)

**Historical status (2026-07-16): mechanism was not yet pinned. Superseded by
the 2026-08-28 T03 decision above: ticket issuance is visible, but delivery is
restricted to browser-host loopback and cannot reach the ESP32 WebUI.**

### What is confirmed from source

- The ticket-exchange endpoint that *consumes* the ticket is public and
  stable: `POST https://api.bambulab.com/v1/user-service/user/ticket/<TICKET>`
  with body `{"ticket":"<TICKET>"}`, returning the full token envelope.
  Source: `BAMBU_REFRESH_CONTRACT_RESEARCH.md` §"Path A" and OBN
  [`src/cloud_auth.cpp`](https://github.com/ClusterM/open-bamboo-networking/blob/master/src/cloud_auth.cpp)
  `login_with_ticket()`, commit
  [`f94fde3`](https://github.com/ClusterM/open-bamboo-networking/commit/f94fde3b3b72f44d39d0ce154278d678e7681f03).
- The ticket is described in OBN
  [`NETWORK_PLUGIN.md` §6.5.1](https://github.com/ClusterM/open-bamboo-networking/blob/master/NETWORK_PLUGIN.md)
  as "a short alphanumeric code (~6 chars) produced by a separate
  browser-based OAuth flow outside this library." That is the only
  description of the ticket's *origin* in any source consulted; the
  document does **not** name a `bambulab.com` URL or page that a user can
  visit to receive a ticket outside Studio.
- Bambu Studio source confirms a related bambulab.com URL pattern:
  `host_url + "api/sign-in/ticket?to=" + host_url + url_encode(model_url) +
  "&ticket=" + ticket` (used for deep-linking from Studio to a logged-in
  browser page). Source:
  [`src/slic3r/GUI/GUI_App.cpp`](https://github.com/bambulab/BambuStudio/blob/master/src/slic3r/GUI/GUI_App.cpp)
  around the model-link builder. **This is a ticket *consumer*, not a
  ticket *issuer*** — it consumes a ticket Studio already holds to open a
  bambulab.com page pre-authenticated. It is not a way to obtain a new
  ticket.

### Historical 2026-07-16 gap (now resolved for T03)

The July research could see only the webview `postMessage` and loopback
delivery surfaces, so it left browser-only issuance as an unknown. The August
production-bundle observation above found the issuer and the generic `to`
validator. The authenticated live-matrix probe then closed the remaining
question: issuance exists, but neither delivery surface accepts the ESP32
origin.

### Updated conclusion for Path 1A

The production portal now exposes the ticket-issuance path in its deployed
client code, but its desktop callback accepts only `http://localhost:<port>`.
Therefore browser-only delivery to an ESP32 WebUI is not merely undocumented;
it is rejected by the observed first-party callback contract. Do not replace
that contract with browser automation or a companion listener for T03.

---

## Path 1B — Bambu Studio ticket-exchange with loopback interception (RECOMMENDED)

**Status: confirmed end-to-end at the wire level from Bambu Studio source.
This is what Studio itself does for every Google sign-in.**

### Why this is the recommended path

Bambu Studio's official login flow for Google-OAuth accounts *is* the
ticket-exchange flow. By running Studio once and intercepting the
ticket-exchange HTTP call (or, equivalently, the loopback redirect that
delivers the ticket), you capture exactly the JSON envelope Studio sends to
its network plugin — the JSON that contains `accessToken`, `refreshToken`,
`expiresIn`, and `refreshExpiresIn`. The response shape is identical for
Google-OAuth and email/password accounts; only the front-end sign-in
mechanism differs. Source:
[`src/slic3r/GUI/HttpServer.cpp`](https://github.com/bambulab/BambuStudio/blob/master/src/slic3r/GUI/HttpServer.cpp)
`TokenResp` struct (lines defining `accessToken`, `refreshToken`, `expiresIn`,
`refreshExpiresIn`) and `TicketLoginTask::do_request_login_info()` (which
builds the `{data:{refresh_token, token, expires_in, refresh_expires_in,
user:{...}}}` envelope from the ticket-exchange response).

### The two interception surfaces Studio exposes

Studio obtains the ticket from `bambulab.com`'s `/sign-in` page via one of
two postMessage-driven paths, both confirmed in
[`src/slic3r/GUI/WebUserLoginDialog.cpp`](https://github.com/bambulab/BambuStudio/blob/master/src/slic3r/GUI/WebUserLoginDialog.cpp)
and mirrored in OrcaSlicer
([`src/slic3r/GUI/WebUserLoginDialog.cpp`](https://github.com/SoftFever/OrcaSlicer/blob/main/src/slic3r/GUI/WebUserLoginDialog.cpp)):

1. **Embedded-webview postMessage** — when the Google sign-in completes
   inside Studio's own embedded webview, the page fires a postMessage with
   command `user_ticket_login` and payload `{data:{ticket:"<T>"}}`. Source:
   `WebUserLoginDialog.cpp::OnScriptMessage` (branch `strCmd ==
   "user_ticket_login"`), which reads `j["data"]["ticket"]` and passes it
   to `TicketLoginTask::perform_async(ticket, …)`.
2. **Loopback HTTP redirect** — when bambulab.com's `/sign-in` page hands
   the user to their default browser (the `thirdparty_login` postMessage
   command → `wxLaunchDefaultBrowser(url)`), the browser ultimately
   redirects to `http://localhost:<PORT>/?ticket=<T>&redirect_url=<URL>`.
   Studio's local HTTP server (`HttpServer::session::handle_login()`) parses
   `ticket` out of the query string and exchanges it. Source:
   [`src/slic3r/GUI/HttpServer.cpp`](https://github.com/bambulab/BambuStudio/blob/master/src/slic3r/GUI/HttpServer.cpp)
   `parse_login_params()` and `handle_login()`.

Either way, the ticket reaches `POST /v1/user-service/user/ticket/<T>` and
the response JSON is the envelope you want.

### Concrete procedure a non-developer can follow (with one short script)

> One-time setup, ~5 minutes. Requires a computer that can run Bambu Studio
> and `curl`. No password needed — Google sign-in happens in the browser.

1. **Install and launch Bambu Studio** (or OrcaSlicer — same flow).
   Download from <https://bambulab.com/download/studio>.
2. **Sign in with Google** through Studio's login dialog. This opens
   `https://bambulab.com/<locale>/sign-in` in Studio's embedded webview
   (source: `WebUserLoginDialog.cpp` constructor, which sets
   `TargetUrl = host_url + "/" + strlang + "/sign-in"`). Complete the
   Google OAuth. Studio is now logged in and holds a valid token envelope
   internally.
3. **Verify the login persisted to disk.** On macOS, quit Studio and inspect
   `~/Library/Application Support/BambuStudio/` (Studio's `data_dir`; see
   Path 2 for the file-format caveat and exact sub-paths). The closed-source
   `libbambu_networking.dylib` writes its auth cache somewhere under this
   directory (OBN STATUS.md confirms `<config_dir>` is "used for auth cache,
   device-cert snapshots, and `<config_dir>/obn.env`"; source:
   [`ClusterM/open-bamboo-networking/STATUS.md` §6.1](https://github.com/ClusterM/open-bamboo-networking/blob/master/STATUS.md)
   row for `bambu_network_set_config_dir`). See Path 2 for how to locate
   the exact file empirically.
4. **If Path 2's file does not contain a usable `refreshToken`, re-run the
   ticket exchange explicitly via `curl`** and capture the JSON envelope
   directly. To do this you need a *fresh* ticket (each is single-use per
   OBN's comment in `cloud_auth.cpp`: *"The ticket is single-use and
   short-lived; any failure here means Studio will re-open the login
   dialog"*). The cleanest way is:
   - Sign out of Studio, sign in again with Google.
   - During sign-in, observe the Studio process's loopback HTTP traffic
     (e.g. with a tiny local proxy on the loopback port Studio reserves, or
     by reading Studio's log file under
     `~/Library/Application Support/BambuStudio/log/` which logs
     `"login url = ..."` and `"third_party_login: new request received"` per
     `WebUserLoginDialog.cpp` / `HttpServer.cpp` logging statements).
   - Grab the `ticket=<T>` value from the logged redirect URL.
   - Immediately exchange it:
     ```sh
     curl -X POST 'https://api.bambulab.com/v1/user-service/user/ticket/<TICKET>' \
          -H 'Content-Type: application/json' \
          --data '{"ticket":"<TICKET>"}'
     ```
     The response is the four-field envelope (`accessToken`, `refreshToken`,
     `expiresIn`, `refreshExpiresIn`) plus incidentals. Wire contract:
     `BAMBU_REFRESH_CONTRACT_RESEARCH.md` §"Login response schema".
5. **Pipe that JSON to `install-cloud-token`** per
   `TOKEN_ROTATION_RUNBOOK.md`.

> Operational note: the ticket is short-lived (seconds, per OBN's code
> comment). Step 4's `curl` must run within that window. If you miss it,
> re-trigger a sign-in in Studio and try again. Step 3 (just read the
> persisted file) is strongly preferred when it works — see Path 2.

### Honesty caveat for Path 1B

The exact wire shape of the loopback redirect
(`http://localhost:<PORT>/?ticket=<T>&redirect_url=<URL>`) is confirmed from
source. Current official Bambu Studio fixes the port at `13618` and binds
`127.0.0.1`; OrcaSlicer may reserve an ephemeral port, which is a separate
implementation detail. Neither form reaches a network ESP32.

---

## Path 2 — Read persisted tokens from Bambu Studio's macOS data dir

**Status: data_dir location confirmed; exact token file inside the
closed-source plugin is NOT pinned from open source. Verify locally.**

### What is confirmed from source

- **Studio's `data_dir` on macOS** is `~/Library/Application Support/BambuStudio/`.
  Source: `BAMBU_AUTH_ROTATION_RESEARCH.md` already pins this from Bambu
  Studio source (`src/slic3r/GUI/GUI_App.cpp` data_dir computation), and OBN
  `NETWORK_PLUGIN.md` §3.1 independently states: *"on macOS
  `~/Library/Application Support/BambuStudio/`"*.
- **The auth envelope Studio applies to its network plugin contains
  `refreshToken`.** Source:
  [`src/slic3r/GUI/HttpServer.cpp`](https://github.com/bambulab/BambuStudio/blob/master/src/slic3r/GUI/HttpServer.cpp)
  `do_request_login_info()`, which sets
  `j["data"]["refresh_token"] = token_data.refreshToken;` — so the value
  is in scope at the moment `agent->change_user(login_info)` is called.
  OrcaSlicer's `WebUserLoginDialog.cpp` `OnScriptMessage` branch for
  `user_ticket_login` independently confirms `refresh_token =
  token_j["refreshToken"].get<std::string>()` is extracted and passed
  forward.
- **The plugin stores auth cache under `<config_dir>` (= `data_dir`).**
  Source: OBN STATUS.md §6.1 row for `bambu_network_set_config_dir`:
  *"Stored on the agent; used for auth cache, device-cert snapshots, and
  `<config_dir>/obn.env`"*. The stock closed-source plugin uses the same
  `data_dir`.

### What is NOT pinned from open source (Gap)

The **exact filename** inside `~/Library/Application Support/BambuStudio/`
where the *stock* `libbambu_networking.dylib` persists the `refreshToken`
is not visible in any open-source repository consulted (BambuStudio source
delegates persistence to the closed-source plugin via
`agent->change_user()`; OBN's own open-source plugin persists in its own
format, not Studio's). Candidate locations to inspect empirically on a
logged-in Mac:

- `~/Library/Application Support/BambuStudio/network/` (typical
  plugin-private subdir convention)
- `~/Library/Application Support/BambuStudio/session/` or
  `~/Library/Application Support/BambuStudio/session.json`
- The plugin's `obn.env` analogue (Studio's stock equivalent is not named
  in source)
- `~/Library/Application Support/BambuStudio/BambuStudio.conf` — this is
  Studio's main INI (wxFileConfig); it holds `installed_networking`,
  `update_network_plugin`, locale, etc., but **not** the token set in any
  version inspected (the token is plugin-private state). Source for the
  conf's role: `GUI_App.cpp` references to `app_config->get(...)`,
  `app_config->set(...)` for `installed_networking` /
  `update_network_plugin` / `ignore_module_cert`.

### Concrete procedure to find the file empirically

```sh
# 1. Snapshot the directory before login
ls -laR ~/Library/Application\ Support/BambuStudio/ > /tmp/before.txt

# 2. Launch Bambu Studio, sign in with Google, quit Studio

# 3. Snapshot again and diff — the new/modified files are where the session landed
ls -laR ~/Library/Application\ Support/BambuStudio/ > /tmp/after.txt
diff /tmp/before.txt /tmp/after.txt

# 4. Grep the changed files for the access-token prefix to confirm which one
#    holds the token envelope. The access token starts with "AQA" or "AQB"
#    (source: NETWORK_PLUGIN.md §6.5.1: "Token format: AQA… / AQB… prefix").
grep -lE 'AQA|AQB' ~/Library/Application\ Support/BambuStudio/network/* 2>/dev/null

# 5. Inspect the matched file. If it contains a "refreshToken" field, you
#    have what you need; pipe that JSON to install-cloud-token.
```

### Honesty caveat for Path 2

- **Whether the stock plugin persists `refreshToken` to disk in plaintext**
  is not confirmed from open source. It is **possible** the plugin persists
  only the `accessToken` + a session cookie and re-derives the refresh token
  via the cloud MQTT cert pipeline, in which case the file would not contain
  a usable `refreshToken` string. The empirical procedure above resolves
  this in 30 seconds on a logged-in Mac; do that rather than assuming.
- The OBN open-source plugin (a drop-in replacement) definitely persists
  both tokens because it implements refresh — but its on-disk format is
  OBN's, not Studio's, so an OBN-persisted file only exists if the user has
  *replaced* their stock plugin with OBN. If they have, the same `ls +
  diff` procedure finds OBN's auth file under the same `data_dir`.

### OrcaSlicer note

OrcaSlicer stores its data under `~/Library/Application Support/OrcaSlicer/`
on macOS and uses the same C ABI network plugin. If the user has OrcaSlicer
installed and logged in via Google, repeat the procedure against that
directory. Source: OBN STATUS.md "Supported slicers" table (Linux paths
shown; macOS uses the same `Application Support/<SlicerName>/` convention).

---

## Path 3 — Home Assistant `bambu_lab` integration (DEAD END)

**Status: confirmed dead end for both Google-OAuth login and `refreshToken`
persistence. Do not pursue.**

### Google-OAuth login: not supported

HA's `bambu_lab` integration offers exactly one cloud login mechanism:
**email + password** (with optional email-verification-code or TOTP second
factor). Source:
[`custom_components/bambu_lab/config_flow.py`](https://github.com/greghesp/ha-bambulab/blob/main/custom_components/bambu_lab/config_flow.py)
`async_step_Bambu` — the form fields are `region`, `email`, `password`
(`fields[vol.Required('password', ...)] = PASSWORD_SELECTOR`), with the
verifyCode / tfaCode branches triggered only by the server's response. There
is **no** "Sign in with Google" option in the config flow. The underlying
login call is
[`custom_components/bambu_lab/pybambu/bambu_cloud.py`](https://github.com/greghesp/ha-bambulab/blob/main/custom_components/bambu_lab/pybambu/bambu_cloud.py)
`BambuCloud.login(region, email, password)` → `_get_authentication_token()`
→ `POST /v1/user-service/user/login` with body
`{"account":"<email>","password":"<password>","apiError":""}`. It cannot
perform Google OAuth itself, but it can use a Bambu password added to the
same Google-created account.

### `refreshToken` persistence: never stored

Even for accounts that *do* have a password, HA deliberately discards the
`refreshToken`. Source:
[`custom_components/bambu_lab/pybambu/bambu_cloud.py`](https://github.com/greghesp/ha-bambulab/blob/main/custom_components/bambu_lab/pybambu/bambu_cloud.py)
`_get_authentication_token()`:

```python
auth_json = response.json()
accessToken = auth_json.get('accessToken', '')
if accessToken != '':
    return accessToken          # <-- only the accessToken is returned
```

The function returns *only* `accessToken`; the response's `refreshToken`,
`expiresIn`, and `refreshExpiresIn` fields are dropped on the floor. The HA
config entry correspondingly persists only `auth_token` (no `refresh_token`
field exists in either `data` or `options` — see `config_flow.py`
`async_step_Bambu_Lan`'s `options = {...}` dict, which contains `auth_token`
but no `refresh_token`). This is consistent with the maintainer's documented
decision in
[issue #565](https://github.com/greghesp/ha-bambulab/issues/565) not to
implement refresh: HA instead prompts the user to re-authenticate when the
~1-year access token expires. Confirmed independently by
`BAMBU_REFRESH_CONTRACT_RESEARCH.md` §"Stability / maintenance".

### Conclusion for Path 3

HA cannot perform Google OAuth and does not persist a refresh token even for
password accounts. **It remains a dead end for bootstrapping autonomous
refresh.** After Path 4 the account could pass HA's email/password login, but
HA would still discard the returned `refreshToken`.

---

## Path 4 — Set a Bambu password on the Google-created account

**Status: CONFIRMED for the target account by a real first-party-site
observation on 2026-08-28. This is the accepted device path.**

### Confirmed flow

The official Bambu `Forgot password` flow allowed the Google-created account
to **set a Bambu password**. The account then accepted the ordinary
email/password login flow documented in
`BAMBU_REFRESH_CONTRACT_RESEARCH.md` §"Path B". That endpoint's response
envelope includes `refreshToken` like any email/password login.

### What is confirmed

- The user opened the official Bambu password-reset flow for the email of an
  existing Google-created account and successfully set a password.
- The user then successfully signed in using email + that Bambu password.
- No secret was disclosed to or persisted by this spike.
- This is an empirical feasibility observation for the target account, not a
  claim that Bambu documents or guarantees the behavior for every account,
  region, or future portal version.

### Concrete procedure to check

1. Open <https://bambulab.com/en/account/forgot-password>.
2. Set a Bambu password for the email used by the Google-created account.
3. In the device WebUI, use that email and Bambu password; complete
   verification/TOTP there if requested.

### Honesty caveat for Path 4

- A second factor (`verifyCode` via email/SMS or `tfa` via TOTP) may still
  be required even after a password is set — see
  `BAMBU_REFRESH_CONTRACT_RESEARCH.md` §"Does login require 2FA / captcha /
  device verification / email code?". That is a property of the account's
  security settings, not of the Google-OAuth-ness; if enabled, the device
  WebUI must expose that verification step.
- Password setup is a one-time account prerequisite. The device still owns
  login, verification, token lifecycle and printer selection; no desktop
  listener, manual token transfer or external callback is introduced.

---

## Summary table — which path to actually use

| Priority | Path | When to use | Effort | Returns `refreshToken`? |
|---|---|---|---|---|
| 1 | **4** (set a Bambu password) | Accepted device onboarding for Google-created account | One-time | ✅ Yes |
| — | **1B** (Studio + log/loopback intercept) | Existing relay/operator flow only; disallowed for device onboarding | ~10 min | ✅ Yes |
| — | **2** (read Studio's persisted token file) | Relay/operator investigation only | ~5 min | ⚠️ Access token yes; refresh token verify-locally |
| — | **1A** (browser-only ticket to device) | Issuer visible; official callback is localhost-only | — | **OAuth callback blocked** |
| — | **3** (HA `bambu_lab`) | Dead end — no Google login, no refresh token | — | ❌ No |

---

## Remaining gaps (stated explicitly, not guessed)

- **No first-party arbitrary-device callback was found.** The deployed portal
  code exposes ticket issuance but gates Studio/Suite delivery to the exact
  localhost-only form documented above. A future supported device-code flow
  or explicit local-network callback would be a new contract and should
  trigger ADR reconsideration.
- **Stock `libbambu_networking.dylib` token filename on macOS is not in
  open source.** The directory (`~/Library/Application Support/BambuStudio/`)
  and the fact that auth cache lives under it are confirmed; the exact file
  the *closed-source* plugin writes (and whether it contains a plaintext
  `refreshToken`) is not. Resolvable empirically in seconds via `ls + diff`
  (Path 2 procedure).
- **Bambu does not document a stability guarantee for adding a password to a
  Google-created account.** The behavior is nevertheless confirmed for the
  target account by the successful 2026-08-28 password setup and subsequent
  email/password login. A future failure of this prerequisite triggers ADR
  reconsideration.
- **Loopback behavior differs by slicer.** Current official Bambu Studio uses
  fixed port `13618`; OrcaSlicer may reserve an ephemeral port. Both are
  browser-host loopback and therefore equivalent for the T03 blocker.

---

## Primary sources

- `ClusterM`, [`open-bamboo-networking`](https://github.com/ClusterM/open-bamboo-networking)
  — open-source drop-in for Bambu Studio's proprietary `bambu_networking`
  plug-in. Ticket-exchange implementation: [`src/cloud_auth.cpp`](https://github.com/ClusterM/open-bamboo-networking/blob/master/src/cloud_auth.cpp) `login_with_ticket()` (commit [`f94fde3`](https://github.com/ClusterM/open-bamboo-networking/commit/f94fde3b3b72f44d39d0ce154278d678e7681f03), 2026-07-11). Wire contract narrative: [`NETWORK_PLUGIN.md`](https://github.com/ClusterM/open-bamboo-networking/blob/master/NETWORK_PLUGIN.md) §6.5.1 (ticket login response shape, ticket description "short alphanumeric code ~6 chars produced by a separate browser-based OAuth flow outside this library"). Per-symbol ABI coverage incl. `<config_dir>` auth-cache role: [`STATUS.md`](https://github.com/ClusterM/open-bamboo-networking/blob/master/STATUS.md) §6.1 row for `bambu_network_set_config_dir` and §6.5 row for `bambu_network_get_my_token`. Pinned at master HEAD [`3d58c81`](https://github.com/ClusterM/open-bamboo-networking/commit/3d58c81b1a7887c42b8c61e9256de23d8fe6087c) (2026-07-15).
- `bambulab`, [`BambuStudio`](https://github.com/bambulab/BambuStudio)
  — official open-source slicer (closed-source network plug-in). Login URL
  construction (`https://bambulab.com/<locale>/sign-in` in embedded webview):
  [`src/slic3r/GUI/WebUserLoginDialog.cpp`](https://github.com/bambulab/BambuStudio/blob/master/src/slic3r/GUI/WebUserLoginDialog.cpp) constructor. postMessage command handling (`user_ticket_login` → `TicketLoginTask::perform_async(ticket)`; `thirdparty_login` → `wxLaunchDefaultBrowser(url)`;
  `get_localhost_url` → returns Studio's loopback base URL to the page):
  `WebUserLoginDialog.cpp::OnScriptMessage`. Loopback HTTP server (parses
  `ticket` and `redirect_url` from `http://localhost:<PORT>/?…`): [`src/slic3r/GUI/HttpServer.cpp`](https://github.com/bambulab/BambuStudio/blob/master/src/slic3r/GUI/HttpServer.cpp) `parse_login_params()`, `handle_login()`. Token-envelope struct (`accessToken`, `refreshToken`, `expiresIn`, `refreshExpiresIn`, `tfaKey`, `accessMethod`, `loginType`) and the
  `{data:{refresh_token, token, expires_in, refresh_expires_in, user:{…}}}`
  envelope builder: `HttpServer.cpp` `TokenResp` and
  `TicketLoginTask::do_request_login_info()`. bambulab.com
  `api/sign-in/ticket?to=…&ticket=…` deep-link URL (ticket *consumer*, not
  issuer): `src/slic3r/GUI/GUI_App.cpp` model-link builder. Data dir
  (`~/Library/Application Support/BambuStudio/` on macOS): established in
  `BAMBU_AUTH_ROTATION_RESEARCH.md` from `src/slic3r/GUI/GUI_App.cpp`.
- `SoftFever`, [`OrcaSlicer`](https://github.com/SoftFever/OrcaSlicer)
  — mirrors Bambu Studio's login flow with more open code paths. Ephemeral
  loopback port reservation and postMessage `user_ticket_login` handling
  with explicit `refresh_token = token_j["refreshToken"]` extraction: [`src/slic3r/GUI/WebUserLoginDialog.cpp`](https://github.com/SoftFever/OrcaSlicer/blob/main/src/slic3r/GUI/WebUserLoginDialog.cpp) `reserve_loopback_port()`, `ensure_loopback_port()`, `OnScriptMessage`.
- `greghesp`, [`ha-bambulab`](https://github.com/greghesp/ha-bambulab)
  — canonical Home Assistant `bambu_lab` integration. Email/password-only
  login flow (no Google-OAuth path): [`custom_components/bambu_lab/config_flow.py`](https://github.com/greghesp/ha-bambulab/blob/main/custom_components/bambu_lab/config_flow.py) `async_step_Bambu`. Login call and
  `accessToken`-only capture (refresh token discarded): [`custom_components/bambu_lab/pybambu/bambu_cloud.py`](https://github.com/greghesp/ha-bambulab/blob/main/custom_components/bambu_lab/pybambu/bambu_cloud.py) `BambuCloud.login()` and `_get_authentication_token()`. Config-entry `options` schema
  (has `auth_token`, no `refresh_token`): `config_flow.py`
  `async_step_Bambu_Lan`. Maintainer's documented decision not to implement
  refresh: [issue #565](https://github.com/greghesp/ha-bambulab/issues/565).
- Existing project research this doc extends:
  [`BAMBU_REFRESH_CONTRACT_RESEARCH.md`](./BAMBU_REFRESH_CONTRACT_RESEARCH.md)
  — refresh endpoint contract, login paths A (ticket) and B (email/password),
  response schema, lifetime observations; and
  [`BAMBU_AUTH_ROTATION_RESEARCH.md`](./BAMBU_AUTH_ROTATION_RESEARCH.md) —
  first-party-only research concluding no supported headless refresh contract,
  pinned macOS `data_dir`.
