#!/usr/bin/env python3
"""Collect only T05 resource/recovery counters from an ESP32 status endpoint."""

import argparse
import json
import time
import urllib.request
from datetime import datetime, timezone
from pathlib import Path


def fetch_status(base_url: str, timeout: float) -> dict:
    request = urllib.request.Request(
        base_url.rstrip("/") + "/api/status",
        headers={"Accept": "application/json", "User-Agent": "t05-soak-monitor"},
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.load(response)


def sanitize(status: dict) -> dict:
    direct = status.get("bambuDirect") or {}
    shadow = direct.get("shadow") or {}
    stacks = status.get("taskStackFree") or {}
    return {
        "at": datetime.now(timezone.utc).isoformat(),
        "uptime": status.get("uptime"),
        "uptimeSeconds": status.get("uptimeSeconds"),
        "fwVersion": status.get("fwVersion"),
        "fwPartition": status.get("fwPartition"),
        "wifi": bool(status.get("wifi")),
        "heapCurrent": status.get("heapInternalCurrent"),
        "heapMinimum": status.get("heapInternalMinimum"),
        "largestCurrent": status.get("heapInternalLargest"),
        "largestMinimum": status.get("heapInternalMinimumLargest"),
        "stackLoop": stacks.get("loop"),
        "stackWeather": stacks.get("weather"),
        "stackPrinter": stacks.get("printer"),
        "stackMqtt": stacks.get("mqtt"),
        "weatherCode": status.get("weatherCode"),
        "weatherAgeSec": status.get("weatherAgeSec"),
        "cloudState": direct.get("state"),
        "configuredSource": direct.get("source"),
        "effectiveSource": direct.get("effectiveSource"),
        "mqttConnected": bool(direct.get("mqttConnected")),
        "mqttReconnectCount": direct.get("mqttReconnectCount"),
        "rejectedReports": direct.get("rejectedReports"),
        "maximumReportBytes": direct.get("maximumReportBytes"),
        "recoveryReason": direct.get("recoveryReason"),
        "recoveryFailures": direct.get("recoveryFailures"),
        "shadowComparisons": shadow.get("comparisons"),
        "shadowMatches": shadow.get("matches"),
        "shadowMismatches": shadow.get("mismatches"),
        "shadowLastReasonMask": shadow.get("lastReasonMask"),
        "shadowReasonCounts": shadow.get("reasonCounts"),
    }


def numeric_min(samples: list[dict], key: str):
    values = [sample[key] for sample in samples if isinstance(sample.get(key), (int, float))]
    return min(values) if values else None


def numeric_delta(samples: list[dict], key: str):
    values = [sample[key] for sample in samples if isinstance(sample.get(key), (int, float))]
    return values[-1] - values[0] if len(values) >= 2 else None


def monotonically_falling(samples: list[dict], key: str) -> bool:
    values = [sample[key] for sample in samples if isinstance(sample.get(key), (int, float))]
    return (
        len(values) >= 3
        and any(later < earlier for earlier, later in zip(values, values[1:]))
        and all(later <= earlier for earlier, later in zip(values, values[1:]))
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://192.168.1.65")
    parser.add_argument("--hours", type=float, default=72.0)
    parser.add_argument("--interval", type=float, default=60.0)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    deadline = time.monotonic() + args.hours * 3600.0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    samples: list[dict] = []
    failures = 0
    with args.output.open("a", encoding="utf-8") as output:
        while time.monotonic() < deadline:
            started = time.monotonic()
            try:
                sample = sanitize(fetch_status(args.url, args.timeout))
                samples.append(sample)
                output.write(json.dumps(sample, ensure_ascii=False, separators=(",", ":")) + "\n")
                output.flush()
            except (OSError, ValueError, json.JSONDecodeError) as error:
                failures += 1
                event = {
                    "at": datetime.now(timezone.utc).isoformat(),
                    "monitorError": type(error).__name__,
                }
                output.write(json.dumps(event, separators=(",", ":")) + "\n")
                output.flush()
            time.sleep(max(0.0, args.interval - (time.monotonic() - started)))

    minimum_heap = numeric_min(samples, "heapMinimum")
    minimum_largest = numeric_min(samples, "largestMinimum")
    reserves = {
        "loop": numeric_min(samples, "stackLoop"),
        "weather": numeric_min(samples, "stackWeather"),
        "printer": numeric_min(samples, "stackPrinter"),
        "mqtt": numeric_min(samples, "stackMqtt"),
    }
    reconnect_delta = numeric_delta(samples, "mqttReconnectCount")
    summary = {
        "samples": len(samples),
        "monitorFailures": failures,
        "minimumHeap": minimum_heap,
        "minimumLargestBlock": minimum_largest,
        "minimumTaskReserve": reserves,
        "heapCurrentDelta": numeric_delta(samples, "heapCurrent"),
        "largestCurrentDelta": numeric_delta(samples, "largestCurrent"),
        "heapMonotonicallyFalling": monotonically_falling(samples, "heapCurrent"),
        "largestMonotonicallyFalling": monotonically_falling(samples, "largestCurrent"),
        "mqttReconnectDelta": reconnect_delta,
        "shadowMismatchDelta": numeric_delta(samples, "shadowMismatches"),
        "uptimeResetDetected": any(
            later.get("uptimeSeconds", 0) < earlier.get("uptimeSeconds", 0)
            for earlier, later in zip(samples, samples[1:])
            if isinstance(earlier.get("uptimeSeconds"), (int, float))
            and isinstance(later.get("uptimeSeconds"), (int, float))
        ),
        "gates": {
            "heap": minimum_heap is not None and minimum_heap >= 51200,
            "largestBlock": minimum_largest is not None and minimum_largest >= 32768,
            "taskReserves": all(
                value is not None and value >= 2048 for value in reserves.values()
            ),
            "multipleReconnects": reconnect_delta is not None and reconnect_delta >= 2,
            "noMonotonicLeak": not monotonically_falling(samples, "heapCurrent")
            and not monotonically_falling(samples, "largestCurrent"),
        },
    }
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
