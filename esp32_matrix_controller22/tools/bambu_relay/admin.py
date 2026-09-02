#!/usr/bin/env python3
"""Safe operational commands for the OpenWrt Bambu relay."""

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path


DEFAULT_AUTH_CHECK_URL = "https://api.bambulab.com/v1/design-user-service/my/preference"
DEFAULT_REFRESH_URL = "https://api.bambulab.com/v1/user-service/user/refreshtoken"


# Stock bambu_network_agent identity block (SYNCED ARTIFACT). Reverse-
# engineered, unsupported; Bambu can break it without notice. Provenance and
# wire order: BAMBU_REFRESH_CONTRACT_RESEARCH.md / OBN NETWORK_PLUGIN.md S6.10.1.
# Track ClusterM/open-bamboo-networking and bump the *_VERSION env overrides
# when Bambu ships a new plug-in build.
BAMBU_AGENT_VERSION = os.environ.get("BAMBU_AGENT_VERSION", "02.07.01.51")
BAMBU_CLIENT_VERSION = os.environ.get("BAMBU_CLIENT_VERSION", "02.07.01.62")
BAMBU_DEVICE_ID = os.environ.get(
    "BAMBU_DEVICE_ID", "b1c524d7-33c2-5f0a-9c2b-4a6e8f1d2a3b"
)
BAMBU_OS_TYPE = os.environ.get("BAMBU_OS_TYPE", "linux")


class AdminError(RuntimeError):
    def __init__(self, message: str, exit_code: int = 1):
        super().__init__(message)
        self.exit_code = exit_code


def read_assignments(contents: str) -> dict[str, str]:
    assignments = {}
    for raw_line in contents.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        name, value = line.split("=", 1)
        name = name.strip()
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            continue
        try:
            parsed = shlex.split(value, comments=True, posix=True)
        except ValueError:
            continue
        if len(parsed) == 1:
            assignments[name] = parsed[0]
        elif not parsed:
            assignments[name] = ""
    return assignments


def replace_assignment(contents: str, name: str, value: str) -> str:
    quoted_value = "'" + value.replace("'", "'\"'\"'") + "'"
    replacement = f"{name}={quoted_value}"
    pattern = re.compile(rf"^{re.escape(name)}=.*$", re.MULTILINE)
    if pattern.search(contents):
        return pattern.sub(replacement, contents, count=1)
    suffix = "" if contents.endswith("\n") or not contents else "\n"
    return f"{contents}{suffix}{replacement}\n"


def remove_assignment(contents: str, name: str) -> str:
    pattern = re.compile(rf"^{re.escape(name)}=.*(?:\n|$)", re.MULTILINE)
    return pattern.sub("", contents, count=1)


def parse_token_input(raw_input: str) -> tuple[str, int | None, str | None]:
    value = raw_input.strip()
    if not value:
        raise AdminError("read an empty token from stdin")
    if not value.startswith("{"):
        return value, None, None
    try:
        payload = json.loads(value)
    except json.JSONDecodeError as exc:
        raise AdminError("stdin looked like JSON but could not be parsed") from exc
    if not isinstance(payload, dict):
        raise AdminError("login response must be a JSON object")
    data = payload.get("data") if isinstance(payload.get("data"), dict) else payload
    token = data.get("accessToken") or data.get("access_token") or data.get("token")
    if not isinstance(token, str) or not token:
        raise AdminError("login response did not contain accessToken")
    refresh = data.get("refreshToken") or data.get("refresh_token")
    if not isinstance(refresh, str):
        refresh = None
    expires_at = _expires_at_from_lifetime(data.get("expiresIn", data.get("expires_in")))
    return token, expires_at, refresh


def _expires_at_from_lifetime(expires_in) -> int | None:
    if expires_in is None:
        return None
    try:
        lifetime = int(expires_in)
    except (TypeError, ValueError):
        return None
    if lifetime <= 0:
        return None
    return int(time.time()) + lifetime


def stock_identity_headers() -> list[tuple[str, str]]:
    """Stock bambu_network_agent identity headers in wire order, for a pre-auth
    POST such as token refresh. Authorization and X-BBL-Client-ID are
    intentionally omitted: refresh is a pre-auth, account-scoped call."""
    try:
        os_version = os.environ.get("BAMBU_OS_VERSION") or os.uname().release
    except AttributeError:
        os_version = "0.0"
    language = os.environ.get("BAMBU_LANGUAGE", "en-US")
    return [
        ("User-Agent", f"bambu_network_agent/{BAMBU_AGENT_VERSION}"),
        ("X-BBL-Client-Name", "BambuStudio"),
        ("X-BBL-Client-Type", "slicer"),
        ("X-BBL-Client-Version", BAMBU_CLIENT_VERSION),
        ("X-BBL-Device-ID", BAMBU_DEVICE_ID),
        ("X-BBL-Language", language),
        ("X-BBL-OS-Type", BAMBU_OS_TYPE),
        ("X-BBL-OS-Version", os_version),
        ("X-BBL-Agent-Version", BAMBU_AGENT_VERSION),
        ("X-BBL-Executable-info", "{}"),
        ("X-BBL-Agent-OS-Type", BAMBU_OS_TYPE),
        ("X-BBL-Executable-Env", "false"),
        ("accept", "application/json"),
        ("Content-Type", "application/json"),
    ]


def atomic_write(path: Path, contents: str, mode: int = 0o600) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        os.fchmod(fd, mode)
        with os.fdopen(fd, "w") as handle:
            handle.write(contents)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temp_name, path)
    except Exception:
        try:
            os.close(fd)
        except OSError:
            pass
        try:
            os.unlink(temp_name)
        except FileNotFoundError:
            pass
        raise


def get_json(url: str, headers: dict[str, str] | None = None) -> dict:
    request = urllib.request.Request(url, headers=headers or {})
    request.add_header("Accept", "application/json")
    request.add_header("User-Agent", "ledmatrix-bambu-relay-admin/1.0")
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            payload = json.loads(response.read().decode("utf-8", "ignore") or "{}")
    except urllib.error.HTTPError as exc:
        exit_code = 2 if exc.code in (401, 403) else 3
        raise AdminError(f"HTTP {exc.code} from validation endpoint", exit_code) from exc
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        raise AdminError(f"request failed: {exc}", 3) from exc
    if not isinstance(payload, dict):
        raise AdminError("validation endpoint returned a non-object response", 3)
    return payload


def validate_cloud_token(token: str, url: str) -> None:
    payload = get_json(url, {"Authorization": f"Bearer {token}"})
    if not any(payload.get(key) for key in ("uid", "userId", "user_id", "accountId", "account_id")):
        raise AdminError("validation response did not contain an account id", 3)


def post_refresh(url: str, refresh_token: str) -> dict:
    body = json.dumps({"refreshToken": refresh_token}).encode("utf-8")
    request = urllib.request.Request(url, data=body, method="POST")
    for name, value in stock_identity_headers():
        request.add_header(name, value)
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            raw = response.read().decode("utf-8", "ignore") or "{}"
        payload = json.loads(raw)
    except urllib.error.HTTPError as exc:
        exit_code = 2 if exc.code in (401, 403) else 3
        raise AdminError(f"HTTP {exc.code} from refresh endpoint", exit_code) from exc
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        raise AdminError(f"refresh request failed: {exc}", 3) from exc
    if not isinstance(payload, dict):
        raise AdminError("refresh endpoint returned a non-object response", 3)
    return payload.get("data") if isinstance(payload.get("data"), dict) else payload


def verify_relay(url: str, relay_key: str, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    last_error = "relay did not become ready"
    while True:
        try:
            headers = {"X-Relay-Key": relay_key} if relay_key else {}
            payload = get_json(url, headers)
            if payload.get("source") == "cloud_mqtt" and payload.get("connected") is True:
                return
            last_error = "relay is not connected to cloud MQTT"
        except AdminError as exc:
            last_error = str(exc)
        if time.monotonic() >= deadline:
            raise AdminError(last_error)
        time.sleep(min(0.5, max(0.0, deadline - time.monotonic())))


def apply_new_tokens(
    config_path: Path,
    service_path: str,
    auth_url: str,
    status_url: str,
    verify_timeout: float,
    access_token: str,
    expires_at: int | None,
    refresh_token: str | None = None,
) -> None:
    validate_cloud_token(access_token, auth_url)

    old_contents = config_path.read_text()
    new_contents = replace_assignment(old_contents, "BAMBU_CLOUD_TOKEN", access_token)
    if refresh_token:
        new_contents = replace_assignment(new_contents, "BAMBU_CLOUD_REFRESH_TOKEN", refresh_token)
    if expires_at is not None:
        new_contents = replace_assignment(
            new_contents, "BAMBU_CLOUD_TOKEN_EXPIRES_AT", str(expires_at)
        )
    else:
        new_contents = remove_assignment(new_contents, "BAMBU_CLOUD_TOKEN_EXPIRES_AT")
    atomic_write(config_path, new_contents, mode=0o600)

    try:
        subprocess.run([service_path, "restart"], check=True)
        relay_key = read_assignments(new_contents).get("BAMBU_RELAY_KEY", "")
        verify_relay(status_url, relay_key, verify_timeout)
    except Exception as install_error:
        atomic_write(config_path, old_contents, mode=0o600)
        try:
            subprocess.run([service_path, "restart"], check=True)
        except Exception as rollback_error:
            raise AdminError(
                f"new token failed and previous config was restored, "
                f"but relay restart failed: {rollback_error}"
            ) from install_error
        raise AdminError("new token failed; previous config restored and relay restarted") from install_error


def install_cloud_token() -> None:
    config_path = Path(os.environ.get("BAMBU_RELAY_CONFIG", "/root/bambu_relay.conf"))
    service_path = os.environ.get("BAMBU_RELAY_SERVICE", "/etc/init.d/bambu-relay")
    auth_url = os.environ.get("BAMBU_AUTH_CHECK_URL", DEFAULT_AUTH_CHECK_URL)
    status_url = os.environ.get("BAMBU_RELAY_STATUS_URL", "http://127.0.0.1:8787/status")
    verify_timeout = float(os.environ.get("BAMBU_RELAY_VERIFY_TIMEOUT", "120"))

    token, expires_at, refresh_token = parse_token_input(sys.stdin.read())
    apply_new_tokens(
        config_path, service_path, auth_url, status_url, verify_timeout,
        token, expires_at, refresh_token,
    )
    print("Cloud token installed and relay verified")


def refresh_cloud_token() -> None:
    config_path = Path(os.environ.get("BAMBU_RELAY_CONFIG", "/root/bambu_relay.conf"))
    service_path = os.environ.get("BAMBU_RELAY_SERVICE", "/etc/init.d/bambu-relay")
    auth_url = os.environ.get("BAMBU_AUTH_CHECK_URL", DEFAULT_AUTH_CHECK_URL)
    refresh_url = os.environ.get("BAMBU_REFRESH_URL", DEFAULT_REFRESH_URL)
    status_url = os.environ.get("BAMBU_RELAY_STATUS_URL", "http://127.0.0.1:8787/status")
    verify_timeout = float(os.environ.get("BAMBU_RELAY_VERIFY_TIMEOUT", "120"))

    refresh_token = read_assignments(config_path.read_text()).get("BAMBU_CLOUD_REFRESH_TOKEN", "")
    if not refresh_token:
        raise AdminError(
            "BAMBU_CLOUD_REFRESH_TOKEN is empty; bootstrap with an official login first", 2
        )

    payload = post_refresh(refresh_url, refresh_token)
    access_token = payload.get("accessToken") or payload.get("access_token")
    if not isinstance(access_token, str) or not access_token:
        raise AdminError("refresh response did not contain accessToken", 3)
    new_refresh = payload.get("refreshToken") or payload.get("refresh_token")
    if not isinstance(new_refresh, str) or not new_refresh:
        new_refresh = refresh_token
    expires_at = _expires_at_from_lifetime(payload.get("expiresIn", payload.get("expires_in")))

    apply_new_tokens(
        config_path, service_path, auth_url, status_url, verify_timeout,
        access_token, expires_at, new_refresh,
    )
    print("Cloud token refreshed and relay verified")


def check_cloud_token() -> None:
    config_path = Path(os.environ.get("BAMBU_RELAY_CONFIG", "/root/bambu_relay.conf"))
    auth_url = os.environ.get("BAMBU_AUTH_CHECK_URL", DEFAULT_AUTH_CHECK_URL)
    warn_seconds = int(os.environ.get("BAMBU_TOKEN_WARN_SECONDS", str(7 * 24 * 60 * 60)))
    assignments = read_assignments(config_path.read_text())
    token = assignments.get("BAMBU_CLOUD_TOKEN", "")
    if not token:
        raise AdminError("BAMBU_CLOUD_TOKEN is empty", 2)
    validate_cloud_token(token, auth_url)
    expires_at = assignments.get("BAMBU_CLOUD_TOKEN_EXPIRES_AT", "")
    if expires_at:
        try:
            remaining = int(expires_at) - int(time.time())
        except ValueError as exc:
            raise AdminError("BAMBU_CLOUD_TOKEN_EXPIRES_AT is invalid", 3) from exc
        if remaining <= warn_seconds:
            raise AdminError("cloud token expires within the configured warning window", 2)
    print("Cloud token is valid")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "command",
        choices=("install-cloud-token", "check-cloud-token", "refresh-cloud-token"),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.command == "install-cloud-token":
            install_cloud_token()
        elif args.command == "check-cloud-token":
            check_cloud_token()
        elif args.command == "refresh-cloud-token":
            refresh_cloud_token()
        return 0
    except AdminError as exc:
        print(f"bambu-relay-admin: {exc}", file=sys.stderr)
        return exc.exit_code
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        print(f"bambu-relay-admin: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
