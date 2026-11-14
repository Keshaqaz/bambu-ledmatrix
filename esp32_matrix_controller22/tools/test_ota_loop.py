#!/usr/bin/env python3

import importlib.util
import pathlib
import unittest
from unittest import mock


MODULE_PATH = pathlib.Path(__file__).with_name("ota_loop.py")
SPEC = importlib.util.spec_from_file_location("ota_loop", MODULE_PATH)
ota_loop = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ota_loop)


class WaitForNewBootTest(unittest.TestCase):
    def test_status_is_read_only_after_new_ring_entry(self):
        responses = [
            {"currentBoot": {"seq": 10}},
            {"currentBoot": {"seq": 11}},
            {"wifi": True, "fwPartition": "app1"},
        ]
        with mock.patch.object(ota_loop, "http_json", side_effect=responses) as get:
            with mock.patch.object(ota_loop.time, "sleep"):
                status, soak = ota_loop.wait_for_new_boot("pve", "matrix", 10, 2)

        self.assertEqual(status["fwPartition"], "app1")
        self.assertEqual(soak["currentBoot"]["seq"], 11)
        self.assertEqual(
            [call.args[2] for call in get.call_args_list],
            ["/api/soak", "/api/soak", "/api/status"],
        )


if __name__ == "__main__":
    unittest.main()
