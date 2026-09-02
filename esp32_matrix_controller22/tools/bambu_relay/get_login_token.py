#!/usr/bin/env python3
"""One-off helper: obtain a Bambu Cloud login JSON (accessToken + refreshToken
+ expiresIn) from email/password, so the output can be piped to:

    python3 get_login_token.py | \\
      ssh root@192.168.1.1 /root/bambu_relay_admin.py install-cloud-token

Handles the three login branches the official/HA clients handle: no-2FA,
email/SMS code (verifyCode), and TOTP (tfa). The login endpoints are
undocumented and sit behind Cloudflare; run this on your own machine where you
can answer a 2FA prompt interactively. Only the final token JSON is printed --
the password is read with getpass and never echoed or sent anywhere except the
Bambu login host. Contract reference: BAMBU_REFRESH_CONTRACT_RESEARCH.md.

Usage: python3 get_login_token.py [cn]   # 'cn' selects the China host
"""
import getpass
import json
import ssl
import sys
import urllib.error
import urllib.request

GLOBAL_HOST = "https://api.bambulab.com"
CN_HOST = "https://api.bambulab.cn"
TFA_HOST = "https://bambulab.com"

HEADERS = [
    ("User-Agent", "bambu_network_agent/02.07.01.51"),
    ("X-BBL-Client-Name", "BambuStudio"),
    ("X-BBL-Client-Type", "slicer"),
    ("X-BBL-Client-Version", "02.07.01.62"),
    ("X-BBL-Language", "en-US"),
    ("X-BBL-OS-Type", "macos"),
    ("X-BBL-OS-Version", "14.0"),
    ("X-BBL-Agent-Version", "02.07.01.51"),
    ("accept", "application/json"),
    ("Content-Type", "application/json"),
]


def post(host, path, body):
    request = urllib.request.Request(host + path, data=json.dumps(body).encode(), method="POST")
    for name, value in HEADERS:
        request.add_header(name, value)
    try:
        with urllib.request.urlopen(request, timeout=20, context=ssl.create_default_context()) as resp:
            raw = resp.read().decode("utf-8", "ignore") or "{}"
            return resp.status, json.loads(raw), resp.headers
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", "ignore") or "{}"
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError:
            parsed = {"_raw": raw}
        return exc.code, parsed, exc.headers


def extract_tokens(payload, headers):
    data = payload.get("data") if isinstance(payload.get("data"), dict) else payload
    if data.get("accessToken") or data.get("access_token"):
        return data
    # The TOTP branch may return the access token as a cookie named "token".
    for part in (headers.get("Set-Cookie", "") or "").split(";"):
        part = part.strip()
        if part.lower().startswith("token="):
            return {"accessToken": part.split("=", 1)[1]}
    return None


def main():
    host = CN_HOST if (len(sys.argv) > 1 and sys.argv[1] == "cn") else GLOBAL_HOST
    email = input("Bambu account email: ").strip()
    password = getpass.getpass("Password (not echoed): ")

    status, payload, headers = post(
        host, "/v1/user-service/user/login",
        {"account": email, "password": password, "apiError": ""},
    )
    if status != 200:
        print(f"login failed: HTTP {status} {payload}", file=sys.stderr)
        return 1

    data = payload.get("data") if isinstance(payload.get("data"), dict) else payload
    login_type = data.get("loginType")

    if not login_type:
        tokens = extract_tokens(payload, headers)
        if tokens:
            print(json.dumps(tokens))
            return 0
        print("login ok but no accessToken in response", file=sys.stderr)
        return 1

    if login_type == "verifyCode":
        post(host, "/v1/user-service/user/sendemail/code", {"email": email, "type": "codeLogin"})
        code = input("Enter the code sent to your email/SMS: ").strip()
        status, payload, headers = post(
            host, "/v1/user-service/user/login", {"account": email, "code": code}
        )
        tokens = extract_tokens(payload, headers)
        if tokens:
            print(json.dumps(tokens))
            return 0
        print(f"code login failed: HTTP {status} {payload}", file=sys.stderr)
        return 1

    if login_type == "tfa":
        code = input("Enter the 6-digit code from your authenticator app: ").strip()
        status, payload, headers = post(
            TFA_HOST, "/api/sign-in/tfa", {"tfaKey": data.get("tfaKey", ""), "tfaCode": code}
        )
        tokens = extract_tokens(payload, headers)
        if tokens:
            print(json.dumps(tokens))
            return 0
        print(f"tfa failed: HTTP {status} {payload}", file=sys.stderr)
        return 1

    print(f"unknown loginType {login_type!r}: {payload}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
