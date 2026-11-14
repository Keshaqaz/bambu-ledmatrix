#!/usr/bin/env python3
"""Web OTA watchdog feedback loop for the project ESP32-C3.

The image is streamed through an SSH-reachable LAN host because the development
machine may not have direct access to the device VLAN.  A successful iteration
must return HTTP 200, boot the opposite OTA slot, add exactly one boot-ring
entry, and remain reachable.  Watchdog-family resets or extra boots are RED.

Usage:
  python3 tools/ota_loop.py build/esp32.esp32.esp32c3/\
esp32_matrix_controller22.ino.bin --via pve --iters 3

Exit 0 = all green, 1 = at least one red, 2 = inconclusive.
"""

import argparse
import json
import pathlib
import re
import subprocess
import sys
import time


BAD_REASONS = {"int_wdt", "task_wdt", "wdt", "brownout"}


def remote(via, command, *, data=None, timeout=30):
    return subprocess.run(
        ["ssh", via, command],
        input=data,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        check=False,
    )


def http_json(via, host, path, timeout=20):
    result = remote(
        via,
        f"curl -fsS --max-time {timeout} http://{host}{path}",
        timeout=timeout + 5,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.decode("utf-8", "replace").strip())
    return json.loads(result.stdout)


def wait_for_new_boot(via, host, before_seq, deadline_s):
    deadline = time.monotonic() + deadline_s
    while time.monotonic() < deadline:
        try:
            soak = http_json(via, host, "/api/soak")
            if soak["currentBoot"]["seq"] > before_seq:
                # Re-read status only after observing the new boot. Reading
                # status first can straddle the delayed reboot and combine an
                # old slot with the new ring entry.
                status = http_json(via, host, "/api/status")
                if status.get("wifi"):
                    return status, soak
        except (RuntimeError, json.JSONDecodeError, subprocess.TimeoutExpired):
            pass
        time.sleep(2)
    return None, None


def upload(via, host, image):
    size = len(image)
    command = (
        "curl -sS --max-time 120 "
        "-w '\\n__OTA_HTTP__=%{http_code} "
        "TIME=%{time_total} UP=%{size_upload}\\n' "
        "-F 'firmware=@-;filename=firmware.bin' "
        f"'http://{host}/api/update?size={size}'"
    )
    result = remote(via, command, data=image, timeout=130)
    output = result.stdout.decode("utf-8", "replace")
    match = re.search(r"__OTA_HTTP__=(\d{3}) TIME=([^ ]+) UP=(\d+)", output)
    return result.returncode, match, output, result.stderr.decode("utf-8", "replace")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=pathlib.Path)
    parser.add_argument("--host", default="192.168.1.65")
    parser.add_argument("--via", default="pve")
    parser.add_argument("--iters", type=int, default=3)
    parser.add_argument("--boot-window", type=int, default=45)
    parser.add_argument("--settle", type=int, default=65,
                        help="uptime required between iterations for boot-guard")
    args = parser.parse_args()

    image = args.image.read_bytes()
    verdicts = []
    for iteration in range(1, args.iters + 1):
        try:
            before_status = http_json(args.via, args.host, "/api/status")
            before_soak = http_json(args.via, args.host, "/api/soak")
        except Exception as error:  # noqa: BLE001 - verdict must survive transport errors
            print(f"[iter {iteration}] UNKNOWN preflight failed: {error}")
            verdicts.append("UNKNOWN")
            continue

        initial_wait = max(0, args.settle - before_status.get("uptimeSeconds", 0))
        if initial_wait:
            print(f"[iter {iteration}] settling {initial_wait}s before upload "
                  "for boot-guard", flush=True)
            time.sleep(initial_wait)
            try:
                before_status = http_json(args.via, args.host, "/api/status")
                before_soak = http_json(args.via, args.host, "/api/soak")
            except Exception as error:  # noqa: BLE001
                print(f"[iter {iteration}] UNKNOWN settled preflight failed: {error}")
                verdicts.append("UNKNOWN")
                continue

        before_slot = before_status.get("fwPartition")
        before_seq = before_soak["currentBoot"]["seq"]
        print(f"[iter {iteration}] upload {len(image)} bytes from "
              f"{before_slot}, seq={before_seq}", flush=True)

        try:
            curl_rc, marker, output, error = upload(
                args.via, args.host, image
            )
        except subprocess.TimeoutExpired as exc:
            curl_rc, marker, output, error = 124, None, "", str(exc)

        http_code = marker.group(1) if marker else "000"
        timing = marker.group(2) if marker else "?"
        uploaded = marker.group(3) if marker else "?"
        print(f"[iter {iteration}] curl_rc={curl_rc} HTTP={http_code} "
              f"time={timing}s uploaded={uploaded}")
        if output.strip():
            print(f"[iter {iteration}] response={output.split('__OTA_HTTP__=', 1)[0].strip()}")
        if error.strip():
            print(f"[iter {iteration}] curl_stderr={error.strip()}")

        after_status, after_soak = wait_for_new_boot(
            args.via, args.host, before_seq, args.boot_window
        )
        try:
            if after_soak is None:
                after_soak = http_json(args.via, args.host, "/api/soak")
            entries = sorted(
                (item for item in after_soak["boots"] if item["seq"] > before_seq),
                key=lambda item: item["seq"],
            )
        except Exception as error:  # noqa: BLE001
            print(f"[iter {iteration}] UNKNOWN postflight failed: {error}")
            verdicts.append("UNKNOWN")
            continue

        reasons = [item["resetReason"] for item in entries]
        watchdogs = [reason for reason in reasons if reason in BAD_REASONS]
        after_slot = after_status.get("fwPartition") if after_status else None
        if watchdogs or len(entries) > 1:
            verdict = "RED"
            detail = f"boots={reasons}, slot={after_slot}"
        elif http_code != "200" or curl_rc != 0:
            verdict = "RED"
            detail = f"upload failed; boots={reasons}, slot={after_slot}"
        elif len(entries) != 1 or after_status is None:
            verdict = "UNKNOWN"
            detail = f"boots={reasons}, status_up={after_status is not None}"
        elif after_slot == before_slot:
            verdict = "RED"
            detail = f"slot did not change ({before_slot}); boots={reasons}"
        else:
            verdict = "GREEN"
            detail = (f"{before_slot}->{after_slot}, boots={reasons}, "
                      f"fw={after_status.get('fwVersion')}")
        verdicts.append(verdict)
        print(f"[iter {iteration}] verdict={verdict} {detail}", flush=True)

        if iteration < args.iters:
            uptime = after_status.get("uptimeSeconds", 0) if after_status else 0
            wait_s = max(0, args.settle - uptime)
            if wait_s:
                print(f"[iter {iteration}] settling {wait_s}s for boot-guard", flush=True)
                time.sleep(wait_s)

    reds = verdicts.count("RED")
    greens = verdicts.count("GREEN")
    unknowns = verdicts.count("UNKNOWN")
    print(f"SUMMARY: {reds} RED / {greens} GREEN / {unknowns} UNKNOWN")
    return 1 if reds else (0 if greens == args.iters else 2)


if __name__ == "__main__":
    sys.exit(main())
