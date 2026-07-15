# Bambu Relay: access-token lifecycle and rotation options

Research date: 2026-07-16  
Scope: Bambu Lab first-party sources only (official blog, official security white paper, and the official `bambulab/BambuStudio` repository).

> Project decision (2026-07-16): retain Cloud/Handy. The LAN-only option below
> remains the technically preferred design when cloud features are expendable,
> but it was explicitly rejected for the current deployment. The implemented
> path is official-login-mediated renewal plus atomic installation, rollback,
> and monitoring.

## Executive conclusion

For a headless relay, there is no publicly documented, supported Bambu Cloud API contract for refreshing or rotating the account access token. Bambu Studio clearly receives an access token, a refresh token, and two server-supplied lifetimes, but the actual account/session handling is delegated to Bambu's proprietary networking plug-in. Therefore, implementing unattended refresh by reverse-engineering the login service, saving an account password, or scraping a browser cookie would be an unsupported integration and could break without notice.

The most reliable way to remove this rotation problem is to stop using a Bambu Cloud account token for the relay and connect to the printer locally with **LAN Only + Developer Mode + the printer Access Code**. Bambu explicitly says LAN mode does not connect the printer to its cloud, and Developer Mode leaves the MQTT channel available to custom solutions. This trades token expiry and cloud dependence for local-network security responsibility.

## What the official sources establish

### Cloud account tokens are expiring session credentials

The current Bambu Studio source models a login/token response with four independent fields: `accessToken`, `refreshToken`, `expiresIn`, and `refreshExpiresIn` ([official source, pinned commit](https://github.com/bambulab/BambuStudio/blob/ba049f6a2e08c3b6033660bb84da80c08722974b/src/slic3r/GUI/HttpServer.cpp#L119-L129)). When Studio exchanges a login ticket, it forwards both tokens and both lifetimes into its login state ([official source](https://github.com/bambulab/BambuStudio/blob/ba049f6a2e08c3b6033660bb84da80c08722974b/src/slic3r/GUI/HttpServer.cpp#L520-L547)).

Consequences:

- Token lifetime must be treated as server-supplied, not as a hard-coded interval. The official open source does not define a fixed number of days.
- The presence of `refreshToken` proves that Bambu's own client has refresh-capable session data; it does **not** by itself expose a supported third-party refresh endpoint.
- Studio hands login data to `bambu_network_change_user`, loaded dynamically from the networking plug-in ([official source](https://github.com/bambulab/BambuStudio/blob/ba049f6a2e08c3b6033660bb84da80c08722974b/src/slic3r/Utils/NetworkAgent.cpp#L285-L330)). Bambu's repository describes that networking plug-in as non-free and optional ([official repository README](https://github.com/bambulab/BambuStudio#readme)). The public source therefore does not reveal the account refresh transaction or guarantee it as an API for unattended third-party clients.

One confusing detail should not be used as evidence of account-token refresh: Bambu Studio has a local HTTP route named `/refresh_token`, but its parameters are device/version/channel and it calls `refresh_agora_url`; this refreshes a live-video/Agora URL, not the user's Bambu Cloud access token ([official source](https://github.com/bambulab/BambuStudio/blob/ba049f6a2e08c3b6033660bb84da80c08722974b/src/slic3r/GUI/HttpServer.cpp#L431-L474)).

### No public supported headless refresh contract was found

As of the research date, Bambu's public Wiki/blog and the public portion of Bambu Studio do not publish all of the following as a stable third-party contract:

- an account-token refresh endpoint;
- its request schema, authentication requirements, and regional hosts;
- refresh-token reuse/rotation semantics;
- error/revocation semantics;
- a service-account or long-lived machine credential intended for a router daemon.

Bambu's authorization announcement instead directs integration partners to private technical documentation and `devpartner@bambulab.com` ([official announcement](https://blog.bambulab.com/firmware-update-introducing-new-authorization-control-system-2/)). The later update likewise says farm-management vendors should work directly with Bambu and lists Developer Mode as the custom-solution option ([official update](https://blog.bambulab.com/updates-and-third-party-integration-with-bambu-connect/)).

Bambu's May 2026 cloud-policy statement makes the risk more concrete: it calls the cloud a private service governed by the user agreement, says impersonating an official client is not allowed, and points advanced users to Bambu Connect, LAN Mode, and Developer Mode instead ([official statement](https://blog.bambulab.com/setting-the-record-straight-on-cloud-access-and-community/)). A relay should therefore not obtain automatic refresh by forging Bambu Studio identity metadata.

This is an absence-of-contract finding, not a claim that Bambu's own applications never refresh sessions internally.

## Official local alternative

### LAN Only

Bambu defines LAN mode as a mode in which the printer does not connect to the cloud and is normally accessed by client software on the same LAN ([official authorization announcement, FAQ](https://blog.bambulab.com/firmware-update-introducing-new-authorization-control-system-2/)). The official Bambu Studio UI tells users where to find the printer's Access Code under the printer's Network or LAN-only settings ([official source](https://github.com/bambulab/BambuStudio/blob/ba049f6a2e08c3b6033660bb84da80c08722974b/src/slic3r/GUI/ConnectPrinter.cpp#L133-L143)).

The Bambu Studio source connects locally with username `bblp` and the printer Access Code as the password ([official source](https://github.com/bambulab/BambuStudio/blob/ba049f6a2e08c3b6033660bb84da80c08722974b/src/slic3r/GUI/DeviceManager.cpp#L2330-L2339)). This is a printer credential, not the expiring Bambu Cloud account access token.

### Developer Mode

Bambu says optional Developer Mode leaves MQTT, live stream, and FTP available for advanced users, who assume responsibility for securing the LAN; Bambu also states these communication protocols are not officially supported interfaces ([official update](https://blog.bambulab.com/updates-and-third-party-integration-with-bambu-connect/)). Bambu's security white paper says Developer Mode makes printer MQTT exempt from Authorization Control so third-party software can continue to function ([official security white paper, p. 13](https://cdn1.bambulab.com/trust-center/file/bambulab-security-whitepaper-en.pdf)).

The January 2025 announcement explicitly named X1, P1, A1, and A1 Mini for Developer Mode. Availability and menu names should still be verified on the exact printer model and current firmware rather than inferred for every newer model.

### Standard LAN mode versus Developer Mode

Bambu distinguishes:

- **Standard LAN mode (default):** retains Bambu's authorization layer and is intended to work without an internet connection or user account through Bambu Connect.
- **Developer Mode:** leaves the underlying local channels open for custom clients.

Because this repository's relay is a custom MQTT client rather than Bambu Connect, Developer Mode is the relevant official operating choice. Standard LAN mode alone should not be assumed to support this relay.

## Recommendation for this relay

Preferred design when Cloud/Handy is not required:

1. Put the printer in LAN Only and enable Developer Mode, if supported by its model/firmware.
2. Configure the existing relay's LAN MQTT mode with printer IP, serial, and printer Access Code.
3. Give the printer a DHCP reservation/static lease.
4. Restrict router-to-printer traffic to the relay host and only the required printer services; do not expose printer MQTT to guest Wi-Fi, WAN, or untrusted VLANs.
5. Store the Access Code in a mode-`0600` router config and never in logs or health responses.
6. Monitor relay MQTT freshness and `/healthz`; alert on sustained disconnect instead of attempting account login automatically.
7. Treat Access Code replacement as an operator-driven security event: update the secret atomically, restart the relay, verify a fresh printer report, and roll back if verification fails.

This removes the periodic cloud-token rotation path entirely. It does mean Bambu Handy/cloud features are unavailable while the printer is in LAN Only, and local protocol compatibility remains subject to firmware changes because Bambu does not call Developer Mode protocols officially supported APIs.

Fallback if cloud mode must remain:

- Keep renewal manual and browser/official-client mediated.
- Alert before the observed expiry and immediately on MQTT authorization failure.
- Install the newly issued token atomically with a backup, restart, and verify both cloud API authorization and a fresh printer MQTT report.
- Do not persist the Bambu account password or email 2FA flow on the router.
- Do not label an implementation based on undocumented login endpoints as "automatic supported rotation"; it would be reverse-engineered automation with operational and account-security risk.

## Decision summary

| Option | Unattended rotation | Cloud/Handy retained | Support/compatibility position | Recommendation |
|---|---:|---:|---|---|
| Cloud token + undocumented refresh/login calls | Possible only by reverse engineering | Yes | No public stable contract found | Avoid |
| Cloud token + manual renewal + alerting | No | Yes | Lowest-risk cloud fallback | Use only if cloud mode is required |
| LAN Only + Developer Mode + Access Code | Rotation problem removed | No | Officially offered mode, protocols explicitly not supported | **Preferred for this read-only relay** |
| Standard LAN mode through Bambu Connect | Managed by official client | No cloud required | Official authorization path | Not directly usable by this custom MQTT relay |

## Primary sources

- Bambu Lab, [Updates and Third-Party Integration with Bambu Connect](https://blog.bambulab.com/updates-and-third-party-integration-with-bambu-connect/), 2025-01-20.
- Bambu Lab, [Firmware Update Introducing New Authorization Control System](https://blog.bambulab.com/firmware-update-introducing-new-authorization-control-system-2/), 2025-01-16, updated 2025-01-20.
- Bambu Lab, [Security White Paper 2025.09](https://cdn1.bambulab.com/trust-center/file/bambulab-security-whitepaper-en.pdf), p. 13.
- Bambu Lab, [Setting the record straight on Cloud Access and Community](https://blog.bambulab.com/setting-the-record-straight-on-cloud-access-and-community/), 2026-05-07.
- Bambu Lab, [`bambulab/BambuStudio`](https://github.com/bambulab/BambuStudio), source pinned at commit [`ba049f6a`](https://github.com/bambulab/BambuStudio/commit/ba049f6a2e08c3b6033660bb84da80c08722974b).
