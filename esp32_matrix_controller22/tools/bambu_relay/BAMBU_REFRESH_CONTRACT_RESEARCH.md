# Bambu Cloud token-refresh wire contract (community / reverse-engineered sources)

Research date: 2026-07-16
Scope: Third-party / community reverse-engineering sources — explicitly extends
`BAMBU_AUTH_ROTATION_RESEARCH.md` (which was restricted to Bambu Lab first-party
sources and concluded "no public supported headless refresh contract"). The
project has accepted the risk of building on an unsupported, reverse-engineered
contract; this document extracts the actual wire contract from the code that
already implements it.

> Not a design document. No daemon code is proposed here. This is extraction
> and citation only, mirroring the structure and citation discipline of
> `BAMBU_AUTH_ROTATION_RESEARCH.md`.

## Executive conclusion

**Yes — the community has reverse-engineered the Bambu Cloud access-token
refresh transaction, and the wire contract is documented and runtime-validated
in at least one actively-maintained source.** The canonical source is
`ClusterM/open-bamboo-networking` (OBN), a clean-room open-source drop-in
replacement for Bambu Studio's proprietary `bambu_networking` plug-in; its
auth surface is verified against SSLKEYLOGFILE/MITM captures of the stock
plug-in plus active probing of `api.bambulab.com`.

**Watershed answer (bare refresh token vs. client-identity metadata):** as of
the research date, the refresh endpoint works with a **bare refresh token**
placed in the JSON request body and **no client-identity metadata** — no
`Authorization` header, no `User-Agent` mimicking Bambu Studio, no `X-BBL-*`
fingerprint block, no `client_id`, no signature. OBN's runtime refresh path
proves this: it sends only the three generic JSON headers (`User-Agent:
OBN/<version>`, `Accept`, `Content-Type`) plus the body, and the call
succeeds in production. The Home Assistant integration author also confirmed
the endpoint name (`/v1/user-service/user/refreshtoken`) in
[greghesp/ha-bambulab #565](https://github.com/greghesp/ha-bambulab/issues/565).

**However, this is fragile and trending toward "identity metadata required".**
OBN's own maintainer opened
[ClusterM/open-bamboo-networking #67](https://github.com/ClusterM/open-bamboo-networking/issues/67)
on 2026-07-15 (the day before this research) to add the **full stock
`X-BBL-*` block in stock wire order to every `api.bambulab.com` call**,
explicitly because "`api.bambulab.com` sits behind Cloudflare, which
fingerprints the request's header set + order (JA4H) … a couple of endpoints
reject calls missing `X-BBL-Client-ID`". The refresh endpoint tolerates bare
clients today; it may stop doing so without notice.

## Sources consulted (overview)

- **`ClusterM/open-bamboo-networking`** (433 stars, last push 2026-07-15, very
  active). Open-source replacement for the closed `bambu_networking` plug-in;
  refresh is implemented and wired into the agent lifecycle. Pinned at master
  HEAD [`3d58c81`](https://github.com/ClusterM/open-bamboo-networking/commit/3d58c81b1a7887c42b8c61e9256de23d8fe6087c)
  (2026-07-15). All endpoint claims in its `NETWORK_PLUGIN.md` §6.5 are marked
  "MITM + probe" evidence.
- **`greghesp/ha-bambulab`** (2,252 stars; the canonical Home Assistant `bambu_lab`
  integration; vendored `pybambu` lives under `custom_components/bambu_lab/pybambu/`).
  HEAD on `main` at research date: [`f7e5290`](https://github.com/greghesp/ha-bambulab/commit/f7e52909949822b76bcd9b7c95cd55c421510aa8).
  **Does not implement refresh.** Issue #565 confirms the endpoint name and
  the maintainer's history of it breaking.
- **`bambulab/BambuStudio`** at [`ba049f6`](https://github.com/bambulab/BambuStudio/commit/ba049f6a2e08c3b6033660bb84da80c08722974b)
  (already pinned in `BAMBU_AUTH_ROTATION_RESEARCH.md`). Confirms response
  field names only — the `/refresh_token` route in its open source is the
  Agora live-video URL refresh, not the account-token refresh (see
  `BAMBU_AUTH_ROTATION_RESEARCH.md` §"What the official sources establish").
- Other community implementations checked (`zhaobenny/bambulab-cloud-py`,
  `torbenconto/bambulabs_cloud_api`, `BambuTools/bambulabs_api`,
  `mattcar15/bambu-connect`, `DMontgomery40/mcp-3D-printer-server`): none
  implement account-token refresh. Several implement only LAN access-code
  auth; the few that do cloud login ignore the `refreshToken` entirely and
  re-prompt on expiry.

## Refresh endpoint contract (core deliverable)

All claims below are from
[`src/cloud_auth.cpp`](https://github.com/ClusterM/open-bamboo-networking/blob/master/src/cloud_auth.cpp)
(commit [`f94fde3`](https://github.com/ClusterM/open-bamboo-networking/commit/f94fde3b3b72f44d39d0ce154278d678e7681f03),
2026-07-11) and
[`src/http_client.cpp`](https://github.com/ClusterM/open-bamboo-networking/blob/master/src/http_client.cpp)
in `ClusterM/open-bamboo-networking`. The function is invoked at runtime from
[`src/agent.cpp`](https://github.com/ClusterM/open-bamboo-networking/blob/master/src/agent.cpp)
`Agent::hydrate_session()` (commit
[`3d58c81`](https://github.com/ClusterM/open-bamboo-networking/commit/3d58c81b1a7887c42b8c61e9256de23d8fe6087c),
2026-07-15), so this is live code, not documentation-only.

### URL and regional hosts

- Single global REST host: `https://api.bambulab.com`
- China-region host: `https://api.bambulab.cn`
- No per-region API prefixes (e.g. no `eu.`, no `asia.`). The only region
  switch is Global vs. China, driven by the account's country code.
  Source: [`src/config.cpp`](https://github.com/ClusterM/open-bamboo-networking/blob/master/src/config.cpp)
  (`kDefaultGlobalApi` / `kDefaultCnApi` + `cloud_api_host_for()`). The same
  two-host split is independently confirmed in `greghesp/ha-bambulab`
  `custom_components/bambu_lab/pybambu/utils.py` `get_Url()` (China → `.com`
  → `.cn` swap). Regional MQTT brokers do have regional prefixes
  (`us.mqtt.bambulab.com` / `cn.mqtt.bambulab.com`); REST does not.
- Full URL: `https://api.bambulab.com/v1/user-service/user/refreshtoken`
  (dash-less form). The endpoint name is confirmed at the "MITM + probe"
  evidence level in
  [`NETWORK_PLUGIN.md` §6.5 / endpoint-evidence table line ~2784](https://github.com/ClusterM/open-bamboo-networking/blob/master/NETWORK_PLUGIN.md).

### HTTP method

`POST`. Source: `cloud_auth.cpp` `refresh_token()` calls `obn::http::post_json(...)`
with the URL and body.

### Request headers — the watershed question, answered explicitly

OBN's refresh call sends **only** these headers, and nothing else:

| Header | Value | Source |
|---|---|---|
| `User-Agent` | `OBN/<version>` (e.g. `OBN/02.08.01.51`) — **not** a Bambu-mimicking UA | `http_client.cpp` `default_user_agent()` returns `"OBN/" + OBN_VERSION_STRING` |
| `Accept` | `application/json` | `http_client.cpp` `perform()` default, applied when not set |
| `Content-Type` | `application/json` | `http_client.cpp` `perform()` default for POST |

**No `Authorization` header. No `X-BBL-Client-Name`, `X-BBL-Client-Type`,
`X-BBL-Client-Version`, `X-BBL-OS-Type`, `X-BBL-Agent-Version`,
`X-BBL-Device-ID`, `X-BBL-Client-ID`. No signature. No API key. No
`client_id`.**

This is the explicit answer to the watershed question: **the refresh call, as
deployed in OBN today, works with a bare refresh token and no client-identity
metadata.** Three independent pieces of evidence converge:

1. `cloud_auth.cpp` `refresh_token()` calls
   `obn::http::post_json(url, body)` with **no headers map argument at all** —
   no per-call identity headers are attached.
2. `http_client.cpp` `perform()` adds only the three defaults above when the
   caller omits them; nothing else is injected for the auth endpoints.
3. The function is invoked at runtime from `agent.cpp` `hydrate_session()`,
   which only treats the result as successful when the returned `accessToken`
   is non-empty; OBN is in active daily use against live `api.bambulab.com`,
   so the bare-header call demonstrably succeeds.

A related confirmation that the server does not enforce `X-BBL-*` on auth-style
endpoints appears in `STATUS.md` §6.9 (user presets): "using only the user's
bearer token (the stock `X-BBL-*` fingerprint headers aren't required by the
server)". `NETWORK_PLUGIN.md` §6.10 (around the bind endpoint) is even more
explicit: "No `Authorization` bearer is required for this endpoint (live probes
return `code:0` anonymously when the envelope is valid). Stock Studio still
sends the usual `X-BBL-*` / `User-Agent: bambu_network_agent/…` fingerprint
headers; **Cloudflare may 403 bare clients without them**."

**Contrast — where `X-BBL-*` IS server-enforced:** `NETWORK_PLUGIN.md` §6.8.1.1
("Cloud print authorization") states that `POST /v1/user-service/my/task`
**requires** `X-BBL-Client-Name: BambuStudio` and a matching `X-BBL-OS-Type`
("mandatory; a wrong/missing value 403s") plus the command-security
`x-bbl-app-certification-id` / `x-bbl-device-security-sign` headers. So
Bambu's enforcement is **per-endpoint**, not uniform. Refresh and the other
auth endpoints are currently on the permissive side of that line.

**Trend / fragility (important):** OBN's maintainer opened
[issue #67](https://github.com/ClusterM/open-bamboo-networking/issues/67) on
2026-07-15 to retroactively add the full stock `X-BBL-*` block (in stock wire
order, to defeat Cloudflare JA4H fingerprinting) to **every**
`api.bambulab.com` call, including `cloud_auth.cpp`. The issue text reads in
part: "`api.bambulab.com` sits behind Cloudflare, which fingerprints the
request's header set + order (JA4H) … a couple of endpoints reject calls
missing `X-BBL-Client-ID`." The implementation note for the change says
"pre-auth calls (ticket exchange, refresh, profile) send it without
`Authorization`/`X-BBL-Client-ID`" — i.e. even after the change, refresh will
not send `Authorization`, but **will** send the `X-BBL-*` identity block.
Conclusion: a relay that bets on "bare refresh token works forever" is betting
against a documented, active hardening trend.

### Request body schema

Single JSON object, one field:

```json
{ "refreshToken": "<refresh_token_string>" }
```

Source: `cloud_auth.cpp` `refresh_body()` builds exactly this string via
`os << "{\"refreshToken\":" << obn::json::escape(refresh) << '}'`. Field name
is `refreshToken` (camelCase, matching the login-response field). No other
fields (`account`, `client_id`, `grant_type`, etc.) are sent — the body is
literally the one key.

### Response body schema

Same shape as the login/ticket response (`fill_auth_fields()` in
`cloud_auth.cpp` is shared between login and refresh). Confirmed fields,
parsed by name:

| Field | Type | Notes |
|---|---|---|
| `accessToken` | string | New short-lived(ish) access token; its presence is OBN's success criterion (`r.ok = !r.access_token.empty()`) |
| `refreshToken` | string | New refresh token if the server rotated it; may equal the prior value if it did not (see "Rotation semantics" below) |
| `expiresIn` | int (seconds) | Lifetime of the new `accessToken`. Observed value at issue: `31536000` (1 year) for the ticket-login flow (`NETWORK_PLUGIN.md` §6.5.1) |
| `refreshExpiresIn` | int (seconds) | Lifetime of the new `refreshToken`. Observed at issue: `31536000` (1 year) |
| `loginType` | string | Parsed but not used by OBN's refresh path |
| `tfaKey` | string | Parsed but not used by refresh (only meaningful on login) |
| `accessMethod` | string | Observed `"ticket"` for the browser-ticket login flow; not meaningful for refresh |

The four token fields are the same four already established as the login
shape in `BAMBU_AUTH_ROTATION_RESEARCH.md` from Bambu Studio's own
`HttpServer.cpp` `TokenResp` struct (`accessToken`, `refreshToken`,
`expiresIn`, `refreshExpiresIn`). OBN additionally confirms two incidental
fields (`tfaKey`, `accessMethod`, `loginType`) that Bambu Studio's struct
also models — confirming the response envelope is the same object on both
endpoints.

### Auth mechanism (where the refresh token goes)

The refresh token is in the **request body**, as the `refreshToken` JSON
field. It is **not** in a header, **not** in a cookie, **not** in the URL.
The request carries no `Authorization` header at all (it is a "pre-auth" call
in OBN's classification). Source: `cloud_auth.cpp` `refresh_body()` and
`refresh_token()`; corroborated by issue #67's note that "pre-auth calls
(ticket exchange, refresh, profile) send [the identity block] without
`Authorization`/`X-BBL-Client-ID`".

## Login endpoint contract (context for the initial `refreshToken`)

There are **two** distinct login paths in the community sources. Either can
produce the `refreshToken` consumed by the refresh endpoint above.

### Path A — Bambu Studio's own ticket exchange (the path the official client uses)

- URL: `POST https://api.bambulab.com/v1/user-service/user/ticket/<TICKET>`
  (China: `api.bambulab.cn`)
- Body: `{"ticket":"<TICKET>"}`
- The `<TICKET>` is a short (~6 char) alphanumeric code produced by a
  separate **browser-based OAuth flow** outside the plug-in (the user signs
  in via Bambu's website, which hands the ticket to Studio over a local
  callback).
- Response (200): full token envelope.
- Source: `NETWORK_PLUGIN.md` §6.5.1, marked "Confirmed from SSLKEYLOGFILE
  capture", and `cloud_auth.cpp` `login_with_ticket()`.
- Significance for automation: **this path is not password-replayable** —
  the ticket must be minted by a browser OAuth round-trip on
  `bambulab.com`. A headless relay cannot use Path A without driving a
  browser.

### Path B — Community email/password login (what HA uses)

- URL: `POST https://api.bambulab.com/v1/user-service/user/login`
  (China: `api.bambulab.cn`)
- Body: `{"account":"<email>","password":"<password>","apiError":""}`
- Source: `greghesp/ha-bambulab`
  [`custom_components/bambu_lab/pybambu/bambu_cloud.py`](https://github.com/greghesp/ha-bambulab/blob/main/custom_components/bambu_lab/pybambu/bambu_cloud.py)
  `_get_authentication_token()` (file last touched by commit
  [`6d7b73e`](https://github.com/greghesp/ha-bambulab/commit/6d7b73ef94),
  2025-12-07). URL defined in
  [`custom_components/bambu_lab/pybambu/const.py`](https://github.com/greghesp/ha-bambulab/blob/main/custom_components/bambu_lab/pybambu/const.py)
  `BAMBU_URL[BambuUrl.LOGIN]`.
- Request headers HA sends (the canonical "mimic Studio" set):
  `User-Agent: bambu_network_agent/01.09.05.01`,
  `X-BBL-Client-Name: OrcaSlicer`, `X-BBL-Client-Type: slicer`,
  `X-BBL-Client-Version: 01.09.05.51`, `X-BBL-Language: en-US`,
  `X-BBL-OS-Type: linux`, `X-BBL-OS-Version: 6.2.0`,
  `X-BBL-Agent-Version: 01.09.05.01`, `X-BBL-Executable-info: {}`,
  `X-BBL-Agent-OS-Type: linux`, plus `accept` and `Content-Type`
  `application/json`. (Source: `bambu_cloud.py` `_get_headers()`.) HA's
  headers are actually labelled as OrcaSlicer rather than BambuStudio — the
  server accepts either.
- **Crucially, a third implementation (`torbenconto/bambulabs_cloud_api`,
  Go) logs in successfully with the totally non-mimicking
  `User-Agent: BambulabsCloudAPI/1.0` and no `X-BBL-*` at all**
  (`cloud.go` `const userAgent`). This independently corroborates OBN's
  finding that the server does not strictly enforce identity metadata on
  the auth endpoints — but, as above, Cloudflare may still interpose.

### Does login require 2FA / captcha / device verification / email code?

**Yes, conditionally — and that condition blocks password-replay automation
for any account with second-factor enabled.** Source:
`greghesp/ha-bambulab` `bambu_cloud.py` `_get_authentication_token()`:

- The `/login` response may return `accessToken` directly (no second factor).
- Or it may return `{"loginType":"verifyCode", ...}` — in which case the
  client must request an email code via
  `POST /v1/user-service/user/sendemail/code` (body `{"email":"<email>",
  "type":"codeLogin"}`) or an SMS code via
  `https://bambulab.cn/api/v1/user-service/user/sendsmscode`, then re-POST to
  `/login` with `{"account":"<email>","code":"<6-digit>"}`. The code is
  one-time, delivered out-of-band to the account's email/phone. Source:
  `_get_email_verification_code()`, `_get_sms_verification_code()`,
  `_get_authentication_token_with_verification_code()`.
- Or it may return `{"loginType":"tfa", "tfaKey":"..."}` — in which case the
  client must POST `{"tfaKey":"...","tfaCode":"<6-digit TOTP>"}` to
  `https://bambulab.com/api/sign-in/tfa`, and the access token comes back as
  a **cookie** named `token` (not in the JSON body). Source:
  `_get_authentication_token_with_2fa_code()` and the `TFA_LOGIN` URL in
  `const.py`.

**Net effect on automation:** username+password replay alone is feasible
only for accounts with no 2FA and no email/SMS verification configured. For
any account where Bambu's login response selects `verifyCode` or `tfa`,
full automation requires either an out-of-band channel the relay cannot
satisfy (mailbox/SMS/TOTP secret). This is consistent with the existing
project decision in `BAMBU_AUTH_ROTATION_RESEARCH.md`: persisting the
account password on the router does not eliminate the rotation problem
because the second-factor interaction cannot be replayed.

### Login response schema (both paths)

Path A (ticket) and Path B (password) both return the four-field token
envelope, plus the incidentals:

```json
{
  "accessToken":    "AQB...",        
  "refreshToken":   "AQB...",
  "expiresIn":      31536000,
  "refreshExpiresIn": 31536000,
  "tfaKey":         "",
  "accessMethod":   "ticket",
  "loginType":      "",
  "firstAppLogin":  false
}
```

Source for Path A: `NETWORK_PLUGIN.md` §6.5.1 ("Confirmed from SSLKEYLOGFILE
capture"). Source for the four core fields at the Bambu-Studio-source level:
`BambuStudio` `src/slic3r/GUI/HttpServer.cpp` `TokenResp` struct at the
commit pinned in `BAMBU_AUTH_ROTATION_RESEARCH.md`. Token format: `AQA…` /
`AQB…` prefix, base64url-encoded opaque token, ~100+ chars
(`NETWORK_PLUGIN.md` §6.5.1).

> **Note on access-token format:** the ticket flow returns an opaque
> `AQA/AQB…` token. The community email/password flow can return a JWT
> instead — `greghesp/ha-bambulab` `_get_username_from_authentication_token()`
> branches on whether `auth_token.split(".")` has length 3 (JWT) or not, and
> falls back to a `/my/preference` lookup for the non-JWT case. So the
> username-extraction logic in the relay must handle both shapes; the refresh
> contract itself does not depend on which shape the access token has.

## Token & session lifetimes

Concrete observed values, with source:

- **`expiresIn` = 31 536 000 s (exactly 1 year)** for the access token at
  issue, observed in the ticket-login SSLKEYLOGFILE capture
  (`NETWORK_PLUGIN.md` §6.5.1).
- **`refreshExpiresIn` = 31 536 000 s (1 year)** for the refresh token at
  issue, same capture.
- The HA integration maintainer (`AdrianGarside`) corroborates the order of
  magnitude independently in
  [issue #565](https://github.com/greghesp/ha-bambulab/issues/565): "They
  currently last a year" (2024-11-05) and "The tokens don't expire for a
  long time (a year)" (2024-10-22).
- **Whether refresh extends the lifetimes:** refresh returns fresh `expiresIn`
  / `refreshExpiresIn` values alongside the new tokens, so the access token's
  clock is reset on every successful refresh. OBN's `Agent::hydrate_session()`
  uses the returned `expires_in` to compute the new expiry, falling back to
  `3 * 30 * 24 * 3600` (90 days) if the field is absent or non-positive
  (`agent.cpp`). The one-year initial value is therefore a server-side
  default, not a hard contract; the relay should always trust the
  server-returned `expiresIn`/`refreshExpiresIn` rather than hard-code 365 days.

> Gap: the community sources do not contain an observation of `expiresIn`
> *after* a refresh cycle on a long-lived account (i.e. it is not confirmed
> whether Bambu re-issues another full year on each refresh, or shortens it).
> Treat the post-refresh lifetime as "whatever the server returns in
> `expiresIn`" and do not assume it equals the initial value.

## Error / revocation / rotation semantics

- **Refresh-token rotation (one-time-use?):** the refresh response **can**
  return a new `refreshToken` distinct from the one posted. OBN's
  `Agent::hydrate_session()` handles this explicitly:
  `auth_store_->update_tokens(r.access_token, r.refresh_token.empty() ? s.refresh_token : r.refresh_token, ...)`
  — i.e. persist the new refresh token when the server rotated it, otherwise
  reuse the prior one (`agent.cpp`). `NETWORK_PLUGIN.md` §6.5.1 separately
  notes that "`accessToken` and `refreshToken` are the same value on initial
  issue — they may diverge after a token refresh cycle." This is consistent
  with (but does not by itself prove) server-side refresh-token rotation; the
  contract is best summarized as **"the server may rotate; the client must
  always persist the response's `refreshToken`, never assume the old one
  remains valid."**
- **Error / failure path:** OBN's `fill_auth_fields()` + `api_error()` set
  `r.ok = false` whenever the returned `accessToken` is empty, and surface
  the server's `message` or `error` JSON field (falling back to
  `"http <status>"`). `hydrate_session()` then logs
  `"cloud: refresh failed: <message>"` and **keeps the existing session** —
  it does not log the user out on a single refresh failure, leaving the
  problem to the next refresh attempt or to eventual access-token expiry.
- **Specific HTTP status codes for expired / reused / revoked refresh
  tokens: NOT DETERMINED FROM SOURCE.** None of OBN, HA, BambuStudio, or the
  secondary community libs documents concrete status codes or error
  `code` integers for the refresh endpoint's failure modes (analogous to the
  `{code:1}`/`{code:2}` codes HA documents for the verification-code login
  flow in `_get_authentication_token_with_verification_code()`). This is a
  real gap: a relay implementing this contract must empirically characterise
  4xx/5xx behaviour for expired, reused, and revoked refresh tokens against
  the live API; it cannot be lifted from any existing source.
- **Reuse invalidation:** not directly confirmed. The combination of "server
  may rotate" + "client must persist the response's refreshToken" is the
  standard defensive pattern for servers that *do* invalidate on reuse, but
  no source proves Bambu's server actually enforces one-time-use. A relay
  should assume rotation-invalidation may happen and always replace — never
  duplicate — the stored refresh token after each successful refresh.

## Stability / maintenance of the community contract

**`ClusterM/open-bamboo-networking` (the contract's primary source):**

- 433 stars, default branch `master`, last push **2026-07-15** (the day
  before this research). Daily commit cadence in July 2026; multiple
  commits per day on 2026-07-14 and 2026-07-15.
- The auth surface specifically has been broken by Bambu and re-fixed by
  the maintainer more than once in 2026:
  - 2026-04-19 `d8b79ed` "Cloud auth"
  - 2026-04-20 `9d4d702` "Web auth fixed"
  - 2026-06-29 `0b067b2` "Feature: MQTT Fallbacks + Alternative Auth (#29)"
    — PR #29 body: "A few of the existing stubs were pointed at the wrong
    endpoints or parsing fields by the wrong names."
  - 2026-07-11 `f94fde3` "Remove dead session persistence duplicate from
    cloud_auth"
  - 2026-07-15 [issue #67](https://github.com/ClusterM/open-bamboo-networking/issues/67)
    open — pre-emptive hardening against Cloudflare JA4H fingerprinting.
- This is the opposite of a stale one-off reverse. It is an actively-tracked
  contract with a maintainer who has demonstrated repeated, rapid repair
  when Bambu changes things. That said, "actively maintained" is exactly
  the maintenance burden the relay would inherit: any deploy pinning this
  contract must monitor OBN commits and be ready to ship header/body
  changes on short notice.

**`greghesp/ha-bambulab` (the integration that *doesn't* ship refresh):**

- 2,252 stars, last push 2026-06-28. Also actively maintained, but its
  maintainer (`AdrianGarside`) explicitly chose **not** to implement
  refresh. In
  [issue #565](https://github.com/greghesp/ha-bambulab/issues/565)
  (2024-11-16): "/v1/user-service/user/refreshtoken is the API although
  apparently it's behavior just changed so it no longer works when passed
  previously valid data." Issue was stale-closed on 2025-10-26 without a
  refresh implementation landing. The HA integration instead prompts the
  user to re-authenticate when the (≈1-year) token expires.
- This is independent historical evidence that **Bambu has broken the
  refresh endpoint's behaviour before** (Nov 2024) — exactly the kind of
  silent contract change a relay consumer must be braced for.

## Gaps (stated explicitly, not guessed)

- **Refresh failure status codes / error `code` integers** for expired,
  reused, and revoked refresh tokens are not documented in any source
  consulted. Must be characterised empirically against the live API.
- **Post-refresh `expiresIn` value** is not observed in any source (only
  the at-issue value of 31 536 000 s is confirmed). Do not assume a fresh
  year is granted on every refresh.
- **Definitive one-time-use / reuse-invalidation behaviour** is not proven.
  The defensive client pattern (always persist the returned
  `refreshToken`) is the only safe assumption.
- **Whether `/v1/user-service/user/refreshtoken` is reachable from a
  non-Chinese, non-mimicking client in all account regions** is confirmed
  only for the global host `api.bambulab.com`. China (`api.bambulab.cn`)
  uses the same path per `cloud_api_host_for()`, but no source provides a
  captured China-region refresh transaction.
- **Rate limits** on the refresh endpoint are not documented in any
  source.

## Relationship to `BAMBU_AUTH_ROTATION_RESEARCH.md`

The first-party-only research doc concluded "no publicly documented
supported headless refresh contract" and recommended either LAN-only +
Developer Mode or manual renewal. Those conclusions **remain correct at the
supported-contract level**: everything in this document is reverse-engineered,
not Bambu-endorsed, and Bambu's May 2026 cloud-policy statement
([cited in the first doc](https://blog.bambulab.com/setting-the-record-straight-on-cloud-access-and-community/))
explicitly says impersonating an official client is not allowed and that
the cloud is a private service. The contract extracted here is the same
"undocumented, Bambu-can-break-it" contract the first doc warned about; the
only new information is its **precise wire shape**, taken from code that
runs it daily.

The project decision recorded in the first doc (retain Cloud/Handy via
official-login-mediated renewal) is not invalidated by this research. If a
future decision is made to add reverse-engineered refresh as a fallback,
the contract above is the implementation reference, and
`ClusterM/open-bamboo-networking` is the upstream to track for breakage.

## Primary sources

- `ClusterM`, [`open-bamboo-networking`](https://github.com/ClusterM/open-bamboo-networking) — open-source drop-in replacement for Bambu Studio's proprietary `bambu_networking` plug-in. Refresh implementation: [`src/cloud_auth.cpp`](https://github.com/ClusterM/open-bamboo-networking/blob/master/src/cloud_auth.cpp) (commit [`f94fde3`](https://github.com/ClusterM/open-bamboo-networking/commit/f94fde3b3b72f44d39d0ce154278d678e7681f03), 2026-07-11). Runtime wiring: [`src/agent.cpp`](https://github.com/ClusterM/open-bamboo-networking/blob/master/src/agent.cpp) `Agent::hydrate_session()` (commit [`3d58c81`](https://github.com/ClusterM/open-bamboo-networking/commit/3d58c81b1a7887c42b8c61e9256de23d8fe6087c), 2026-07-15). HTTP defaults: [`src/http_client.cpp`](https://github.com/ClusterM/open-bamboo-networking/blob/master/src/http_client.cpp). Regional hosts: [`src/config.cpp`](https://github.com/ClusterM/open-bamboo-networking/blob/master/src/config.cpp). Wire-contract narrative and MITM/probe evidence: [`NETWORK_PLUGIN.md`](https://github.com/ClusterM/open-bamboo-networking/blob/master/NETWORK_PLUGIN.md) (commit [`3bc0014`](https://github.com/ClusterM/open-bamboo-networking/commit/3bc001404d252d87a85620e1896fd2f072c14f3c), 2026-07-15) §§6.5, 6.5.1, 6.8.1.1, 6.9, 6.10. ABI coverage / per-endpoint enforcement: [`STATUS.md`](https://github.com/ClusterM/open-bamboo-networking/blob/master/STATUS.md). Pinned at master HEAD `3d58c81` (2026-07-15). Pre-emptive identity-header hardening: [issue #67](https://github.com/ClusterM/open-bamboo-networking/issues/67) (2026-07-15).
- `greghesp`, [`ha-bambulab`](https://github.com/greghesp/ha-bambulab) — canonical Home Assistant `bambu_lab` integration (2,252 stars). Cloud-auth source: [`custom_components/bambu_lab/pybambu/bambu_cloud.py`](https://github.com/greghesp/ha-bambulab/blob/main/custom_components/bambu_lab/pybambu/bambu_cloud.py) (commit [`6d7b73e`](https://github.com/greghesp/ha-bambulab/commit/6d7b73ef94), 2025-12-07). URLs: [`custom_components/bambu_lab/pybambu/const.py`](https://github.com/greghesp/ha-bambulab/blob/main/custom_components/bambu_lab/pybambu/const.py). Region swap: `custom_components/bambu_lab/pybambu/utils.py` `get_Url()`. Endpoint-name confirmation + maintainer history of refresh breakage: [issue #565](https://github.com/greghesp/ha-bambulab/issues/565) (`[Feature] RefreshToken usage`, 2024-07-23 → 2025-10-26). Pinned at `main` HEAD `f7e5290`.
- `bambulab`, [`BambuStudio`](https://github.com/bambulab/BambuStudio) at commit [`ba049f6`](https://github.com/bambulab/BambuStudio/commit/ba049f6a2e08c3b6033660bb84da80c08722974b) — confirms the four-field token envelope at the `TokenResp` struct in `src/slic3r/GUI/HttpServer.cpp` (lines 119–129) and the login-info forwarding at lines 516–554. The local `/refresh_token` route at lines 431–475 is the Agora live-video URL refresh, not account-token refresh (already established in `BAMBU_AUTH_ROTATION_RESEARCH.md`).
- `torbenconto`, [`bambulabs_cloud_api`](https://github.com/torbenconto/bambulabs_cloud_api) — Go implementation; corroborates that the email/password `/login` endpoint accepts a non-mimicking `User-Agent: BambulabsCloudAPI/1.0`. `cloud.go` `Login()`/`userAgent`. Does not implement refresh.
- `zhaobenny`, [`bambulab-cloud-py`](https://github.com/zhaobenny/bambulab-cloud-py) — Python implementation; corroborates the Global/China two-host split (`models.py` `Region.base_url()`) and that the login response is consumed via a single `access_token` field. Does not implement refresh.
- `BambuTools`, [`bambulabs_api`](https://github.com/BambuTools/bambulabs_api) and `mattcar15`, [`bambu-connect`](https://github.com/mattcar15/bambu-connect) — checked for refresh; both are LAN/access-code only and do not touch cloud account tokens.

## Canonical stock X-BBL header block (for the full-identity implementation path)

Research date: 2026-07-16. Scope: the exact header **names, stock values, and
wire order** the proprietary `bambu_network_agent` plug-in sends to
`api.bambulab.com`, for the project's decision to send the full stock
identity block from day one (Cloudflare JA4H fingerprints header set +
order; see [ClusterM/open-bamboo-networking #67](https://github.com/ClusterM/open-bamboo-networking/pull/67)).

> Extraction only. No design. Every claim carries a repo / file / line /
> commit / URL citation, matching the discipline of the rest of this doc.

### TL;DR verdict

**Stock wire order IS pinned from source: yes — for the core 11-header
fingerprint block, independently and from a *merged* source.**

- The relative order of the 11 `X-BBL-*`/`User-Agent` fingerprint headers is
  documented from **MITM/SSLKEYLOGFILE captures of the stock plug-in** in
  `ClusterM/open-bamboo-networking` `NETWORK_PLUGIN.md` §6.10.1 (lines
  2713-2729, on **`master`**, commit
  [`3d58c81`](https://github.com/ClusterM/open-bamboo-networking/commit/3d58c81b1a7887c42b8c61e9256de23d8fe6087c)).
  This is a second, merged, capture-backed source — it is *not* dependent on
  the open PR.
- The full **executable** wire order (including the position of
  `X-BBL-Client-ID` on device-scoped calls, the extra `X-BBL-Executable-Env`,
  and the trailing `accept`/`Authorization`/`Content-Type` transport headers)
  is encoded as the `kBblHeaderOrder[]` constant in `src/http_client.cpp` of
  **open PR [#67](https://github.com/ClusterM/open-bamboo-networking/pull/67)**
  (commit
  [`ac2374c`](https://github.com/ClusterM/open-bamboo-networking/commit/ac2374c137b3564194a986a2da9d928eef8c370e),
  branch `danielwoz:genuine-cloud-identity`). **PR #67 has NOT landed in
  `master`** (status Open, no reviews, 1 commit, as of the research date).

**Therefore:** a mitmproxy self-capture is **not** required to obtain the core
order — it is already pinned in the merged doc. A mitmproxy capture is only
worth doing to confirm the two PR-only refinements (the exact wire position of
`X-BBL-Client-ID`, and whether `X-BBL-Executable-Env` is really on the wire),
neither of which is enforced server-side today. See "Open questions /
mitmproxy scope" below.

### Source A — the MITM-pinned fingerprint block (merged master)

`NETWORK_PLUGIN.md` §6.10.1 "Common cloud transport", lines 2713-2729. The doc
states verbatim: *"MITM/SSLKEYLOGFILE captures of the stock plugin show the
full fingerprint header set on every request:"* followed by this block, in
this order:

| # | Header | Stock value | Source |
|---|--------|-------------|--------|
| 1 | `User-Agent` | `bambu_network_agent/<agent-ver>` | `NETWORK_PLUGIN.md` §6.10.1 L2716 |
| 2 | `X-BBL-Client-Name` | `BambuStudio` | L2717 |
| 3 | `X-BBL-Client-Type` | `slicer` | L2718 |
| 4 | `X-BBL-Client-Version` | `<slicer-ver>` (e.g. `02.07.00.55`) | L2719 |
| 5 | `X-BBL-Device-ID` | `<slicer-machine-uuid>` (machine-local UUID, not printer serial) | L2720, L2729 |
| 6 | `X-BBL-Language` | `en-US` (UI locale, BCP-47) | L2721 |
| 7 | `X-BBL-OS-Type` | `linux` / `windows` / `macos` | L2722 |
| 8 | `X-BBL-OS-Version` | OS build string (e.g. `10.0.26200`) | L2723, L2729 |
| 9 | `X-BBL-Agent-Version` | `<agent-ver>` (network-plugin build, e.g. `02.07.00.50`) | L2724 |
| 10 | `X-BBL-Executable-info` | literal `{}` | L2725, L2729 |
| 11 | `X-BBL-Agent-OS-Type` | same value as `X-BBL-OS-Type` | L2726 |

Plus, on **device-scoped endpoints only** (firmware query, device metadata —
*not* on account-scoped endpoints like auth/profile/presets):

| Header | Stock value | Source |
|--------|-------------|--------|
| `X-BBL-Client-ID` | `slicer:{user_id}:{4-char-suffix}` (`user_id` = numeric `uidStr`; suffix = "random or session-derived nonce, exact derivation unconfirmed"; not required for the API to accept the call) | L2739-2745 |

Citation: [`NETWORK_PLUGIN.md` §6.10.1](https://github.com/ClusterM/open-bamboo-networking/blob/master/NETWORK_PLUGIN.md)
(master, `3d58c81`). The doc's own provenance statement (§0) marks capture
claims as "MITM captures of the stock `libbambu_networking.so` against
`api.bambulab.com`" — no binary disassembly. The same section independently
confirms that **none** of these `X-BBL-*` headers (nor the custom User-Agent)
are required for *most* endpoints (auth, profile, presets, bind,
project/upload) — they are "analytics only" there; the only value-enforced
exception is `POST /v1/user-service/my/task`, which hard-requires
`X-BBL-Client-Name: BambuStudio` + a matching `X-BBL-OS-Type` (L2733-2737).

### Source B — the executable wire order (open PR #67)

PR [#67](https://github.com/ClusterM/open-bamboo-networking/pull/67)
(`cloud: present the genuine stock identity on every api.bambulab.com call`,
opened 2026-07-15 by `danielwoz`, **Open / unmerged / 0 reviews**, commit
[`ac2374c`](https://github.com/ClusterM/open-bamboo-networking/commit/ac2374c137b3564194a986a2da9d928eef8c370e))
adds a single ordered constant in
[`src/http_client.cpp`](https://github.com/ClusterM/open-bamboo-networking/blob/genuine-cloud-identity/src/http_client.cpp)
(`perform()`, just below the `curl_easy_init` block) that libcurl emits
headers in **before** anything else:

```c
// Stock bambu_network_agent wire order for the identity headers.
static const char* const kBblHeaderOrder[] = {
    "User-Agent",         "X-BBL-Client-ID",    "X-BBL-Client-Name",
    "X-BBL-Client-Type",  "X-BBL-Client-Version", "X-BBL-Device-ID",
    "X-BBL-Language",     "X-BBL-OS-Type",      "X-BBL-OS-Version",
    "X-BBL-Agent-Version","X-BBL-Executable-info", "X-BBL-Agent-OS-Type",
    "X-BBL-Executable-Env", "accept",           "Authorization",
    "Content-Type",
};
```

Numbered wire order as the PR sends it:

1. `User-Agent`
2. `X-BBL-Client-ID` *(only present on device-scoped calls; the PR's
   `identity_headers(include_client_id=true)` path adds it —
   `include/obn/identity_headers.hpp`)*
3. `X-BBL-Client-Name`
4. `X-BBL-Client-Type`
5. `X-BBL-Client-Version`
6. `X-BBL-Device-ID`
7. `X-BBL-Language`
8. `X-BBL-OS-Type`
9. `X-BBL-OS-Version`
10. `X-BBL-Agent-Version`
11. `X-BBL-Executable-info`
12. `X-BBL-Agent-OS-Type`
13. `X-BBL-Executable-Env: false` *(PR-only; **not** in the §6.10.1 MITM block)*
14. `accept: application/json` *(lowercase, as the PR populates it)*
15. `Authorization: Bearer <token>` *(omitted on pre-auth calls — see below)*
16. `Content-Type: application/json` *(omitted on GETs that the stock agent
    sends without it — e.g. `slicer/setting`, filament list/config)*

The block is built by `obn::bbl::identity_headers(access_token, user_id,
include_client_id, with_content_type)` in the new
[`include/obn/identity_headers.hpp`](https://github.com/ClusterM/open-bamboo-networking/blob/genuine-cloud-identity/include/obn/identity_headers.hpp),
then emitted in the `kBblHeaderOrder` order by `http::perform()`. The header
file's own header comment states the values reproduce "the stock
BambuStudio agent" and that "http::perform() emits recognised headers in the
stock wire order".

### Header-by-header: name | stock value / variable | citation

| Header | Stock value (or variable) | Citation |
|--------|----------------------------|----------|
| `User-Agent` | `bambu_network_agent/<agent-ver>` — `<agent-ver>` from the release's plugin build, **not** the host slicer's version unless `OBN_ALLOW_VERSION_OVERRIDES` is set | `identity_headers.hpp` (`ua_product + "/" + agent_ver`, `BBL_UA_PRODUCT` env default `bambu_network_agent`); version table `slicer_plugin_versions.hpp` |
| `X-BBL-Client-ID` | `slicer:<uid>:<4 hex>`; suffix derived from an atomic counter in the PR (`%04x` of `ctr.fetch_add(0x9e37) & 0xffff`), matching the stock "varies per request" behaviour; **device-scoped calls only** | `identity_headers.hpp`; stock-shape note `NETWORK_PLUGIN.md` L2742-2745 |
| `X-BBL-Client-Name` | `BambuStudio` (env `BBL_CLIENT_NAME` / `config::client_name` override; this exact value is what `POST /my/task` requires) | `identity_headers.hpp`; L2734 |
| `X-BBL-Client-Type` | `slicer` | `identity_headers.hpp`; L2718 |
| `X-BBL-Client-Version` | slicer release tag, e.g. `02.07.00.55` (from `slicer_plugin_versions.hpp`, gated to the build's `ABI_VERSION`) | `slicer_plugin_versions.hpp` row for `0x020700`; L2719 |
| `X-BBL-Device-ID` | machine-derived RFC-4122 v5 UUID (PR derives from `/etc/machine-id` / Windows `MachineGuid` / macOS `gethostuuid`); stock sends a random UUID saved to disk by the slicer | `identity_headers.hpp` `detail::machine_uuid()`; L2720/L2729 |
| `X-BBL-Language` | UI locale BCP-47 (`en-US`); detected from `LANG`/`LC_*` (POSIX) or `GetUserDefaultLocaleName` (Windows) | `identity_headers.hpp` `detail::detect_language()`; L2721 |
| `X-BBL-OS-Type` | `windows` / `linux` / `mac` (PR) — note §6.10.1 captures show `linux`/`windows`/`macos`; PR uses `mac` for Apple with a TODO ("confirm mac/darwin") | `identity_headers.hpp` `default_os_type()`; L2722 |
| `X-BBL-OS-Version` | Windows `major.minor.build` via `RtlGetVersion` (e.g. `10.0.26200`); Linux three-digit kernel (e.g. `5.15.0`); macOS `14.5.0` placeholder (TODO) | `identity_headers.hpp` `detect_os_version()`; L2723/L2729 |
| `X-BBL-Agent-Version` | plugin build, e.g. `02.07.00.50` (same source as the `User-Agent` version half) | `slicer_plugin_versions.hpp`; L2724 |
| `X-BBL-Executable-info` | literal `{}` | `identity_headers.hpp` (`BBL_EXEC_INFO` default `"{}"`); L2725/L2729 |
| `X-BBL-Agent-OS-Type` | same value as `X-BBL-OS-Type` | `identity_headers.hpp` (set to `os_type`); L2726 |
| `X-BBL-Executable-Env` | `false` | `identity_headers.hpp` (hard-coded) — **PR-only, not in §6.10.1 capture** |
| `accept` | `application/json` (lowercase in the PR's map) | `identity_headers.hpp` |
| `Authorization` | `Bearer <accessToken>` — **omitted on pre-auth calls** (ticket exchange, refresh, profile, app-cert fetch) | `identity_headers.hpp` (added only when `!access_token.empty()`); PR `cloud_auth.cpp` `refresh_token()` passes empty token |
| `Content-Type` | `application/json` on POSTs; **omitted on the GETs the stock agent sends bare** (`slicer/setting`, filament list/config, app-cert) | `identity_headers.hpp` `with_content_type` flag; PR `cloud_presets.cpp` / `cloud_filament.cpp` pass `false` |

**Slicer ↔ plugin version pairs** (the two version-bearing headers must come
from a real release; `slicer_plugin_versions.hpp` table, gated to
`ABI_VERSION`):

| Release line | `X-BBL-Client-Version` (slicer) | `X-BBL-Agent-Version` + UA (plugin) |
|---|---|---|
| `02.07.00` | `02.07.00.55` | `02.07.00.50` |
| `02.07.01` | `02.07.01.62` | `02.07.01.51` |
| `02.08.00` (newest/default `#else`) | `02.08.00.50` | `02.08.00.51` |

Source: `include/obn/slicer_plugin_versions.hpp` `kSlicerPluginVersions[]`
(PR #67). These match `BAMBU_NETWORK_AGENT_VERSION` vs `SLIC3R_VERSION` in
BambuStudio `version.inc` / `bambu_networking.hpp`.

### What the refresh call specifically sends (post-#67, pre-auth variant)

From PR `src/cloud_auth.cpp` `refresh_token()` (diff hunks at the commit
above): the refresh call is a **pre-auth** call, so it passes
`identity_headers(/*token*/"", /*uid*/"", /*include_client_id*/false,
/*with_content_type*/true)`. That means the on-wire set is the full identity
block **minus** `X-BBL-Client-ID` and **minus** `Authorization`, plus
`Content-Type: application/json`. In wire order:

```
User-Agent: bambu_network_agent/<agent-ver>
X-BBL-Client-Name: BambuStudio
X-BBL-Client-Type: slicer
X-BBL-Client-Version: <slicer-ver>
X-BBL-Device-ID: <uuid>
X-BBL-Language: en-US
X-BBL-OS-Type: <os>
X-BBL-OS-Version: <os-build>
X-BBL-Agent-Version: <agent-ver>
X-BBL-Executable-info: {}
X-BBL-Agent-OS-Type: <os>
X-BBL-Executable-Env: false
accept: application/json
Content-Type: application/json
```

(Ticket exchange `login_with_ticket()` and `get_profile()` use the same
pre-auth variant; `fetch_device_cert()` additionally drops `Content-Type`.)

### Discrepancies between the three sources

Comparing **Source A** (`NETWORK_PLUGIN.md` §6.10.1 MITM, merged master),
**Source B** (PR #67 `kBblHeaderOrder`, open), and **Source C**
(`greghesp/ha-bambulab` `bambu_cloud.py` `_get_headers()`, `main` HEAD
`f7e5290`):

- **Core 11-header order.** Source A and Source B agree exactly on the
  relative order of `User-Agent` → `Client-Name` → `Client-Type` →
  `Client-Version` → `Device-ID` → `Language` → `OS-Type` → `OS-Version` →
  `Agent-Version` → `Executable-info` → `Agent-OS-Type`. Source C
  (HA) agrees on the same relative order for the ~9 headers it sends. **No
  source contradicts the order of the core block.**
- **`X-BBL-Client-ID` position.** Source A documents the *value* and that it
  appears on device-scoped endpoints, but does **not** pin its wire position
  within the block. Source B places it at **#2** (immediately after
  `User-Agent`). Source C omits it entirely (commented out in `_get_headers()`
  with "need to work out what an appropriate ID is"). → The #2 position is
  the PR contributor's reconstruction, not MITM-confirmed.
- **`X-BBL-Executable-Env`.** Present in Source B (`false`, #13). **Absent**
  from Source A's capture block and from Source C. → PR-only; unconfirmed by
  the merged MITM doc.
- **`X-BBL-Device-ID`.** Present in A and B. Source C has it commented out
  ("Example: X-BBL-Device-ID: 370f9f43-…") — HA never sends it.
- **Identity label.** Source A/B default to `X-BBL-Client-Name: BambuStudio`
  (the value `/my/task` enforces). Source C labels itself `OrcaSlicer` with
  `X-BBL-Client-Version: 01.09.05.51` and `User-Agent:
  bambu_network_agent/01.09.05.01` — the server accepts either on
  non-`/my/task` endpoints, but HA's set is **not** a faithful Studio
  masquerade and would 403 on `/my/task`.
- **`Accept-Encoding`.** Source C adds `Accept-Encoding: gzip, deflate`
  explicitly; Source B delegates that to libcurl
  (`CURLOPT_ACCEPT_ENCODING=""`, auto-negotiate). Source A does not list it
  (transport-level, outside the fingerprint set).

### Open questions / mitmproxy scope (stated, not guessed)

- **`X-BBL-Client-ID` wire position** (#2 per PR) is the single ordering claim
  not corroborated by the merged MITM doc. It is not server-enforced today, so
  it does not block a bare implementation, but a masquerade-critical deploy
  should confirm it with one mitmproxy capture of a device-scoped call (e.g.
  `GET /v1/iot-service/api/user/print?force=true`) from stock Studio.
- **`X-BBL-Executable-Env`** presence/value is PR-only; confirm or drop via
  the same capture.
- **macOS identity** (`X-BBL-OS-Type` value `mac` vs `macos`; OS-Version
  format) is an explicit TODO in the PR (`identity_headers.hpp`) and is
  capture-dependent; not relevant for the router (Linux) relay but noted for
  completeness.

Neither gap blocks the core order: the 11-header fingerprint block and its
order are pinned from a merged, capture-backed source.

### Has PR #67 landed in OBN code?

**No.** As of the research date (2026-07-16), PR
[#67](https://github.com/ClusterM/open-bamboo-networking/pull/67) is **Open**,
on branch `danielwoz:genuine-cloud-identity`, single commit
[`ac2374c`](https://github.com/ClusterM/open-bamboo-networking/commit/ac2374c137b3564194a986a2da9d928eef8c370e),
with **zero reviews** and not merged into `master` (`master` HEAD remains
[`3d58c81`](https://github.com/ClusterM/open-bamboo-networking/commit/3d58c81b1a7887c42b8c61e9256de23d8fe6087c),
which does **not** contain `identity_headers.hpp`, `slicer_plugin_versions.hpp`,
or the `kBblHeaderOrder` constant). The stock identity block therefore exists
**only as issue/PR specification + the merged §6.10.1 narrative** — it is not
yet live in OBN's shipped code path. OBN's current `master` refresh path still
sends the bare three-header set documented in the main body of this file
above.
