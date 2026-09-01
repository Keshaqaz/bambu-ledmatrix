import importlib.util
import pathlib
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("bambu_auth_transport_probe.py")


def load_probe_module():
    spec = importlib.util.spec_from_file_location("bambu_auth_transport_probe", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class TransportOracleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.probe = load_probe_module()

    def test_desktop_application_json_is_pass(self):
        self.assertEqual(
            self.probe.classify_http_response(
                400,
                "application/json; charset=utf-8",
                b'{"code":-1,"error":"invalid"}',
            ),
            "TRANSPORT_PASS",
        )

    def test_generic_or_malformed_json_is_error(self):
        for status, body in (
            (502, b'{"code":-1,"error":"proxy"}'),
            (400, b'{"message":"generic refusal"}'),
            (400, b'{"code":'),
            (400, b""),
        ):
            with self.subTest(status=status, body=body):
                self.assertEqual(
                    self.probe.classify_http_response(
                        status, "application/json", body
                    ),
                    "TRANSPORT_ERROR",
                )

    def test_cloudflare_html_403_is_blocked(self):
        self.assertEqual(
            self.probe.classify_http_response(
                403, "text/html", b"<title>Attention Required | Cloudflare</title>"
            ),
            "TRANSPORT_BLOCKED",
        )

    def test_non_application_failure_is_error(self):
        self.assertEqual(
            self.probe.classify_http_response(503, "text/plain", b"upstream unavailable"),
            "TRANSPORT_ERROR",
        )

    def test_esp_public_error_maps_without_body_or_secret(self):
        self.assertEqual(
            self.probe.classify_esp_status(
                {"state": "recoverable_error", "error": "cloudflare_blocked"}
            ),
            "TRANSPORT_BLOCKED",
        )
        self.assertEqual(
            self.probe.classify_esp_status(
                {"state": "recoverable_error", "error": "invalid_credentials"}
            ),
            "TRANSPORT_PASS",
        )
        self.assertEqual(
            self.probe.classify_esp_status(
                {"state": "recoverable_error", "error": "csrf_missing_cookie"}
            ),
            "TRANSPORT_ERROR",
        )
        self.assertEqual(
            self.probe.classify_esp_status(
                {"state": "recoverable_error", "error": "https_connect_failed"}
            ),
            "TRANSPORT_ERROR",
        )

    def test_esp_production_verification_states_are_pass(self):
        for state in (
            "verification_required_email",
            "verification_required_sms",
            "verification_required_totp",
        ):
            with self.subTest(state=state):
                self.assertEqual(
                    self.probe.classify_esp_status({"state": state, "error": ""}),
                    "TRANSPORT_PASS",
                )

    def test_esp_result_is_correlated_to_accepted_operation(self):
        responses = iter(
            (
                {
                    "state": "signed_out",
                    "configured": False,
                    "candidatePending": False,
                    "operationId": "",
                },
                {"accepted": True, "operationId": "expected-operation"},
                {
                    "state": "submitting_credentials",
                    "operationId": "expected-operation",
                    "error": "",
                },
                {
                    "state": "recoverable_error",
                    "operationId": "",
                    "error": "cloudflare_blocked",
                },
            )
        )
        original_read_json = self.probe.read_json
        original_sleep = self.probe.time.sleep
        self.probe.read_json = lambda *args, **kwargs: next(responses)
        self.probe.time.sleep = lambda _: None
        try:
            self.assertEqual(
                self.probe.esp_request("http://device.invalid", 2),
                "TRANSPORT_BLOCKED",
            )
        finally:
            self.probe.read_json = original_read_json
            self.probe.time.sleep = original_sleep

    def test_esp_fast_terminal_status_is_correlated_by_clean_baseline(self):
        responses = iter(
            (
                {
                    "state": "signed_out",
                    "configured": False,
                    "candidatePending": False,
                    "operationId": "",
                },
                {"accepted": True, "operationId": "expected-operation"},
                {
                    "state": "recoverable_error",
                    "operationId": "",
                    "error": "cloudflare_blocked",
                },
            )
        )
        original_read_json = self.probe.read_json
        original_sleep = self.probe.time.sleep
        original_monotonic = self.probe.time.monotonic
        ticks = iter((0, 0, 3))
        self.probe.read_json = lambda *args, **kwargs: next(responses, None)
        self.probe.time.sleep = lambda _: None
        self.probe.time.monotonic = lambda: next(ticks, 3)
        try:
            self.assertEqual(
                self.probe.esp_request("http://device.invalid", 2),
                "TRANSPORT_BLOCKED",
            )
        finally:
            self.probe.read_json = original_read_json
            self.probe.time.sleep = original_sleep
            self.probe.time.monotonic = original_monotonic

    def test_esp_foreign_operation_after_clean_baseline_is_error(self):
        responses = iter(
            (
                {
                    "state": "signed_out",
                    "configured": False,
                    "candidatePending": False,
                    "operationId": "",
                },
                {"accepted": True, "operationId": "expected-operation"},
                {
                    "state": "recoverable_error",
                    "operationId": "foreign-operation",
                    "error": "invalid_credentials",
                },
            )
        )
        original_read_json = self.probe.read_json
        self.probe.read_json = lambda *args, **kwargs: next(responses)
        try:
            self.assertEqual(
                self.probe.esp_request("http://device.invalid", 2),
                "TRANSPORT_ERROR",
            )
        finally:
            self.probe.read_json = original_read_json

    def test_esp_recoverable_state_is_reset_only_without_credentials(self):
        responses = iter(
            (
                {
                    "state": "recoverable_error",
                    "configured": False,
                    "candidatePending": False,
                    "operationId": "",
                },
                {"accepted": True},
                {
                    "state": "signed_out",
                    "configured": False,
                    "candidatePending": False,
                    "operationId": "",
                },
            )
        )
        original_read_json = self.probe.read_json
        original_sleep = self.probe.time.sleep
        self.probe.read_json = lambda *args, **kwargs: next(responses)
        self.probe.time.sleep = lambda _: None
        try:
            self.assertTrue(
                self.probe.prepare_esp_baseline("http://device.invalid", 2)
            )
        finally:
            self.probe.read_json = original_read_json
            self.probe.time.sleep = original_sleep


if __name__ == "__main__":
    unittest.main()
