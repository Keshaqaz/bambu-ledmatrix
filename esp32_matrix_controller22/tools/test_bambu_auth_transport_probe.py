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
                400, "application/json; charset=utf-8", b'{"error":"invalid"}'
            ),
            "TRANSPORT_PASS",
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


if __name__ == "__main__":
    unittest.main()
