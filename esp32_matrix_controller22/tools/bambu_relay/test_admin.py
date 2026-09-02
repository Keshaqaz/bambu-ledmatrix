#!/usr/bin/env python3
import json
import os
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


HERE = Path(__file__).resolve().parent
ADMIN = HERE / "admin.py"


class RotationHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/preference":
            if self.server.auth_status != 200:
                self.send_response(self.server.auth_status)
                self.end_headers()
                return
            if self.headers.get("Authorization") != "Bearer fresh-token":
                self.send_response(401)
                self.end_headers()
                return
            payload = {"uid": "123456"}
        elif self.path == "/status":
            payload = {
                "ok": True,
                "source": "cloud_mqtt",
                "connected": self.server.status_connected,
            }
        else:
            self.send_response(404)
            self.end_headers()
            return
        body = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.path != "/refreshtoken":
            self.send_response(404)
            self.end_headers()
            return
        length = int(self.headers.get("Content-Length", 0) or 0)
        self.server.last_refresh_body = self.rfile.read(length) if length else b""
        self.server.last_refresh_headers = [(k, v) for k, v in self.headers.items()]
        status = getattr(self.server, "refresh_status", 200)
        if status != 200:
            self.send_response(status)
            self.end_headers()
            return
        response = getattr(self.server, "refresh_response", None)
        if response is None:
            response = {
                "accessToken": "fresh-token",
                "refreshToken": "rotated-refresh",
                "expiresIn": 3600,
            }
        body = json.dumps(response).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, _format, *_args):
        return


class AdminCliTest(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        self.config = self.root / "bambu_relay.conf"
        self.config.write_text(
            "BAMBU_CLOUD_TOKEN='old-token'\n"
            "BAMBU_CLOUD_REFRESH_TOKEN='old-refresh'\n"
            "BAMBU_PRINTER_SERIAL='SERIAL'\n"
            "BAMBU_RELAY_KEY='relay-key'\n"
        )
        self.config.chmod(0o600)

        self.service_log = self.root / "service.log"
        self.service = self.root / "bambu-relay"
        self.service.write_text(
            "#!/bin/sh\n"
            f"printf '%s\\n' \"$1\" >> '{self.service_log}'\n"
        )
        self.service.chmod(0o700)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), RotationHandler)
        self.server.auth_status = 200
        self.server.status_connected = True
        self.server.refresh_status = 200
        self.server.refresh_response = None
        self.server.last_refresh_headers = None
        self.server.last_refresh_body = None
        self.server_thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.server_thread.start()
        self.base_url = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.tempdir.cleanup()

    def run_admin(self, stdin, command="install-cloud-token"):
        env = os.environ.copy()
        env.update({
            "BAMBU_RELAY_CONFIG": str(self.config),
            "BAMBU_RELAY_SERVICE": str(self.service),
            "BAMBU_AUTH_CHECK_URL": f"{self.base_url}/preference",
            "BAMBU_REFRESH_URL": f"{self.base_url}/refreshtoken",
            "BAMBU_RELAY_STATUS_URL": f"{self.base_url}/status",
            "BAMBU_RELAY_VERIFY_TIMEOUT": "1",
        })
        return subprocess.run(
            [sys.executable, str(ADMIN), command],
            input=stdin,
            text=True,
            capture_output=True,
            env=env,
            check=False,
        )

    def test_valid_token_is_installed_atomically_and_verified(self):
        result = self.run_admin("fresh-token\n")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("BAMBU_CLOUD_TOKEN='fresh-token'", self.config.read_text())
        self.assertEqual(stat.S_IMODE(self.config.stat().st_mode), 0o600)
        self.assertEqual(self.service_log.read_text().splitlines(), ["restart"])
        self.assertNotIn("fresh-token", result.stdout + result.stderr)

    def test_failed_relay_verification_restores_previous_token(self):
        self.server.status_connected = False

        result = self.run_admin("fresh-token\n")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("BAMBU_CLOUD_TOKEN='old-token'", self.config.read_text())
        self.assertEqual(self.service_log.read_text().splitlines(), ["restart", "restart"])
        self.assertNotIn("fresh-token", result.stdout + result.stderr)

    def test_check_cloud_token_reports_expired_without_restarting(self):
        result = self.run_admin("", command="check-cloud-token")

        self.assertEqual(result.returncode, 2)
        self.assertFalse(self.service_log.exists())
        self.assertNotIn("old-token", result.stdout + result.stderr)
        self.assertIn("HTTP 401", result.stderr)

    def test_check_distinguishes_transient_api_failure_from_rotation(self):
        self.config.write_text("BAMBU_CLOUD_TOKEN='fresh-token'\n")
        self.server.auth_status = 503

        result = self.run_admin("", command="check-cloud-token")

        self.assertEqual(result.returncode, 3)
        self.assertIn("HTTP 503", result.stderr)
        self.assertNotIn("fresh-token", result.stdout + result.stderr)

    def test_login_json_records_server_supplied_expiration(self):
        before = int(time.time())
        login_response = json.dumps({"accessToken": "fresh-token", "expiresIn": 3600})

        result = self.run_admin(login_response)

        self.assertEqual(result.returncode, 0, result.stderr)
        assignments = {}
        for line in self.config.read_text().splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                assignments[key] = value.strip("'")
        expires_at = int(assignments["BAMBU_CLOUD_TOKEN_EXPIRES_AT"])
        self.assertGreaterEqual(expires_at, before + 3600)
        self.assertLessEqual(expires_at, int(time.time()) + 3600)
        self.assertNotIn("fresh-token", result.stdout + result.stderr)

    def test_check_warns_before_recorded_expiration(self):
        self.config.write_text(
            "BAMBU_CLOUD_TOKEN='fresh-token'\n"
            f"BAMBU_CLOUD_TOKEN_EXPIRES_AT='{int(time.time()) + 3600}'\n"
            "BAMBU_PRINTER_SERIAL='SERIAL'\n"
        )

        result = self.run_admin("", command="check-cloud-token")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expires within", result.stderr)
        self.assertNotIn("fresh-token", result.stdout + result.stderr)
        self.assertFalse(self.service_log.exists())

    def test_bambu_studio_login_payload_is_accepted(self):
        login_payload = json.dumps({
            "data": {"token": "fresh-token", "expires_in": "3600"}
        })

        result = self.run_admin(login_payload)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("BAMBU_CLOUD_TOKEN='fresh-token'", self.config.read_text())
        self.assertIn("BAMBU_CLOUD_TOKEN_EXPIRES_AT=", self.config.read_text())

    def test_raw_token_removes_stale_expiration_metadata(self):
        self.config.write_text(
            "BAMBU_CLOUD_TOKEN='old-token'\n"
            "BAMBU_CLOUD_TOKEN_EXPIRES_AT='1'\n"
            "BAMBU_PRINTER_SERIAL='SERIAL'\n"
        )

        result = self.run_admin("fresh-token\n")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("BAMBU_CLOUD_TOKEN_EXPIRES_AT", self.config.read_text())

    def test_installation_repairs_insecure_config_permissions(self):
        self.config.chmod(0o640)

        result = self.run_admin("fresh-token\n")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(stat.S_IMODE(self.config.stat().st_mode), 0o600)

    def test_refresh_rotates_token_and_persists_rotated_refresh_token(self):
        result = self.run_admin("", command="refresh-cloud-token")

        self.assertEqual(result.returncode, 0, result.stderr)
        config = self.config.read_text()
        self.assertIn("BAMBU_CLOUD_TOKEN='fresh-token'", config)
        self.assertIn("BAMBU_CLOUD_REFRESH_TOKEN='rotated-refresh'", config)
        self.assertIn("BAMBU_CLOUD_TOKEN_EXPIRES_AT=", config)
        self.assertEqual(self.service_log.read_text().splitlines(), ["restart"])
        self.assertNotIn("fresh-token", result.stdout + result.stderr)
        self.assertNotIn("rotated-refresh", result.stdout + result.stderr)

    def test_refresh_sends_stock_identity_block_in_wire_order_without_authorization(self):
        self.run_admin("", command="refresh-cloud-token")

        received = self.server.last_refresh_headers or []
        lowered = [k.lower() for k, _ in received]
        self.assertNotIn("authorization", lowered)
        expected_order = [
            "user-agent", "x-bbl-client-name", "x-bbl-client-type",
            "x-bbl-client-version", "x-bbl-device-id", "x-bbl-language",
            "x-bbl-os-type", "x-bbl-os-version", "x-bbl-agent-version",
            "x-bbl-executable-info", "x-bbl-agent-os-type", "x-bbl-executable-env",
            "accept", "content-type",
        ]
        ordered = []
        seen = set()
        for name in lowered:
            if name in expected_order and name not in seen:
                seen.add(name)
                ordered.append(name)
        self.assertEqual(ordered, expected_order)

        self.assertEqual(
            self.server.last_refresh_body, b'{"refreshToken": "old-refresh"}'
        )
        values = {k.lower(): v for k, v in received}
        self.assertTrue(values.get("user-agent", "").startswith("bambu_network_agent/"))
        self.assertEqual(values.get("x-bbl-client-name"), "BambuStudio")
        self.assertEqual(values.get("x-bbl-client-type"), "slicer")
        self.assertEqual(values.get("x-bbl-executable-info"), "{}")

    def test_refresh_keeps_refresh_token_when_server_does_not_rotate(self):
        self.server.refresh_response = {
            "accessToken": "fresh-token",
            "refreshToken": "old-refresh",
            "expiresIn": 3600,
        }

        result = self.run_admin("", command="refresh-cloud-token")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("BAMBU_CLOUD_REFRESH_TOKEN='old-refresh'", self.config.read_text())

    def test_refresh_failure_leaves_config_untouched(self):
        self.server.refresh_status = 401

        result = self.run_admin("", command="refresh-cloud-token")

        self.assertEqual(result.returncode, 2)
        self.assertIn("HTTP 401", result.stderr)
        config = self.config.read_text()
        self.assertIn("BAMBU_CLOUD_TOKEN='old-token'", config)
        self.assertIn("BAMBU_CLOUD_REFRESH_TOKEN='old-refresh'", config)
        self.assertFalse(self.service_log.exists())

    def test_refresh_with_empty_refresh_token_signals_rotation_needed(self):
        self.config.write_text(
            "BAMBU_CLOUD_TOKEN='old-token'\nBAMBU_PRINTER_SERIAL='SERIAL'\n"
        )

        result = self.run_admin("", command="refresh-cloud-token")

        self.assertEqual(result.returncode, 2)
        self.assertIsNone(self.server.last_refresh_body)
        self.assertFalse(self.service_log.exists())

    def test_refresh_rolls_back_on_relay_verify_failure(self):
        self.server.status_connected = False

        result = self.run_admin("", command="refresh-cloud-token")

        self.assertNotEqual(result.returncode, 0)
        config = self.config.read_text()
        self.assertIn("BAMBU_CLOUD_TOKEN='old-token'", config)
        self.assertIn("BAMBU_CLOUD_REFRESH_TOKEN='old-refresh'", config)
        self.assertEqual(self.service_log.read_text().splitlines(), ["restart", "restart"])
        self.assertNotIn("fresh-token", result.stdout + result.stderr)

    def test_install_from_login_json_bootstraps_refresh_token(self):
        login = json.dumps({
            "accessToken": "fresh-token",
            "refreshToken": "boot-refresh",
            "expiresIn": 3600,
        })

        result = self.run_admin(login)

        self.assertEqual(result.returncode, 0, result.stderr)
        config = self.config.read_text()
        self.assertIn("BAMBU_CLOUD_TOKEN='fresh-token'", config)
        self.assertIn("BAMBU_CLOUD_REFRESH_TOKEN='boot-refresh'", config)


if __name__ == "__main__":
    unittest.main()
