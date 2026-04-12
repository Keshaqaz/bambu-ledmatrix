#!/usr/bin/env python3
import time
import unittest

import relay


class RelayStateTest(unittest.TestCase):
    def test_map_lifecycle(self):
        self.assertEqual(relay.map_lifecycle("RUNNING", False), "printing")
        self.assertEqual(relay.map_lifecycle("PREPARE", False), "preparing")
        self.assertEqual(relay.map_lifecycle("PAUSE", False), "paused")
        self.assertEqual(relay.map_lifecycle("FINISH", False), "finished")
        self.assertEqual(relay.map_lifecycle("IDLE", False), "idle")
        self.assertEqual(relay.map_lifecycle("RUNNING", True), "error")

    def test_partial_packet_does_not_wipe_state(self):
        st = relay.RelayState("SERIAL", "test")
        st.update_from_print({
            "gcode_state": "RUNNING",
            "mc_percent": 42,
            "subtask_name": "benchy.3mf",
            "layer_num": 3,
            "total_layer_num": 12,
        })
        before = st.get_payload()
        st.update_from_print({"sequence_id": "2"})
        after = st.get_payload()
        self.assertEqual(after["lifecycle"], before["lifecycle"])
        self.assertEqual(after["progress"], before["progress"])
        self.assertEqual(after["jobName"], before["jobName"])

    def test_finished_demotes_to_idle_after_grace_period(self):
        st = relay.RelayState("SERIAL", "test")
        st.update_from_print({"gcode_state": "FINISH", "mc_percent": 100})
        self.assertEqual(st.get_payload()["lifecycle"], "finished")
        st.finished_since = time.time() - 31
        payload = st.get_payload()
        self.assertEqual(payload["lifecycle"], "idle")
        self.assertEqual(payload["progress"], 0.0)

    def test_malformed_numeric_fields_are_safe(self):
        st = relay.RelayState("SERIAL", "test")
        st.update_from_print({
            "gcode_state": "RUNNING",
            "mc_percent": "bad-progress",
            "stg_cur": "bad-stage",
            "layer_num": "bad-layer",
            "total_layer_num": "bad-total",
            "mc_remaining_time": "bad-remaining",
        })
        payload = st.get_payload()
        self.assertEqual(payload["lifecycle"], "printing")
        self.assertEqual(payload["currentLayer"], 0)
        self.assertEqual(payload["totalLayers"], 0)
        self.assertEqual(payload["remainingSeconds"], 0)

    def test_running_stage_255_uses_lifecycle_stage(self):
        st = relay.RelayState("SERIAL", "test")
        st.update_from_print({
            "gcode_state": "RUNNING",
            "mc_percent": "0.51",
            "stg_cur": 255,
            "mc_remaining_time": "13",
        })
        payload = st.get_payload()
        self.assertEqual(payload["lifecycle"], "printing")
        self.assertEqual(payload["status"], "RUNNING")
        self.assertEqual(payload["stage"], "printing")
        self.assertEqual(payload["progress"], 51.0)
        self.assertEqual(payload["remainingSeconds"], 780)


if __name__ == "__main__":
    unittest.main()
