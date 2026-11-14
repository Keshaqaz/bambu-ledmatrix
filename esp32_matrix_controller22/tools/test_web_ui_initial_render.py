#!/usr/bin/env python3
"""Regression checks for WebUI state before asynchronous hydration completes."""

from html.parser import HTMLParser
from pathlib import Path
import re
import unittest


WEB_UI = Path(__file__).resolve().parents[1] / "web" / "index.html"


class RangeParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ranges = []

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        if tag == "input" and values.get("type") == "range":
            self.ranges.append(values)


class InitialRenderTest(unittest.TestCase):
    def test_range_fill_matches_thumb_before_settings_load(self):
        parser = RangeParser()
        parser.feed(WEB_UI.read_text(encoding="utf-8"))
        self.assertTrue(parser.ranges, "WebUI must contain range controls")

        for control in parser.ranges:
            with self.subTest(control=control.get("id")):
                minimum = float(control.get("min", 0))
                maximum = float(control.get("max", 100))
                value = float(control["value"])
                expected = (value - minimum) / (maximum - minimum) * 100
                match = re.search(r"(?:^|;)\s*--p\s*:\s*([0-9.]+)%", control.get("style", ""))
                self.assertIsNotNone(
                    match,
                    "range needs an inline --p so its first painted fill matches its thumb",
                )
                self.assertAlmostEqual(float(match.group(1)), expected, places=2)


if __name__ == "__main__":
    unittest.main()
