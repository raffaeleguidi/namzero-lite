#!/usr/bin/env python3
"""Test delle decisioni WiFi e degli endpoint di stato, senza nmcli reale.

    python3 test/wifi_manager_test.py
"""
import importlib.util
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

import wifi_manager as wm  # noqa: E402


def load_server():
    path = os.path.join(ROOT, "web", "server.py")
    spec = importlib.util.spec_from_file_location("namzero_web_server", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


server = load_server()


class ConfigTest(unittest.TestCase):
    def write_config(self, data):
        handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        try:
            json.dump(data, handle)
        finally:
            handle.close()
        self.addCleanup(lambda: os.path.exists(handle.name) and os.unlink(handle.name))
        return handle.name

    def test_manager_load_config_normalizza(self):
        path = self.write_config({
            "networks": [
                {"ssid": "casa", "password": "p", "priority": "3"},
                {"ssid": "  ", "priority": 9},
                "rotta",
            ],
            "ap": {"enabled": False, "ssid": "ap", "password": "12345678"},
        })
        cfg = wm.load_config(path)
        self.assertEqual(cfg["networks"],
                         [{"ssid": "casa", "password": "p", "priority": 3}])
        self.assertFalse(cfg["ap"]["enabled"])
        self.assertEqual(cfg["ap"]["ssid"], "ap")

    def test_manager_default_ap_se_assente(self):
        path = self.write_config({"networks": []})
        cfg = wm.load_config(path)
        self.assertTrue(cfg["ap"]["enabled"])
        self.assertEqual(cfg["ap"]["ssid"], "namzero-lite")

    def test_manager_config_rotta(self):
        path = self.write_config({"networks": []})
        with open(path, "w") as f:
            f.write("{rotto")
        self.assertIsNone(wm.load_config(path))

    def test_server_wifi_config_default_e_clean(self):
        path = self.write_config({
            "networks": [{"ssid": "casa", "password": "x", "priority": 2}],
        })
        with mock.patch.object(server, "WIFI", path):
            cfg = server.wifi_config()
        self.assertEqual(cfg["networks"][0]["ssid"], "casa")
        self.assertTrue(cfg["ap"]["enabled"])
        self.assertEqual(cfg["ap"]["ssid"], "namzero-lite")

    def test_server_clean_ap(self):
        current = {"enabled": True, "ssid": "ap", "password": "12345678"}
        self.assertEqual(server._clean_ap({"ssid": "altro"}, current)["ssid"],
                         "altro")
        with self.assertRaises(ValueError):
            server._clean_ap({"password": "corta"}, current)
        with self.assertRaises(ValueError):
            server._clean_ap({"ssid": ""}, current)
        self.assertFalse(server._clean_ap({"enabled": False}, current)["enabled"])


class ParsingTest(unittest.TestCase):
    def test_split_terse_colon(self):
        self.assertEqual(wm.split_terse(r"*:my\:ssid:80:WPA2"),
                         ["*", "my:ssid", "80", "WPA2"])

    def test_split_terse_backslash(self):
        self.assertEqual(wm.split_terse(r" :a\\b:70:WPA2"),
                         [" ", "a\\b", "70", "WPA2"])

    def test_split_terse_semplice(self):
        self.assertEqual(wm.split_terse("x:y:z"), ["x", "y", "z"])

    def test_redact_cmd(self):
        text = wm.redact_cmd(["nmcli", "connection", "add", "wifi-sec.psk",
                              "segretissima", "password", "altrettanto"])
        self.assertNotIn("segretissima", text)
        self.assertNotIn("altrettanto", text)
        self.assertEqual(text.count("***"), 2)

    def test_validate_ap(self):
        self.assertIsNone(wm.validate_ap({"enabled": True,
                                          "ssid": "namzero-lite",
                                          "password": "namzero-lite"}))
        self.assertIsNotNone(wm.validate_ap({"enabled": True, "ssid": "x",
                                             "password": "short"}))
        self.assertIsNotNone(wm.validate_ap({"enabled": True, "ssid": "",
                                             "password": "12345678"}))
        self.assertIsNone(wm.validate_ap({"enabled": False, "ssid": "",
                                          "password": ""}))

    def test_visible_known_ordina(self):
        nets = [
            {"ssid": "a", "password": "", "priority": 1},
            {"ssid": "b", "password": "", "priority": 5},
            {"ssid": "c", "password": "", "priority": 5},
        ]
        visible = {"a": {"signal": 90}, "b": {"signal": 40}, "c": {"signal": 70}}
        self.assertEqual([n["ssid"] for n, _ in wm.visible_known(nets, visible)],
                         ["c", "b", "a"])


class DecisionTest(unittest.TestCase):
    def test_startup(self):
        self.assertEqual(wm.decide_startup(False, False, True), "start_ap")
        self.assertEqual(wm.decide_startup(False, True, True), "connect_sta")
        self.assertEqual(wm.decide_startup(True, False, True), "keep_sta")
        self.assertEqual(wm.decide_startup(False, False, False), "wait")

    def test_ap(self):
        self.assertEqual(wm.decide_ap(True), "switch_sta")
        self.assertEqual(wm.decide_ap(False), "stay_ap")

    def test_sta_con_isteresi(self):
        self.assertEqual(wm.decide_sta(False, False, True, 0), "wait")
        self.assertEqual(wm.decide_sta(False, False, True, 1), "wait")
        self.assertEqual(wm.decide_sta(False, False, True, 2), "start_ap")
        self.assertEqual(wm.decide_sta(False, True, True, 1), "reconnect_sta")
        self.assertEqual(wm.decide_sta(True, False, True, 5), "stay_sta")

    def test_selftest_interno(self):
        self.assertEqual(wm.run_selftests(), 0)


class StatusTest(unittest.TestCase):
    def setUp(self):
        handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        json.dump({"networks": [], "ap": {"enabled": True, "ssid": "ap-test",
                                          "password": "namzero-lite"}}, handle)
        handle.close()
        self.addCleanup(lambda: os.path.exists(handle.name) and os.unlink(handle.name))
        self.wifi_path = handle.name

    def status_with(self, values):
        def fake(obj_args, field, timeout=3):
            return values.get(field, "")
        with mock.patch.object(server, "WIFI", self.wifi_path), \
                mock.patch.object(server, "_nm_get", side_effect=fake):
            return server.wifi_status()

    def test_sta(self):
        status = self.status_with({
            "GENERAL.STATE": "100 (connected)",
            "GENERAL.CONNECTION": "Casa",
            "802-11-wireless.mode": "infrastructure",
            "802-11-wireless.ssid": "Casa",
            "IP4.ADDRESS": "192.168.1.5/24",
        })
        self.assertEqual(status, {"mode": "sta", "ssid": "Casa",
                                  "ip": "192.168.1.5", "ap_ssid": "ap-test"})

    def test_ap(self):
        status = self.status_with({
            "GENERAL.STATE": "100 (connected)",
            "GENERAL.CONNECTION": "namzero-lite-ap",
            "802-11-wireless.mode": "ap",
            "802-11-wireless.ssid": "namzero-lite",
            "IP4.ADDRESS": "10.42.0.1/24",
        })
        self.assertEqual(status["mode"], "ap")
        self.assertEqual(status["ssid"], "namzero-lite")
        self.assertEqual(status["ip"], "10.42.0.1")
        self.assertEqual(status["ap_ssid"], "ap-test")

    def test_ap_da_nome_connessione(self):
        status = self.status_with({
            "GENERAL.STATE": "100 (connected)",
            "GENERAL.CONNECTION": "namzero-lite-ap",
            "802-11-wireless.mode": "",
            "802-11-wireless.ssid": "namzero-lite",
            "IP4.ADDRESS": "10.42.0.1/24",
        })
        self.assertEqual(status["mode"], "ap")

    def test_none(self):
        status = self.status_with({"GENERAL.STATE": "30 (disconnected)"})
        self.assertEqual(status["mode"], "none")
        self.assertEqual(status["ssid"], "")
        self.assertEqual(status["ip"], "")
        self.assertEqual(status["ap_ssid"], "ap-test")


class DryRunTest(unittest.TestCase):
    def test_dry_run_senza_link(self):
        handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        json.dump({"networks": [{"ssid": "casa", "password": "x",
                                 "priority": 1}],
                   "ap": {"enabled": True, "ssid": "ap-test",
                          "password": "namzero-lite"}}, handle)
        handle.close()
        self.addCleanup(lambda: os.path.exists(handle.name) and os.unlink(handle.name))
        self.assertEqual(wm.main(["--config", handle.name, "--dry-run",
                                  "--simulate", "no-link"]), 0)


class DryRunNoMutationTest(unittest.TestCase):
    """Il dry-run può leggere nmcli, ma non deve mai eseguire mutazioni."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(lambda: shutil.rmtree(self.tmp, True))
        bindir = os.path.join(self.tmp, "bin")
        os.makedirs(bindir)
        self.mutations = os.path.join(self.tmp, "mutations.log")
        fake = os.path.join(bindir, "nmcli")
        with open(fake, "w") as f:
            f.write("""#!/usr/bin/env bash
case "$1" in
  -g)
    case "$2" in
      GENERAL.STATE) echo "30 (disconnected)" ;;
      *) echo "" ;;
    esac
    exit 0 ;;
  -t) exit 0 ;;
  *) echo "$@" >> "$FAKE_MUTATIONS"; exit 0 ;;
esac
""")
        os.chmod(fake, os.stat(fake).st_mode | stat.S_IXUSR)
        self.bindir = bindir
        self.config = os.path.join(self.tmp, "wifi.json")
        with open(self.config, "w") as f:
            json.dump({"networks": [{"ssid": "Casa", "password": "x",
                                     "priority": 1}],
                       "ap": {"enabled": True, "ssid": "namzero-lite",
                              "password": "namzero-lite"}}, f)

    def run_manager(self, *args):
        env = dict(os.environ)
        env["PATH"] = self.bindir + os.pathsep + env.get("PATH", "")
        env["FAKE_MUTATIONS"] = self.mutations
        return subprocess.run([sys.executable,
                               os.path.join(ROOT, "scripts", "wifi_manager.py")]
                              + list(args), env=env, capture_output=True,
                              text=True)

    def test_dry_run_auto_non_muta(self):
        proc = self.run_manager("--dry-run", "--config", self.config)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("decisione: avvierei l'AP", proc.stdout)
        self.assertFalse(os.path.exists(self.mutations),
                         "dry-run ha chiamato nmcli per modificare NM")

    def test_dry_run_simulato_non_chiama_nm(self):
        proc = self.run_manager("--dry-run", "--simulate", "no-link",
                                "--config", self.config)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertFalse(os.path.exists(self.mutations),
                         "dry-run simulato ha modificato NM")


class DaemonStartupTest(unittest.TestCase):
    """La fase di avvio decide AP/STA senza eseguire nmcli reale."""

    def setUp(self):
        handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        self.path = handle.name
        handle.close()
        self.addCleanup(lambda: os.path.exists(self.path) and os.unlink(self.path))

    def write_cfg(self, enabled=True, password="namzero-lite"):
        with open(self.path, "w") as f:
            json.dump({"networks": [], "ap": {"enabled": enabled,
                                              "ssid": "namzero-lite",
                                              "password": password}}, f)

    def run_once(self, cfg, status, initial=False):
        self.write_cfg(**cfg)
        with mock.patch.object(wm, "device_status", return_value=status), \
                mock.patch.object(wm, "initial_connect", return_value=initial), \
                mock.patch.object(wm, "start_ap", return_value=True) as start, \
                mock.patch.object(wm, "stop_ap", return_value=True) as stop, \
                mock.patch.object(wm, "verify_web", return_value=True), \
                mock.patch.object(wm.signal, "signal"):
            rc = wm.run_daemon(self.path, once=True)
        return rc, start, stop

    def test_senza_link_avvia_ap(self):
        rc, start, _stop = self.run_once(
            {}, {"mode": "none", "connection": "", "ssid": "", "ip": ""})
        self.assertEqual(rc, 0)
        start.assert_called_once()

    def test_con_link_non_avvia_ap(self):
        rc, start, _stop = self.run_once(
            {}, {"mode": "sta", "connection": "Casa", "ssid": "Casa",
                 "ip": "192.168.1.2"})
        self.assertEqual(rc, 0)
        start.assert_not_called()

    def test_ap_disabilitato_non_avvia_ap(self):
        rc, start, _stop = self.run_once(
            {"enabled": False},
            {"mode": "none", "connection": "", "ssid": "", "ip": ""})
        self.assertEqual(rc, 0)
        start.assert_not_called()

    def test_ap_attivo_verificato(self):
        rc, start, _stop = self.run_once(
            {}, {"mode": "ap", "connection": "namzero-lite-ap",
                 "ssid": "namzero-lite", "ip": "10.42.0.1"})
        self.assertEqual(rc, 0)
        start.assert_called_once()

    def test_ap_attivo_ma_disabilitato_viene_chiuso(self):
        rc, start, stop = self.run_once(
            {"enabled": False},
            {"mode": "ap", "connection": "namzero-lite-ap",
             "ssid": "namzero-lite", "ip": "10.42.0.1"})
        self.assertEqual(rc, 0)
        start.assert_not_called()
        stop.assert_called_once()

    def test_ap_attivo_con_password_invalida_lasciato(self):
        rc, start, _stop = self.run_once(
            {"password": "corta"},
            {"mode": "ap", "connection": "namzero-lite-ap",
             "ssid": "namzero-lite", "ip": "10.42.0.1"})
        self.assertEqual(rc, 0)
        start.assert_not_called()


class StartApGuardTest(unittest.TestCase):
    def test_disabilitato_non_tocca_nm(self):
        cfg = {"networks": [], "ap": {"enabled": False, "ssid": "x",
                                      "password": "12345678"}}
        with mock.patch.object(wm, "device_status") as status:
            self.assertFalse(wm.start_ap(cfg))
        status.assert_not_called()

    def test_password_corta_non_tocca_nm(self):
        cfg = {"networks": [], "ap": {"enabled": True, "ssid": "x",
                                      "password": "corta"}}
        with mock.patch.object(wm, "device_status") as status:
            self.assertFalse(wm.start_ap(cfg))
        status.assert_not_called()


if __name__ == "__main__":
    unittest.main(verbosity=2)
