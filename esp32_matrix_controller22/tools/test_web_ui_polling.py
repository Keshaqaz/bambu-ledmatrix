#!/usr/bin/env python3
"""Regression checks for bounded WebUI polling on the single-threaded ESP32 server."""

from pathlib import Path
import re
import unittest


WEB_UI_SOURCE = Path(__file__).resolve().parents[1] / "src" / "index_html.h"


class WebUiPollingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = WEB_UI_SOURCE.read_text(encoding="utf-8")

    def test_status_polling_waits_for_the_previous_cycle(self):
        self.assertNotRegex(
            self.source,
            r"setInterval\s*\(\s*fetchStatus",
            "setInterval can overlap slow ESP32 requests and create a request storm",
        )
        self.assertIn("statusRequestInFlight", self.source)
        self.assertRegex(
            self.source,
            r"await\s+fetchStatus\s*\(\s*\)\s*;\s*scheduleStatusPoll",
        )

    def test_steady_status_poll_does_not_spawn_a_second_auth_request(self):
        match = re.search(
            r"async function fetchStatus\(\)\{(?P<body>.*?)\n\}\n\n/\* ── WiFi",
            self.source,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(match, "fetchStatus function not found")
        self.assertNotIn(
            "fetchBambuAuth()",
            match.group("body"),
            "steady status polling must not double the HTTP request rate",
        )

    def test_polling_pauses_when_the_page_is_hidden(self):
        self.assertIn("if(document.hidden||fwBusy)return", self.source)
        self.assertRegex(
            self.source,
            r"visibilitychange.*stopStatusPolling",
        )


if __name__ == "__main__":
    unittest.main()
