#!/usr/bin/env python3
"""AT01 Bambu auth transport oracle using only fixed invalid credentials.

The probe intentionally prints exactly one verdict and never prints the request,
response body, cookies, operation id, or credentials.
"""

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import threading
import time
import urllib.error
import urllib.request


VERDICT_PASS = "TRANSPORT_PASS"
VERDICT_BLOCKED = "TRANSPORT_BLOCKED"
VERDICT_ERROR = "TRANSPORT_ERROR"

SYNTHETIC_ACCOUNT = "at01-transport-probe@example.invalid"
SYNTHETIC_PASSWORD = "AT01-synthetic-invalid-password"
LOGIN_PATH = "/v1/user-service/user/login"
MAXIMUM_RESPONSE_BYTES = 64 * 1024
CLASSIFIER_PREFIX_BYTES = 2048


def synthetic_login_body():
    return json.dumps(
        {
            "account": SYNTHETIC_ACCOUNT,
            "password": SYNTHETIC_PASSWORD,
            "apiError": "",
        },
        separators=(",", ":"),
    ).encode("utf-8")


def classify_http_response(status_code, content_type, bounded_body_prefix):
    prefix = bounded_body_prefix[:CLASSIFIER_PREFIX_BYTES].lower()
    if status_code == 403 and b"cloudflare" in prefix:
        return VERDICT_BLOCKED
    media_type = (content_type or "").split(";", 1)[0].strip().lower()
    if media_type != "application/json":
        return VERDICT_ERROR
    try:
        payload = json.loads(bounded_body_prefix[:CLASSIFIER_PREFIX_BYTES])
    except (UnicodeDecodeError, json.JSONDecodeError):
        return VERDICT_ERROR
    if (
        status_code in (400, 401)
        and isinstance(payload, dict)
        and isinstance(payload.get("code"), (int, str))
        and isinstance(payload.get("error"), str)
    ):
        return VERDICT_PASS
    data = payload.get("data", payload) if isinstance(payload, dict) else {}
    if status_code == 200 and isinstance(data, dict) and any(
        key in data
        for key in (
            "loginType",
            "accessToken",
            "access_token",
            "refreshToken",
            "refresh_token",
        )
    ):
        return VERDICT_PASS
    return VERDICT_ERROR


def classify_esp_status(status):
    error = status.get("error", "")
    state = status.get("state", "")
    if error == "cloudflare_blocked":
        return VERDICT_BLOCKED
    if error == "invalid_credentials":
        return (
            VERDICT_PASS
            if status.get("transportApplicationJson") is True
            else VERDICT_ERROR
        )
    if state in {
        "verification_required",
        "verification_required_email",
        "verification_required_sms",
        "verification_required_totp",
        "verification_email",
        "verification_sms",
        "verification_totp",
        "fetching_printers",
        "printer_selection_required",
        "connecting",
        "ready",
    }:
        return VERDICT_PASS
    if state == "recoverable_error":
        return VERDICT_ERROR
    return None


def parse_http_response(response):
    header_end = response.find(b"\r\n\r\n")
    if header_end < 0:
        return None
    lines = response[:header_end].split(b"\r\n")
    try:
        status_code = int(lines[0].split(b" ", 2)[1])
    except (IndexError, ValueError):
        return None
    content_type = ""
    for line in lines[1:]:
        name, separator, value = line.partition(b":")
        if separator and name.strip().lower() == b"content-type":
            content_type = value.strip().decode("ascii", "replace")
            break
    body_prefix = response[header_end + 4 : header_end + 4 + CLASSIFIER_PREFIX_BYTES]
    return status_code, content_type, body_prefix


def openssl_command():
    configured = os.environ.get("OPENSSL_BIN")
    candidates = [
        configured,
        "/opt/homebrew/opt/openssl@3/bin/openssl",
        "/opt/homebrew/bin/openssl",
        shutil.which("openssl"),
    ]
    for candidate in candidates:
        if candidate and pathlib.Path(candidate).is_file():
            return candidate
    return None


def desktop_request(timeout_seconds):
    body = synthetic_login_body()
    headers = [
        "POST %s HTTP/1.0" % LOGIN_PATH,
        "Host: api.bambulab.com",
        "User-Agent: bambu_network_agent/02.07.01.51",
        "X-BBL-Client-Name: BambuStudio",
        "X-BBL-Client-Type: slicer",
        "X-BBL-Client-Version: 02.07.01.62",
        "X-BBL-Device-ID: 00000000-0000-4000-8000-000000000001",
        "X-BBL-Language: en-US",
        "X-BBL-OS-Type: linux",
        "X-BBL-OS-Version: esp32",
        "X-BBL-Agent-Version: 02.07.01.51",
        "X-BBL-Executable-info: {}",
        "X-BBL-Agent-OS-Type: linux",
        "X-BBL-Executable-Env: false",
        "Accept: application/json",
        "Content-Type: application/json",
        "Content-Length: %d" % len(body),
        "Connection: close",
        "",
        "",
    ]
    request = "\r\n".join(headers).encode("ascii") + body
    executable = openssl_command()
    if not executable:
        return VERDICT_ERROR
    command = [
        executable,
        "s_client",
        "-quiet",
        "-connect",
        "api.bambulab.com:443",
        "-servername",
        "api.bambulab.com",
        "-verify_return_error",
        "-verify_hostname",
        "api.bambulab.com",
    ]
    try:
        process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
    except OSError:
        return VERDICT_ERROR
    timed_out = threading.Event()

    def kill_on_timeout():
        timed_out.set()
        process.kill()

    timer = threading.Timer(timeout_seconds, kill_on_timeout)
    timer.start()
    try:
        process.stdin.write(request)
        process.stdin.close()
        response = process.stdout.read(MAXIMUM_RESPONSE_BYTES + 1)
        return_code = process.wait()
    except (BrokenPipeError, OSError):
        process.kill()
        process.wait()
        return VERDICT_ERROR
    finally:
        timer.cancel()
    if timed_out.is_set() or return_code != 0 or len(response) > MAXIMUM_RESPONSE_BYTES:
        return VERDICT_ERROR
    parsed = parse_http_response(response)
    if not parsed:
        return VERDICT_ERROR
    return classify_http_response(*parsed)


def read_json(url, method="GET", body=None, timeout_seconds=5):
    request = urllib.request.Request(
        url,
        data=body,
        method=method,
        headers={"Accept": "application/json", "Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout_seconds) as response:
            if response.status not in (200, 202):
                return None
            raw = response.read(4097)
    except (urllib.error.URLError, TimeoutError, OSError):
        return None
    if len(raw) > 4096:
        return None
    try:
        return json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None


def esp_status_is_clean(status):
    return bool(
        status
        and status.get("state") == "signed_out"
        and not status.get("configured")
        and not status.get("candidatePending")
        and not status.get("operationId")
    )


def prepare_esp_baseline(base_url, timeout_seconds):
    status_url = base_url.rstrip("/") + "/api/bambu/auth/status"
    status = read_json(status_url, timeout_seconds=min(timeout_seconds, 5))
    if esp_status_is_clean(status):
        return True
    if (
        not status
        or status.get("configured")
        or status.get("candidatePending")
        or status.get("operationId")
        or status.get("state") != "recoverable_error"
    ):
        return False
    accepted = read_json(
        base_url.rstrip("/") + "/api/bambu/logout",
        method="POST",
        body=b"{}",
        timeout_seconds=min(timeout_seconds, 5),
    )
    if not accepted or not accepted.get("accepted"):
        return False
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        status = read_json(status_url, timeout_seconds=min(timeout_seconds, 5))
        if esp_status_is_clean(status):
            return True
        if status and (status.get("configured") or status.get("candidatePending")):
            return False
        time.sleep(0.25)
    return False


def esp_request(base_url, timeout_seconds, profile=0):
    if not prepare_esp_baseline(base_url, min(timeout_seconds, 5)):
        return VERDICT_ERROR
    login_body = json.dumps(
        {
            "email": SYNTHETIC_ACCOUNT,
            "password": SYNTHETIC_PASSWORD,
            "region": "global",
            "transportProfile": profile,
        },
        separators=(",", ":"),
    ).encode("utf-8")
    accepted = read_json(
        base_url.rstrip("/") + "/api/bambu/auth/login",
        method="POST",
        body=login_body,
        # A direct ESP-TLS profile runs synchronously in the device handler and
        # can legitimately take longer than the normal asynchronous enqueue.
        timeout_seconds=min(timeout_seconds, 20),
    )
    if not accepted or not accepted.get("accepted"):
        return VERDICT_ERROR
    operation_id = accepted.get("operationId")
    if not isinstance(operation_id, str) or not operation_id:
        return VERDICT_ERROR
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        status = read_json(
            base_url.rstrip("/") + "/api/bambu/auth/status",
            timeout_seconds=min(5, max(1, deadline - time.monotonic())),
        )
        if status:
            status_operation_id = status.get("operationId", "")
            if status_operation_id and status_operation_id != operation_id:
                return VERDICT_ERROR
            verdict = classify_esp_status(status)
            if verdict:
                if status.get("transportProfile") != profile:
                    return VERDICT_ERROR
                return verdict
        time.sleep(0.25)
    return VERDICT_ERROR


def main():
    parser = argparse.ArgumentParser(description="Bambu AT01 transport oracle")
    subparsers = parser.add_subparsers(dest="target", required=True)
    desktop = subparsers.add_parser("desktop")
    desktop.add_argument("--timeout", type=float, default=20.0)
    esp = subparsers.add_parser("esp")
    esp.add_argument("--url", required=True)
    esp.add_argument("--timeout", type=float, default=25.0)
    esp.add_argument("--profile", type=int, choices=range(5), default=0)
    args = parser.parse_args()
    if args.target == "desktop":
        verdict = desktop_request(args.timeout)
    else:
        verdict = esp_request(args.url, args.timeout, args.profile)
    print(verdict)
    return {VERDICT_PASS: 0, VERDICT_BLOCKED: 2, VERDICT_ERROR: 3}[verdict]


if __name__ == "__main__":
    raise SystemExit(main())
