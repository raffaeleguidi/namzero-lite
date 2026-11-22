#!/usr/bin/env python3
"""Test dell'interfaccia audio web: parsing aplay, validazione e API.

    python3 test/audio_web_test.py

Non tocca il motore vero: usa un presets.json temporaneo e mock di ctl/reload.
"""
import importlib.util
import json
import os
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_server():
    path = os.path.join(ROOT, "web", "server.py")
    spec = importlib.util.spec_from_file_location("namzero_audio_server", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


server = load_server()


class AplayParseTest(unittest.TestCase):
    SAMPLE = """**** List of PLAYBACK Hardware Devices ****
card 1: XTONE [XTONE], device 0: USB Audio [USB Audio]
  Subdevices: 0/1
  Subdevice #0: subdevice #0
card 2: vc4hdmi [vc4-hdmi], device 0: MAI PCM i2s-hifi-0 [MAI PCM i2s-hifi-0]
  Subdevices: 1/1
  Subdevice #0: subdevice #0
card 0: Device [Long Card Name], device 1: Some PCM [Some PCM]
  Subdevices: 1/1
"""

    def test_parse_three_cards(self):
        devices = server.parse_aplay_l(self.SAMPLE)
        self.assertEqual(len(devices), 3)
        first = devices[0]
        self.assertEqual(first["card"], 1)
        self.assertEqual(first["device"], 0)
        self.assertEqual(first["id"], "XTONE")
        self.assertEqual(first["name"], "USB Audio")
        self.assertEqual(first["pcm"], "hw:XTONE")
        self.assertEqual(first["plughw"], "plughw:XTONE")
        self.assertIn("XTONE", first["label"])
        self.assertEqual(devices[1]["pcm"], "hw:vc4hdmi")
        self.assertEqual(devices[2]["pcm"], "hw:Device,1")
        self.assertEqual(devices[2]["plughw"], "plughw:Device,1")

    def test_parse_ignores_garbage_and_empty(self):
        text = ("**** List ...\n"
                "  Subdevices: 1/1\n"
                "card senza numeri\n"
                "card 4: Foo [Foo], device 0: Bar [Bar]\n")
        devices = server.parse_aplay_l(text)
        self.assertEqual(len(devices), 1)
        self.assertEqual(devices[0]["id"], "Foo")
        self.assertEqual(server.parse_aplay_l(""), [])
        self.assertEqual(server.parse_aplay_l(None), [])

    def test_name_with_parentheses(self):
        devices = server.parse_aplay_l(
            "card 3: Foo [Foo], device 0: Bar [Bar (2)]\n")
        self.assertEqual(devices[0]["name"], "Bar")
        self.assertEqual(devices[0]["id"], "Foo")


class ValidationTest(unittest.TestCase):
    CURRENT = {"device": "hw:XTONE", "buffer": 64, "periods": 3, "input": 0}

    def test_valid(self):
        cfg = server._validate_audio(
            {"device": " plughw:XTONE ", "buffer": 128, "periods": 4, "input": 1},
            self.CURRENT)
        self.assertEqual(cfg, {"device": "plughw:XTONE", "buffer": 128,
                               "periods": 4, "input": 1})

    def test_missing_fields_keep_current(self):
        self.assertEqual(server._validate_audio({}, self.CURRENT), self.CURRENT)

    def test_device_invalid(self):
        for value in ("", "   ", "hw: XTONE", "hw:\nXTONE", "a" * 129, 3, None,
                      ["hw:XTONE"]):
            with self.assertRaises(ValueError, msg=repr(value)):
                server._validate_audio({"device": value}, self.CURRENT)

    def test_buffer_invalid(self):
        for value in (15, 2049, 0, -1, "64", 64.5, True, None):
            with self.assertRaises(ValueError, msg=repr(value)):
                server._validate_audio({"buffer": value}, self.CURRENT)

    def test_periods_invalid(self):
        for value in (1, 9, 0, "3", 3.5, False):
            with self.assertRaises(ValueError, msg=repr(value)):
                server._validate_audio({"periods": value}, self.CURRENT)

    def test_input_invalid(self):
        for value in (-1, 2, "0", 0.5, True):
            with self.assertRaises(ValueError, msg=repr(value)):
                server._validate_audio({"input": value}, self.CURRENT)

    def test_body_not_object(self):
        for value in ([], "x", None, 3):
            with self.assertRaises(ValueError):
                server._validate_audio(value, self.CURRENT)


class ConfigFileTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.tmp.name, "presets.json")
        self.old = server.PRESETS_CFG
        server.PRESETS_CFG = self.path

    def tearDown(self):
        server.PRESETS_CFG = self.old
        self.tmp.cleanup()

    def write(self, data):
        with open(self.path, "w") as f:
            json.dump(data, f)

    def test_defaults_when_missing(self):
        self.assertEqual(server.audio_config(),
                         {"device": "hw:XTONE", "buffer": 64, "periods": 3,
                          "input": 0})

    def test_read_and_clamp(self):
        self.write({"gate": {"threshold_db": -70},
                    "audio": {"device": "  plughw:XTONE  ", "buffer": 9999,
                              "periods": 1, "input": 9}})
        self.assertEqual(server.audio_config(),
                         {"device": "plughw:XTONE", "buffer": 2048,
                          "periods": 2, "input": 1})

    def test_bad_types_fall_back(self):
        self.write({"audio": {"device": 3, "buffer": "x", "periods": True,
                              "input": None}})
        self.assertEqual(server.audio_config(),
                         {"device": "hw:XTONE", "buffer": 64, "periods": 3,
                          "input": 0})

    def test_write_preserves_other_blocks_and_makes_backup(self):
        original = {"gate": {"threshold_db": -70}, "wifi": {"x": 1},
                    "slots": ["presets/vox_wah.json"]}
        self.write(original)
        with open(self.path) as f:
            before = f.read()
        server._write_audio({"device": "hw:XTONE", "buffer": 128,
                             "periods": 4, "input": 1})
        with open(self.path) as f:
            saved = json.load(f)
        self.assertEqual(saved["gate"], original["gate"])
        self.assertEqual(saved["wifi"], original["wifi"])
        self.assertEqual(saved["slots"], original["slots"])
        self.assertEqual(saved["audio"], {"device": "hw:XTONE", "buffer": 128,
                                          "periods": 4, "input": 1})
        with open(self.path + ".bak") as f:
            self.assertEqual(f.read(), before)
        self.assertFalse(os.path.exists(self.path + ".tmp"))

    def test_corrupt_file_raises(self):
        with open(self.path, "w") as f:
            f.write("{non json")
        with self.assertRaises(ValueError):
            server.audio_config()


class HttpTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.tmp.name, "presets.json")
        with open(self.path, "w") as f:
            json.dump({"gate": {"threshold_db": -70}}, f)
        server.PRESETS_CFG = self.path
        self.httpd = server.Server(("127.0.0.1", 0), server.Handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:%d" % self.httpd.server_address[1]
        self.reload_calls = []

        def fake_reload(cfg):
            self.reload_calls.append(dict(cfg))
            return {"reloaded": True,
                    "status": {"open": True, "device": cfg["device"],
                               "config_device": cfg["device"], "fallback": False,
                               "buffer": cfg["buffer"], "periods": cfg["periods"],
                               "input": cfg["input"], "rate": 48000,
                               "error": "", "boot_id": 2}}

        self.patches = [
            mock.patch.object(server, "_audio_reload", side_effect=fake_reload),
            mock.patch.object(server, "_audio_status",
                              return_value={"open": True, "device": "hw:XTONE",
                                            "config_device": "hw:XTONE",
                                            "fallback": False, "buffer": 64,
                                            "periods": 3, "input": 0,
                                            "rate": 48000, "error": "",
                                            "boot_id": 1}),
        ]
        for patch in self.patches:
            patch.start()

    def tearDown(self):
        for patch in reversed(self.patches):
            patch.stop()
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=2)
        self.tmp.cleanup()

    def request(self, method, path, data=None, raw=None):
        body = raw if raw is not None else (
            json.dumps(data).encode() if data is not None else None)
        req = urllib.request.Request(self.base + path, data=body, method=method,
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=5) as resp:
                return resp.status, json.loads(resp.read())
        except urllib.error.HTTPError as exc:
            return exc.code, json.loads(exc.read())

    def test_get_audio(self):
        status, data = self.request("GET", "/api/audio")
        self.assertEqual(status, 200)
        self.assertEqual(data["device"], "hw:XTONE")
        self.assertEqual(data["buffer"], 64)
        self.assertEqual(data["periods"], 3)
        self.assertEqual(data["input"], 0)
        self.assertEqual(data["rate"], 48000)
        self.assertEqual([i["index"] for i in data["inputs"]], [0, 1])
        self.assertTrue(data["status"]["open"])

    def test_get_devices(self):
        status, data = self.request("GET", "/api/audio/devices")
        self.assertEqual(status, 200)
        self.assertIn("devices", data)
        values = [d["pcm"] for d in data["devices"]]
        self.assertIn("default", values)
        for dev in data["devices"]:
            self.assertIn("label", dev)
            self.assertIn("id", dev)

    def test_post_invalid_does_not_write(self):
        with open(self.path) as f:
            before = f.read()
        for payload in ({"device": ""}, {"buffer": 15}, {"periods": 9},
                        {"input": 2}, {"buffer": "64"}, {"device": "hw: X"}):
            status, data = self.request("POST", "/api/audio", payload)
            self.assertEqual(status, 400, msg=repr(payload))
            self.assertIn("error", data)
        status, _ = self.request("POST", "/api/audio", raw=b"{non json")
        self.assertEqual(status, 400)
        with open(self.path) as f:
            self.assertEqual(f.read(), before)
        self.assertEqual(self.reload_calls, [])

    def test_post_valid_saves_and_reloads(self):
        status, data = self.request("POST", "/api/audio",
                                    {"device": "plughw:XTONE", "buffer": 128,
                                     "periods": 4, "input": 1})
        self.assertEqual(status, 200, msg=repr(data))
        self.assertTrue(data["ok"])
        self.assertTrue(data["reloaded"])
        with open(self.path) as f:
            saved = json.load(f)
        self.assertEqual(saved["audio"], {"device": "plughw:XTONE", "buffer": 128,
                                          "periods": 4, "input": 1})
        self.assertEqual(saved["gate"], {"threshold_db": -70})
        self.assertEqual(self.reload_calls, [{"device": "plughw:XTONE",
                                              "buffer": 128, "periods": 4,
                                              "input": 1}])


if __name__ == "__main__":
    unittest.main(verbosity=2)
