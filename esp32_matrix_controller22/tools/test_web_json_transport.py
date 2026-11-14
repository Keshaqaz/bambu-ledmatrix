#!/usr/bin/env python3
"""Regression check for efficient JSON writes to the ESP32 WebServer."""

from pathlib import Path
import re
import unittest


APP_SOURCE = Path(__file__).resolve().parents[1] / "src" / "app.cpp"


class WebJsonTransportTest(unittest.TestCase):
    def test_json_is_not_serialized_one_byte_at_a_time_to_wifi_client(self):
        source = APP_SOURCE.read_text(encoding="utf-8")
        match = re.search(
            r"static void sendJsonDocument\(.*?\n\}", source, flags=re.DOTALL
        )
        self.assertIsNotNone(match, "sendJsonDocument function not found")
        body = match.group(0)
        self.assertNotIn(
            "serializeJson(doc, server.client())",
            body,
            "ArduinoJson writes bytewise to Print; buffer before Wi-Fi transport",
        )
        self.assertIn("serializeJson(doc, payload)", body)
        self.assertIn('server.send(code, "application/json", payload)', body)


if __name__ == "__main__":
    unittest.main()
