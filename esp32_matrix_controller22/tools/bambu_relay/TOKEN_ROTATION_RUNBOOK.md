# Bambu Relay cloud-token runbook

Cloud/Handy mode is retained. Bambu does not publish a supported headless token
refresh API, so renewal stays official-login mediated. The automated part is
validation, atomic installation, restart verification, rollback, and expiry/
`401` detection.

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

## Checks

Validate the current token without restarting anything:

```sh
ssh root@192.168.1.1 /root/bambu_relay_admin.py check-cloud-token
```

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

- No Bambu account password or refresh token is stored on the router.
- No undocumented login/refresh endpoint is automated.
- Token acquisition remains an explicit official-login action.
- If fully unattended cloud renewal becomes a hard requirement, use a supported
  Bambu partner contract rather than impersonating Bambu Studio.
