#!/usr/bin/env python3
"""DNS-race panic feedback loop (T05 early-boot defect).

Verdict comes from the firmware's own soak boot ring (GET /api/soak).

PLATFORM ARTIFACT (proven 2026-09-03 by tools probe on this exact board,
core 3.3.8): a CLEAN ESP.restart() reports esp_reset_reason()==3
(ESP_RST_PANIC) on ESP32-C3. Ring entries labeled "panic" after a warm
reboot are therefore NOT evidence of a crash. Honest crash signals are:
  * MORE ring entries than reboots performed (a crashed boot writes its
    own entry and is followed by another boot's entry), or
  * watchdog-family reset reasons (int_wdt / task_wdt / wdt / brownout).
GREEN = exactly one new entry per reboot, none of them watchdog-family,
and /api/status healthy afterwards.

Serial cannot be the verdict source here: app-level HWCDC output never
reaches a host fd on this Mac (ROM console does — esp_rom_printf — but
firmware logs via Serial).

Usage: python3 tools/dnsrace_loop.py [--iters 5] [--window 45]
       [--host 192.168.1.65]
       exit 0 = all green, 1 = >=1 red, 2 = inconclusive
"""
import argparse
import json
import sys
import threading
import time
import urllib.request

# 'panic' is excluded deliberately: see the platform-artifact note above.
# A real Guru crash is still detected via the entry-count rule (a crashed
# boot always enqueues an extra entry, because soakPushBoot runs in setup()
# before any of the known crash sites).
BAD_REASONS = {"int_wdt", "task_wdt", "wdt", "brownout"}


class SerialReader(threading.Thread):
    """Optional postmortem capture; may itself reset the device on open."""

    def __init__(self, port, window):
        super().__init__(daemon=True)
        self.port = port
        self.deadline = time.monotonic() + window
        self.lines = []
        self.lock = threading.Lock()
        self._stop = threading.Event()

    def run(self):
        while not self._stop.is_set() and time.monotonic() < self.deadline:
            try:
                # rts/dtr MUST be cleared before open(): asserting them on the
                # C3 USB-Serial-JTAG port resets the device and contaminates
                # the verdict (boot ring shows spurious "jtag" resets).
                ser_cls = __import__("serial").Serial
                ser = ser_cls(None)
                ser.port = self.port
                ser.baudrate = 115200
                ser.timeout = 0.5
                ser.rts = False
                ser.dtr = False
                ser.open()
            except Exception:  # noqa: BLE001
                time.sleep(0.3)
                continue
            quiet = 0.0
            while time.monotonic() < self.deadline and not self._stop.is_set():
                try:
                    raw = ser.read(256)
                except Exception:  # noqa: BLE001
                    break
                if raw:
                    quiet = 0.0
                    text = raw.decode("utf-8", "replace")
                    with self.lock:
                        for line in text.splitlines():
                            line = line.rstrip()
                            if line:
                                self.lines.append(line)
                else:
                    # EOF-forever after a device reset looks like a stream of
                    # empty reads, not an exception. Reopen only after a long
                    # park: an immediate reopen asserts DTR and resets the C3,
                    # murdering the boot we are trying to observe.
                    quiet += 0.5
                    if quiet > 4.0:
                        break
            try:
                ser.close()
            except Exception:  # noqa: BLE001
                pass
            if time.monotonic() < self.deadline:
                time.sleep(20.0)  # let the observed boot pass its crash window

    def snapshot(self):
        with self.lock:
            return list(self.lines)

    def stop(self):
        self._stop.set()


def http_json(host, path, timeout=5):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=timeout) as r:
        return json.loads(r.read().decode())


def http_post(host, path, timeout=10):
    req = urllib.request.Request(f"http://{host}{path}", data=b"", method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status


def wait_status_up(host, deadline_s):
    end = time.monotonic() + deadline_s
    while time.monotonic() < end:
        try:
            st = http_json(host, "/api/status")
            if st.get("wifi"):
                return st
        except Exception:  # noqa: BLE001
            pass
        time.sleep(1.5)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iters", type=int, default=5)
    ap.add_argument("--window", type=int, default=45,
                    help="seconds to watch the ring after each reboot")
    ap.add_argument("--host", default="192.168.1.65")
    ap.add_argument("--capture", default=None,
                    help="optional serial port for postmortem log "
                         "(WARNING: opening it resets the device once)")
    args = ap.parse_args()

    verdicts = []
    for i in range(1, args.iters + 1):
        reader = None
        try:
            before = http_json(args.host, "/api/soak")
            seq0 = before["currentBoot"]["seq"]
        except Exception as e:  # noqa: BLE001
            print(f"[iter {i}] device unreachable at start: {e}")
            verdicts.append("UNKNOWN")
            continue
        if args.capture:
            reader = SerialReader(args.capture, args.window + 10)
            reader.start()
            time.sleep(2.0)  # let the port-open reset settle, then re-read seq
            try:
                seq0 = http_json(args.host, "/api/soak")["currentBoot"]["seq"]
            except Exception:  # noqa: BLE001
                pass
        try:
            http_post(args.host, "/api/reboot")
            print(f"[iter {i}] reboot requested (session seq {seq0})")
        except Exception as e:  # noqa: BLE001
            print(f"[iter {i}] reboot POST failed: {e}")
            verdicts.append("UNKNOWN")
            if reader:
                reader.stop()
            continue

        st = wait_status_up(args.host, args.window)
        time.sleep(args.window - 10 if args.window > 10 else 5)
        try:
            after = http_json(args.host, "/api/soak")
        except Exception as e:  # noqa: BLE001
            print(f"[iter {i}] soak unreadable after window: {e}")
            verdicts.append("UNKNOWN")
            if reader:
                reader.stop()
            continue
        entries = [b for b in after["boots"] if b["seq"] > seq0]
        entries.sort(key=lambda b: b["seq"])
        reasons = [b["resetReason"] for b in entries]
        crashed = [r for r in reasons if r in BAD_REASONS]
        healthy = st is not None
        if crashed:
            verdict, detail = "RED", f"watchdog-family reasons {reasons}"
        elif len(entries) > 1:
            # one reboot must yield exactly one new boot entry; extras mean
            # a post-reboot session died and another boot followed
            verdict, detail = "RED", f"{len(entries)} boots for one reboot: {reasons}"
        elif not entries:
            verdict, detail = "UNKNOWN", "no new ring entry: reboot never landed"
        elif not healthy:
            verdict, detail = "UNKNOWN", f"ring {reasons} but status not healthy"
        else:
            verdict, detail = "GREEN", f"new boots {reasons}, fw={st.get('fwVersion')}"
        if reader:
            reader.stop()
            with open(f"/tmp/dnsrace_iter_{i}.log", "w") as f:
                f.write("\n".join(reader.snapshot()) + "\n")
        verdicts.append(verdict)
        print(f"[iter {i}] verdict={verdict} {detail}")
        if i < args.iters:
            # Let the fresh session pass 60 s of uptime so the firmware
            # boot-guard clears its counter; otherwise consecutive sub-60 s
            # reboots legitimately trigger a slot rollback (documented
            # tradeoff) and pollute the next iteration's entry count.
            time.sleep(65)

    reds = verdicts.count("RED")
    greens = verdicts.count("GREEN")
    print(f"SUMMARY: {reds} RED / {greens} GREEN / "
          f"{args.iters - reds - greens} UNKNOWN of {args.iters}")
    sys.exit(1 if reds else (0 if greens == args.iters else 2))


if __name__ == "__main__":
    main()
