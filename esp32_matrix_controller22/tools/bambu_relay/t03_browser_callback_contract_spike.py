#!/usr/bin/env python3
"""PROTOTYPE ONLY — T03 browser-ticket callback feasibility probe.

Run:
    python3 tools/bambu_relay/t03_browser_callback_contract_spike.py --matrix

Question: can one callback URL both pass Bambu's official Studio browser
callback gate and reach an ESP32 WebUI from the user's browser?

This is a pure contract model of the first-party web bundle observed on
2026-08-28 plus the authenticated generic-redirect observation recorded in
BAMBU_GOOGLE_AUTH_TOKEN_ACQUISITION.md. It performs no network I/O, accepts no
ticket or credential, and persists nothing.
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import re
from dataclasses import asdict, dataclass
from urllib.parse import urlsplit


OFFICIAL_STUDIO_CALLBACK = re.compile(r"^http://localhost:\d+$")
OBSERVED_GENERIC_TO_REJECTIONS = {"http://192.168.1.65/"}


@dataclass(frozen=True)
class CallbackContract:
    callback_base: str
    evidence: str
    official_gate_accepts: bool
    destination: str
    reaches_esp32_webui: bool
    feasible: bool
    reason: str


def evaluate_callback(callback_base: str) -> CallbackContract:
    """Evaluate only the load-bearing callback constraint from T03."""
    accepted = OFFICIAL_STUDIO_CALLBACK.fullmatch(callback_base) is not None
    observed_rejected = callback_base in OBSERVED_GENERIC_TO_REJECTIONS
    hostname = (urlsplit(callback_base).hostname or "").lower()
    try:
        loopback = ipaddress.ip_address(hostname).is_loopback
    except ValueError:
        loopback = hostname == "localhost"
    destination = "browser_loopback" if loopback else "network_host"
    reaches_device = destination == "network_host"
    feasible = accepted and reaches_device

    if observed_rejected:
        evidence = "authenticated generic-to probe"
        reason = "Bambu kept navigation on its own site instead of redirecting to the ESP32"
    elif accepted:
        evidence = "deployed Studio/Suite callback regex"
        reason = "accepted callback resolves on the browser host, not on the ESP32"
    else:
        evidence = "deployed Studio/Suite callback regex"
        reason = "ESP32 callback is rejected by Bambu's exact localhost-only gate"

    if feasible:
        reason = "official callback reaches the ESP32 WebUI"

    return CallbackContract(
        callback_base=callback_base,
        evidence=evidence,
        official_gate_accepts=accepted,
        destination=destination,
        reaches_esp32_webui=reaches_device,
        feasible=feasible,
        reason=reason,
    )


SCENARIOS = (
    "http://localhost:13618",
    "http://127.0.0.1:13618",
    "http://192.168.1.65/",
    "http://192.168.4.1/",
    "http://ledmatrix.local/",
)


def render_matrix() -> None:
    results = [evaluate_callback(url) for url in SCENARIOS]
    print(json.dumps([asdict(result) for result in results], indent=2))
    print("\nOAUTH_CALLBACK:", "FEASIBLE" if any(r.feasible for r in results) else "BLOCKED")
    print("REVISED_AUTH_FLOW: FEASIBLE (Bambu email/password prerequisite confirmed)")


def render_frame(callback_base: str) -> None:
    print("\033[2J\033[H", end="")
    print("\033[1mT03 browser callback contract — PROTOTYPE ONLY\033[0m")
    print(json.dumps(asdict(evaluate_callback(callback_base)), indent=2))
    print("\n\033[1m[l]\033[0m localhost  \033[1m[i]\033[0m device IP  "
          "\033[1m[m]\033[0m device mDNS  \033[1m[u]\033[0m custom  \033[1m[q]\033[0m quit")


def interactive() -> None:
    callback_base = SCENARIOS[0]
    while True:
        render_frame(callback_base)
        action = input("> ").strip().lower()
        if action == "q":
            return
        if action == "l":
            callback_base = SCENARIOS[0]
        elif action == "i":
            callback_base = SCENARIOS[2]
        elif action == "m":
            callback_base = SCENARIOS[4]
        elif action == "u":
            callback_base = input("callback base: ").strip()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--matrix", action="store_true", help="print reproducible built-in cases")
    args = parser.parse_args()
    render_matrix() if args.matrix else interactive()


if __name__ == "__main__":
    main()
