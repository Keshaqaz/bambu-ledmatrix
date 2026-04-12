#!/usr/bin/env python3
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


STATE_PATH = os.environ.get("BAMBU_MOCK_STATE", "/tmp/bambu_mock_state.json")
HOST = os.environ.get("BAMBU_MOCK_HOST", "0.0.0.0")
PORT = int(os.environ.get("BAMBU_MOCK_PORT", "8788"))


DEFAULT_STATE = {
    "ok": True,
    "source": "mock",
    "serial": "MOCK-SERIAL",
    "connected": True,
    "printerOnline": True,
    "lifecycle": "idle",
    "progress": 0.0,
    "status": "IDLE",
    "stage": "idle",
    "detail": "idle",
    "jobName": "mock job",
    "currentLayer": 0,
    "totalLayers": 0,
    "remainingSeconds": 0,
    "updatedAt": 0,
}


def load_state():
    try:
        with open(STATE_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
            if isinstance(data, dict):
                return data
    except FileNotFoundError:
        pass
    except Exception:
        pass
    return dict(DEFAULT_STATE)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path not in ("/", "/status", "/healthz"):
            self.send_response(404)
            self.end_headers()
            return
        payload = load_state()
        raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, fmt, *args):
        return


if __name__ == "__main__":
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    server.serve_forever()
