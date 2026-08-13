#!/usr/bin/env python3
"""
Test the --dump-config descriptor emitted by scripts/sim_peripheral.py.

Nyxus (and other tooling) reads this JSON to learn the peripheral's
configurable parameters without hardcoding them, so the shape is a
contract: device_type, binary, and a params list of typed descriptors.
"""
import json
import subprocess
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPT = REPO_ROOT / "scripts" / "sim_peripheral.py"


class DumpConfig(unittest.TestCase):
    def _dump(self):
        out = subprocess.check_output(
            [sys.executable, str(SCRIPT), "--dump-config"], text=True)
        return json.loads(out)

    def test_top_level_shape(self):
        d = self._dump()
        self.assertEqual(d["device_type"], "ble-peripheral")
        self.assertEqual(d["binary"], "vbt-sim-peripheral")
        self.assertIsInstance(d["params"], list)

    def test_params_carry_typed_defaults(self):
        params = {p["name"]: p for p in self._dump()["params"]}
        # The socket is the one required positional.
        self.assertTrue(params["socket"]["positional"])
        self.assertTrue(params["socket"]["required"])
        # Address/UUID defaults are shown in the form a user would type,
        # not as raw bytes / ints.
        self.assertEqual(params["addr"]["default"], "c0:ff:ee:00:00:01")
        self.assertEqual(params["uuid"]["default"], "180f")
        self.assertEqual(params["name"]["default"], "SimPeri")
        self.assertEqual(params["verbose"]["type"], "bool")
        # The dump switch never advertises itself as a parameter.
        self.assertNotIn("dump_config", params)


if __name__ == "__main__":
    unittest.main()
