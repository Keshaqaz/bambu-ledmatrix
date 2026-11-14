#!/usr/bin/env python3
import argparse
import hmac
import json
import os
import signal
import ssl
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def env(name: str, default: str = "") -> str:
    return os.environ.get(name, default)


def parse_args():
    parser = argparse.ArgumentParser(description="Bambu MQTT -> HTTP relay")
    parser.add_argument("--printer-serial", default=env("BAMBU_PRINTER_SERIAL"))
    parser.add_argument("--listen-host", default=env("BAMBU_RELAY_HOST", "0.0.0.0"))
    parser.add_argument("--listen-port", type=int, default=int(env("BAMBU_RELAY_PORT", "8787")))
    parser.add_argument("--auth-key", default=env("BAMBU_RELAY_KEY"))
    parser.add_argument("--stale-seconds", type=float, default=float(env("BAMBU_RELAY_STALE_SECONDS", "70")))

    parser.add_argument("--cloud-token", default=env("BAMBU_CLOUD_TOKEN"))
    parser.add_argument("--cloud-uid", default=env("BAMBU_CLOUD_UID"))
    parser.add_argument("--mqtt-host", default=env("BAMBU_MQTT_HOST", "us.mqtt.bambulab.com"))
    parser.add_argument("--mqtt-port", type=int, default=int(env("BAMBU_MQTT_PORT", "8883")))

    parser.add_argument("--printer-host", default=env("BAMBU_PRINTER_HOST"))
    parser.add_argument("--printer-access-code", default=env("BAMBU_PRINTER_ACCESS_CODE"))
    parser.add_argument("--mqtt-username", default=env("BAMBU_MQTT_USERNAME", "bblp"))
    return parser.parse_args()


def http_json(url: str, bearer_token: str):
    req = urllib.request.Request(url)
    req.add_header("Accept", "application/json")
    if bearer_token:
        req.add_header("Authorization", f"Bearer {bearer_token}")
    req.add_header("User-Agent", "ledmatrix-bambu-relay/1.0")
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            raw = resp.read().decode("utf-8", "ignore")
            return resp.status, json.loads(raw or "{}")
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "ignore")
        raise RuntimeError(f"HTTP {exc.code} from {url}: {body[:400]}")
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Request failed for {url}: {exc}")


def retry_runtime(description: str, func, attempts: int = 24, delay: int = 5):
    last_exc = None
    for attempt in range(1, attempts + 1):
        try:
            return func()
        except (RuntimeError, OSError) as exc:
            last_exc = exc
            print(f"[relay] {description} failed attempt {attempt}/{attempts}: {exc}")
            if attempt == attempts:
                break
            time.sleep(delay)
    raise RuntimeError(f"{description} failed after {attempts} attempts: {last_exc}")



def safe_int(value, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return default


def callback_reason_code(args, default="unknown"):
    if len(args) >= 4:
        return args[3]
    if len(args) >= 3:
        return args[2]
    return default


def resolve_uid(explicit_uid: str, cloud_token: str) -> str:
    if explicit_uid:
        return explicit_uid.removeprefix("u_")
    _, payload = http_json("https://api.bambulab.com/v1/design-user-service/my/preference", cloud_token)
    for key in ("uid", "userId", "user_id", "accountId", "account_id"):
        value = payload.get(key)
        if value:
            return str(value).removeprefix("u_")
    raise RuntimeError(f"Could not resolve Bambu uid from preference payload: {json.dumps(payload, ensure_ascii=False)[:400]}")


def resolve_serial(explicit_serial: str, cloud_token: str) -> str:
    if explicit_serial:
        return explicit_serial
    _, payload = http_json("https://api.bambulab.com/v1/iot-service/api/user/bind", cloud_token)
    devices = payload.get("devices")
    if not isinstance(devices, list) or not devices:
        raise RuntimeError(f"Cloud bind returned no devices: {json.dumps(payload, ensure_ascii=False)[:400]}")
    if len(devices) > 1:
        raise RuntimeError("Cloud bind returned multiple printers; pass --printer-serial explicitly")
    dev = devices[0]
    for key in ("dev_id", "devId", "serial", "deviceId"):
        value = dev.get(key)
        if value:
            return str(value)
    raise RuntimeError(f"Could not resolve printer serial from bind payload: {json.dumps(dev, ensure_ascii=False)[:400]}")


def map_lifecycle(gcode_state: str, has_error: bool) -> str:
    # Ошибка = принтер сам остановился (FAILED или PAUSE с hms/print_error).
    # Информационные hms/print_error при RUNNING/IDLE/FINISH игнорируются —
    # паритет с прошивкой (bambuLifecycleFromReport).
    state = (gcode_state or "").upper()
    if state == "RUNNING":
        return "printing"
    if state == "PREPARE":
        return "preparing"
    if state in ("PAUSE", "PAUSED"):
        return "error" if has_error else "paused"
    if state == "FINISH":
        return "finished"
    if state == "IDLE":
        return "idle"
    if state == "FAILED":
        return "error"
    return "unknown"


def stage_label(stage_id: int) -> str:
    labels = {
        0: "printing",
        1: "auto_bed_leveling",
        2: "heatbed_preheating",
        4: "changing_filament",
        5: "m400_pause",
        6: "paused_filament_runout",
        7: "heating_hotend",
        10: "inspecting_first_layer",
        16: "paused_user",
        22: "filament_unloading",
        24: "filament_loading",
        29: "cooling_chamber",
        35: "paused_nozzle_clog",
        49: "heating_chamber",
        -1: "idle",
        255: "idle",
    }
    return labels.get(stage_id, "")


def stage_from_lifecycle(lifecycle: str) -> str:
    labels = {
        "printing": "printing",
        "preparing": "preparing",
        "paused": "paused",
        "finished": "finished",
        "error": "error",
    }
    return labels.get(lifecycle, "")


class RelayState:
    def __init__(self, serial: str, source: str, stale_seconds: float = 70.0):
        self.lock = threading.Lock()
        self.serial = serial
        self.source = source
        self.stale_seconds = stale_seconds
        self.connected = False
        self.last_message_at = 0.0
        self.last_printer_report_at = 0.0
        self.finished_since = 0.0
        self.payload = {
            "ok": True,
            "source": source,
            "serial": serial,
            "connected": False,
            "printerOnline": False,
            "lifecycle": "unknown",
            "progress": 0.0,
            "status": "",
            "stage": "",
            "detail": "Relay запущен, ждём MQTT",
            "jobName": "",
            "currentLayer": 0,
            "totalLayers": 0,
            "remainingSeconds": 0,
            "updatedAt": 0,
        }

    def _has_printer_report(self) -> bool:
        return self.last_printer_report_at > 0.0

    def _apply_unknown_printer_snapshot(self, payload: dict, detail: str):
        payload["printerOnline"] = False
        payload["lifecycle"] = "unknown"
        payload["progress"] = 0.0
        payload["status"] = ""
        payload["stage"] = ""
        payload["jobName"] = ""
        payload["currentLayer"] = 0
        payload["totalLayers"] = 0
        payload["remainingSeconds"] = 0
        payload["detail"] = detail

    def set_connected(self, connected: bool, detail: str):
        with self.lock:
            self.connected = connected
            self.payload["connected"] = connected
            if connected and not self._has_printer_report():
                # MQTT broker connectivity does not mean the printer itself is online.
                self._apply_unknown_printer_snapshot(self.payload, "MQTT подключен, ждём данные принтера")
            elif not connected and not self._has_printer_report():
                self._apply_unknown_printer_snapshot(self.payload, detail)
            else:
                # Keep the last valid printer snapshot across short MQTT reconnects.
                self.payload["detail"] = detail
            self.payload["updatedAt"] = int(time.time())

    def update_from_print(self, print_obj: dict):
        gcode_state = str(print_obj.get("gcode_state", "") or "")
        stage_obj = print_obj.get("stage") if isinstance(print_obj.get("stage"), dict) else {}
        stage_id = safe_int(stage_obj.get("_id", print_obj.get("stg_cur", -1)), -1)
        stage_name = str(stage_obj.get("name", stage_obj.get("stage", "")) or "")
        progress_raw = print_obj.get("mc_percent", None)
        progress = None
        if progress_raw is not None:
            try:
                progress = float(progress_raw)
            except Exception:
                progress = None
            if progress is not None and 0.0 <= progress < 1.0:
                progress *= 100.0
        print_error_raw = print_obj.get("print_error", None)
        print_error = 0
        if print_error_raw is not None:
            try:
                print_error = safe_int(print_error_raw, 0)
            except Exception:
                print_error = 0
        hms = print_obj.get("hms")
        has_error = (print_error_raw is not None and print_error != 0) or (isinstance(hms, list) and len(hms) > 0)
        if not stage_name:
            stage_name = stage_label(stage_id)
        job_name = str(print_obj.get("subtask_name", print_obj.get("gcode_file", "")) or "")
        remaining_minutes = print_obj.get("mc_remaining_time", print_obj.get("remaining_minutes", print_obj.get("remainingMinutes")))
        remaining_seconds = print_obj.get("remaining_seconds", print_obj.get("remainingSeconds", print_obj.get("remaining_time", print_obj.get("remainingTime"))))
        if remaining_minutes is not None:
            try:
                remaining_seconds = safe_int(remaining_minutes, 0) * 60
            except Exception:
                remaining_seconds = 0
        else:
            try:
                remaining_seconds = safe_int(remaining_seconds, 0)
            except Exception:
                remaining_seconds = 0

        meaningful = bool(
            gcode_state or
            progress_raw is not None or
            print_error_raw is not None or
            stage_obj or
            ("stg_cur" in print_obj) or
            ("subtask_name" in print_obj) or
            ("gcode_file" in print_obj) or
            ("layer_num" in print_obj) or
            ("total_layer_num" in print_obj) or
            ("mc_remaining_time" in print_obj) or
            ("remaining_seconds" in print_obj) or
            ("remaining_time" in print_obj) or
            (isinstance(hms, list) and len(hms) > 0)
        )
        if not meaningful:
            return

        with self.lock:
            prev = dict(self.payload)
            status_text = gcode_state or str(prev.get("status", "") or "")
            stage_text = stage_name or str(prev.get("stage", "") or "")
            if progress is None:
                progress = float(prev.get("progress", 0.0) or 0.0)
            lifecycle = map_lifecycle(status_text, has_error)
            if lifecycle == "unknown":
                lifecycle = str(prev.get("lifecycle", "unknown") or "unknown")
            if (not stage_text or stage_text == "idle") and lifecycle not in ("idle", "unknown"):
                stage_text = stage_from_lifecycle(lifecycle) or stage_text
            if not job_name:
                job_name = str(prev.get("jobName", "") or "")
            if remaining_seconds <= 0:
                try:
                    remaining_seconds = safe_int(prev.get("remainingSeconds", 0), 0)
                except Exception:
                    remaining_seconds = 0

            prev_lifecycle = str(prev.get("lifecycle", "unknown") or "unknown")
            if lifecycle == "finished":
                if prev_lifecycle != "finished" or self.finished_since == 0.0:
                    self.finished_since = time.time()
                if progress < 100.0:
                    progress = 100.0
            else:
                self.finished_since = 0.0

            self.connected = True
            self.last_message_at = time.time()
            self.last_printer_report_at = self.last_message_at
            self.payload.update({
                "serial": self.serial,
                "connected": True,
                "printerOnline": True,
                "lifecycle": lifecycle,
                "progress": max(0.0, min(progress, 100.0)),
                "status": status_text,
                "stage": stage_text,
                "detail": job_name or stage_text or status_text or "MQTT payload received",
                "jobName": job_name,
                "currentLayer": safe_int(print_obj.get("layer_num", prev.get("currentLayer", 0)), 0),
                "totalLayers": safe_int(print_obj.get("total_layer_num", prev.get("totalLayers", 0)), 0),
                "remainingSeconds": remaining_seconds,
                "updatedAt": int(time.time()),
            })

    def get_payload(self):
        with self.lock:
            payload = dict(self.payload)
            if payload.get("lifecycle") == "finished" and self.finished_since and (time.time() - self.finished_since > 30.0):
                payload["lifecycle"] = "idle"
                payload["progress"] = 0.0
                payload["status"] = "IDLE"
                payload["stage"] = "idle"
                payload["detail"] = "idle"
            if payload.get("printerOnline") and payload.get("connected") and payload.get("lifecycle") in ("unknown", "") and not payload.get("status") and not payload.get("jobName"):
                payload["lifecycle"] = "idle"
                payload["progress"] = 0.0
                payload["status"] = "IDLE"
                payload["stage"] = "idle"
                payload["detail"] = "idle"
            if self.last_printer_report_at and time.time() - self.last_printer_report_at > self.stale_seconds:
                self._apply_unknown_printer_snapshot(payload, "MQTT stale, ждём новое сообщение")
            return payload


class RelayHandler(BaseHTTPRequestHandler):
    state: RelayState = None
    auth_key: str = ""

    def _send_json(self, code: int, payload: dict):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/healthz":
            self._send_json(200, {"ok": True})
            return
        if self.path not in ("/status", "/"):
            self._send_json(404, {"ok": False, "error": "not_found"})
            return
        if self.auth_key:
            if not hmac.compare_digest(self.headers.get("X-Relay-Key", ""), self.auth_key):
                self._send_json(401, {"ok": False, "error": "unauthorized"})
                return
        self._send_json(200, self.state.get_payload())

    def log_message(self, fmt, *args):
        return


def main():
    args = parse_args()
    try:
        import paho.mqtt.client as mqtt
    except ModuleNotFoundError as exc:
        if exc.name == "paho":
            raise SystemExit(
                "Missing dependency: paho-mqtt. Install it with: "
                "python3 -m pip install -r tools/bambu_relay/requirements.txt"
            ) from exc
        raise

    cloud_mode = bool(args.cloud_token)

    if cloud_mode:
        uid = retry_runtime("resolve_uid", lambda: resolve_uid(args.cloud_uid, args.cloud_token))
        serial = retry_runtime("resolve_serial", lambda: resolve_serial(args.printer_serial, args.cloud_token))
        mqtt_host = args.mqtt_host
        mqtt_port = args.mqtt_port
        mqtt_user = f"u_{uid}"
        mqtt_password = args.cloud_token
        source = "cloud_mqtt"
    else:
        if not args.printer_host or not args.printer_serial or not args.printer_access_code:
            raise SystemExit("Need --cloud-token for cloud mode or --printer-host/--printer-serial/--printer-access-code for LAN mode")
        serial = args.printer_serial
        mqtt_host = args.printer_host
        mqtt_port = args.mqtt_port
        mqtt_user = args.mqtt_username
        mqtt_password = args.printer_access_code
        source = "lan_mqtt"

    state = RelayState(serial, source, stale_seconds=args.stale_seconds)
    report_topic = f"device/{serial}/report"
    request_topic = f"device/{serial}/request"

    client_id = f"ledmatrix-relay-{int(time.time())}"
    if hasattr(mqtt, "CallbackAPIVersion"):
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
    else:
        client = mqtt.Client(client_id=client_id)
    client.username_pw_set(mqtt_user, mqtt_password)
    client.reconnect_delay_set(min_delay=1, max_delay=30)
    if cloud_mode:
        client.tls_set_context(ssl.create_default_context())
    else:
        client.tls_set(cert_reqs=ssl.CERT_NONE)
        client.tls_insecure_set(True)

    def publish_initial():
        client.publish(request_topic, '{"info":{"sequence_id":"0","command":"get_version"}}', qos=1)
        client.publish(request_topic, '{"pushing":{"sequence_id":"0","command":"pushall"}}', qos=1)

    def on_connect(*cb_args):
        reason_code = callback_reason_code(cb_args, default=0)
        state.set_connected(True, "Подключено к Bambu Cloud MQTT" if cloud_mode else "Подключено к локальному Bambu MQTT")
        client.subscribe(report_topic, qos=1)
        publish_initial()
        print(f"[relay] MQTT connected rc={reason_code}")

    def on_disconnect(*cb_args):
        reason_code = callback_reason_code(cb_args)
        state.set_connected(False, f"MQTT disconnected rc={reason_code}")
        print(f"[relay] MQTT disconnected rc={reason_code}")

    def on_message(_client, _userdata, msg):
        try:
            root = json.loads(msg.payload.decode("utf-8", "ignore"))
        except Exception as exc:
            print(f"[relay] bad MQTT payload on {msg.topic}: {exc}")
            return
        print_obj = root.get("print")
        if isinstance(print_obj, dict):
            state_name = str(print_obj.get("gcode_state", "") or "")
            stage_id = print_obj.get("stg_cur", "")
            progress = print_obj.get("mc_percent", "")
            remaining = print_obj.get("mc_remaining_time", print_obj.get("remaining_time", ""))
            print(f"[relay] report state='{state_name}' stage='{stage_id}' progress='{progress}' remaining='{remaining}' keys={','.join(sorted(print_obj.keys())[:12])}")
            state.update_from_print(print_obj)

    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    retry_runtime("mqtt_connect", lambda: client.connect(mqtt_host, mqtt_port, keepalive=5), attempts=30, delay=2)
    client.loop_start()

    def keepalive_loop():
        while True:
            time.sleep(30)
            try:
                if client.is_connected():
                    client.publish(request_topic, '{"pushing":{"sequence_id":"0","command":"start"}}', qos=1)
            except Exception as exc:
                print(f"[relay] keepalive publish failed: {exc}")

    threading.Thread(target=keepalive_loop, daemon=True).start()

    RelayHandler.state = state
    RelayHandler.auth_key = args.auth_key or ""
    server = ThreadingHTTPServer((args.listen_host, args.listen_port), RelayHandler)

    def on_sigterm(_sig, _frame):
        # procd sends SIGTERM on restart/stop; without a handler Python dies
        # before the finally below can stop the MQTT loop cleanly.
        print("[relay] SIGTERM received, shutting down")
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, on_sigterm)

    print(f"[relay] HTTP listening on http://{args.listen_host}:{args.listen_port}/status")
    print(f"[relay] MQTT target {mqtt_host}:{mqtt_port} serial={serial} mode={source}")
    if cloud_mode:
        print(f"[relay] Cloud uid=u_{uid}")
    try:
        server.serve_forever()
    finally:
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()
