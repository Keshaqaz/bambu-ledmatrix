# Bambu Relay cloud-token runbook

Cloud/Handy mode is retained. Bambu does not publish a supported headless token
refresh API, so the access token originally comes from an official login. Two
renewal modes are now supported:

- **Automatic (default when bootstrapped):** if a `refreshToken` is present in
  the config, the cron check calls the (reverse-engineered, unsupported)
  `refreshtoken` endpoint itself on confirmed expiry and installs the new token.
  See "Refresh automatically" below.
- **Manual fallback:** when no refresh token is bootstrapped, or auto-refresh
  fails, renewal stays an official-login action mediated by `install-cloud-token`.

The automated parts in both modes are validation, atomic installation, restart
verification, rollback, and expiry/`401` detection.

## Current deployment

Проверено 16 июля 2026 на OpenWrt-роутере `192.168.1.1`:

- `bambu-relay` запущен и подключён к `cloud_mqtt`;
- `/healthz` возвращает `{"ok": true}`;
- `check-cloud-token` подтверждает валидность текущего токена;
- `/tmp/bambu-relay-token.state` содержит `ok`;
- cron-проверка установлена на `17 */6 * * *`.

Этот раздел фиксирует результат развёртывания, а не гарантирует будущее
состояние. Для актуальной проверки используйте команды ниже.

## Router files

- `/root/bambu_relay_admin.py` — token validation and installation CLI (`0700`)
- `/usr/bin/bambu-relay-token-check` — transition-based syslog wrapper (`0755`)
- `/root/bambu_relay.conf` — relay configuration and token (`0600`)

Cron checks the token every six hours:

```cron
17 */6 * * * /usr/bin/bambu-relay-token-check
```

The wrapper logs only when the state changes to failed or recovers, so an
expired token does not flood syslog.

## Install a renewed token

Obtain the token through an official Bambu login. Prefer the complete login JSON:
it contains the server-supplied lifetime and enables the seven-day warning. If
the official flow exposes only the raw access token, the checker still detects
`401` every six hours, but it cannot predict expiry because the opaque token has
no lifetime metadata.

Do not put either form in a command argument, shell history, chat, or a
world-readable file. For a raw token, read it without echo and pipe it to the
router:

```sh
read -r -s -p 'Bambu cloud token: ' BAMBU_TOKEN; echo
printf '%s' "$BAMBU_TOKEN" |
  ssh root@192.168.1.1 /root/bambu_relay_admin.py install-cloud-token
unset BAMBU_TOKEN
```

The CLI accepts either a raw access token or the complete JSON login response.
When JSON contains `expiresIn`/`expires_in`, it records
`BAMBU_CLOUD_TOKEN_EXPIRES_AT` and starts warning seven days before expiry.

Installation performs these steps:

1. Validate the candidate token against the Bambu account-preference endpoint.
2. Replace only `BAMBU_CLOUD_TOKEN` in the existing config via same-directory
   atomic rename, enforcing mode `0600`.
3. Restart `bambu-relay`.
4. Wait for `/status` to report connected `cloud_mqtt`.
5. Restore the exact previous config and restart again if verification fails.

The token is never printed by the CLI.

## Refresh automatically

When `BAMBU_CLOUD_REFRESH_TOKEN` is set, the cron wrapper
(`bambu-relay-token-check`) attempts autonomous recovery: on confirmed rotation
needed (`check-cloud-token` exit `2`) it runs `refresh-cloud-token`, which POSTs
the refresh token to `https://api.bambulab.com/v1/user-service/user/refreshtoken`
with the stock `bambu_network_agent` identity header block, installs the returned
access token, persists the (possibly rotated) refresh token, and restarts +
verifies the relay. It never fires on a transient check failure (exit `3`), so a
network blip does not burn a refresh attempt.

Manually trigger a refresh (no official login needed):

```sh
ssh root@192.168.1.1 /root/bambu_relay_admin.py refresh-cloud-token
```

Expected success: `Cloud token refreshed and relay verified`. Exit `2` means the
refresh token itself is empty/invalid/expired — fall back to the manual install
flow above, which re-bootstraps the refresh token if the login JSON contains one.

The refresh contract is reverse-engineered and unsupported; Bambu has broken it
before and is tightening Cloudflare fingerprinting. Auto-refresh is an
optimization on top of the manual safety net, not a replacement: when Bambu
breaks the contract, the wrapper surfaces the failure as "needs rotation" and
you re-bootstrap via the official login. Contract provenance and the wire shape
live in `BAMBU_REFRESH_CONTRACT_RESEARCH.md`; track
`ClusterM/open-bamboo-networking` for upstream changes and bump the `X-BBL-*`
version values (env `BAMBU_AGENT_VERSION` / `BAMBU_CLIENT_VERSION`) when Bambu
ships a new plug-in build.

Ожидаемый успешный результат:

```text
Cloud token installed and relay verified
```

Если reconnect не подтвердился за 120 секунд, installer восстанавливает точное
предыдущее содержимое конфига, снова запускает relay и завершает работу с
ошибкой. Не заменяйте конфиг вручную после такого rollback — сначала выясните,
почему новый токен не прошёл проверку.

## Checks

Validate the current token without restarting anything:

```sh
ssh root@192.168.1.1 /root/bambu_relay_admin.py check-cloud-token
```

Коды завершения:

- `0` — токен действителен и не попал в окно предупреждения;
- `2` — нужна ротация: токен пуст, отклонён (`401/403`) или истекает в течение
  настроенного окна;
- `3` — проверка не состоялась из-за API, сети или неожиданного ответа; старый
  токен не заменять только на основании этого кода;
- `1` — локальная ошибка конфигурации/запуска.

Inspect the transition-based monitor:

```sh
ssh root@192.168.1.1 'cat /tmp/bambu-relay-token.state'
ssh root@192.168.1.1 'logread | grep -F bambu-relay-token | tail'
```

Verify the relay after rotation:

```sh
curl http://192.168.1.1:8787/healthz
```

`/healthz` proves that the relay process reached its HTTP server. The installer
also checks authenticated `/status` and cloud MQTT connectivity, which is the
stronger signal.

## Deployment/update

Take temporary backups on the router, copy with legacy SCP, enforce permissions,
and install the cron entry once:

```sh
ssh root@192.168.1.1 '
  backup=/tmp/bambu-relay-token-backup-$(date +%s)
  mkdir -p "$backup"
  [ ! -e /root/bambu_relay_admin.py ] || cp -p /root/bambu_relay_admin.py "$backup"/
  [ ! -e /usr/bin/bambu-relay-token-check ] || cp -p /usr/bin/bambu-relay-token-check "$backup"/
'
scp -O tools/bambu_relay/admin.py root@192.168.1.1:/root/bambu_relay_admin.py
scp -O tools/bambu_relay/openwrt/bambu-relay-token-check \
  root@192.168.1.1:/usr/bin/bambu-relay-token-check
ssh root@192.168.1.1 'chmod 700 /root/bambu_relay_admin.py && \
  chmod 755 /usr/bin/bambu-relay-token-check && \
  (grep -Fqx "17 */6 * * * /usr/bin/bambu-relay-token-check" /etc/crontabs/root || \
   echo "17 */6 * * * /usr/bin/bambu-relay-token-check" >> /etc/crontabs/root) && \
  /etc/init.d/cron restart'
```

This router requires `scp -O`: it provides legacy SCP but no SFTP server.

## Security boundary

- No Bambu account password is stored on the router. 2FA/email-code login is not
  automated (and cannot be replayed headlessly), so the account password alone
  would not suffice even if stored.
- A `refreshToken` **is** stored on the router (`0600` config) when auto-refresh
  is bootstrapped. It is obtained once from an official login and rotated by the
  relay thereafter. Treat it as a session credential: compromise of the router
  yields a refreshable cloud session until revoked.
- The undocumented `refreshtoken` endpoint **is** automated, and the request
  carries the stock `bambu_network_agent` identity headers. This is reverse-
  engineered integration, not a Bambu-supported contract.
- Access-token acquisition is still ultimately rooted in an explicit official
  login; auto-refresh only extends the session that login established.

> **Decision record (2026-07-16, supersedes the prior boundary).** The earlier
> boundary here forbade storing a refresh token and automating any refresh
> endpoint, citing Bambu's May 2026 statement that impersonating an official
> client is not allowed. The project has since deliberately accepted that risk
> to make rotation autonomous: the refresh transaction is the same undocumented,
> Bambu-can-break-it-without-notice contract warned about in
> `BAMBU_AUTH_ROTATION_RESEARCH.md`, now extracted in
> `BAMBU_REFRESH_CONTRACT_RESEARCH.md`. The mitigation is operational, not
> contractual: auto-refresh degrades cleanly to the manual official-login flow
> on any failure, so breakage never leaves the relay worse off than the manual
> baseline. If a supported Bambu partner contract becomes available later,
> prefer it over this path.
