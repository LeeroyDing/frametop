"""Apple High Performance Screen Sharing displays in ft-layout, offline: no headset or Mac."""
import importlib.util
import io
import json
import pathlib
import socket
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock


def load_layout_module(tmp):
    path = pathlib.Path(__file__).resolve().parents[1] / "ft_layout.py"
    spec = importlib.util.spec_from_file_location("remote_apple_layout", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CONF_PATH = str(tmp / "frametop.conf")
    module.LAYOUT_PATH = str(tmp / "layout.json")
    return module


class RemoteAppleTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.m = load_layout_module(pathlib.Path(self.tmp.name))
        self.stream_calls = []
        self.appless_calls = []

        def fake_stream(*args, timeout=60, stdin=None):
            self.stream_calls.append((args, stdin))
            return 0, "paired", ""

        def fake_appless(*args, timeout=30, stdin=None):
            self.appless_calls.append((args, stdin))
            return 0, "credentials ready", ""

        def no_screens():
            raise self.m.NoAnswer("ft-screens isn't running")

        self.m.run_stream = fake_stream
        self.m.run_appless = fake_appless
        self.m.screens_socket = no_screens
        self.m.save_layout({"hosts": []})

    def tearDown(self):
        self.tmp.cleanup()

    def hosts(self):
        with open(self.m.LAYOUT_PATH) as f:
            return json.load(f)["hosts"]

    def test_adds_retina_virtual_display_without_moonlight_pairing(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.m.remote_command(["add", "Space-Odysseus", "192.168.1.178", "apple:virtual"])
        self.assertEqual(self.stream_calls, [])
        self.assertEqual([c[0] for c in self.appless_calls], [("check", "192.168.1.178")])
        host = self.hosts()[0]
        self.assertEqual(host["kind"], "apple")
        d = host["displays"][0]
        self.assertEqual((d["id"], d["app"], d["label"], d["size"]),
                         ("space-odysseus-virtual", "apple:virtual", "Virtual", [3840, 2160]))

    def test_login_flag_runs_appless_auth_with_stdin(self):
        out = io.StringIO()
        with mock.patch("sys.stdin", io.StringIO("leeroy\ns3cr3t\n")), redirect_stdout(out):
            self.m.remote_command(["add", "Mac", "192.168.1.178", "apple:physical", "--login"])
        self.assertEqual(self.stream_calls, [])
        self.assertEqual(self.appless_calls, [(("auth", "192.168.1.178", "--login"), "leeroy\ns3cr3t\n")])
        d = self.hosts()[0]["displays"][0]
        self.assertEqual((d["app"], d["label"], d["size"]), ("apple:physical", "Main", [1920, 1080]))

    def test_set_accepts_apple_virtual_scale_suffix_and_remove_skips_unpair(self):
        with redirect_stdout(io.StringIO()):
            self.m.remote_command(["add", "Mac", "192.168.1.178", "apple:virtual"])
        self.m.remote_command(["set", "mac-virtual", "app=apple:virtual@2", "direct.bitrate=35000"])
        self.assertEqual(self.hosts()[0]["displays"][0]["app"], "apple:virtual@2")
        self.assertEqual(self.hosts()[0]["displays"][0]["routes"], {"direct": {"bitrate": 35000}})
        with redirect_stdout(io.StringIO()):
            self.m.remote_command(["remove", "mac-virtual"])
        self.assertEqual(self.hosts()[0]["displays"], [])
        self.assertEqual(self.stream_calls, [])

    def test_host_answers_probes_port_5900_for_apple_hosts(self):
        probed = []

        class DummyConn:
            def __enter__(self):
                return self

            def __exit__(self, *_):
                return False

        def fake_connect(addr_port, timeout=1.5):
            probed.append(addr_port)
            if addr_port[1] == 5900:
                return DummyConn()
            raise OSError("closed")

        host = {"name": "Mac", "address": "mac.local", "kind": "apple", "displays": []}
        with mock.patch.object(socket, "create_connection", side_effect=fake_connect):
            self.assertTrue(self.m.host_answers(host))
        self.assertEqual(probed, [("mac.local", 5900)])

    def test_set_and_host_configure_keyboard_layout_and_keymap(self):
        with redirect_stdout(io.StringIO()):
            self.m.remote_command(["add", "Magma Llama", "Magma-Llama.local", "apple:virtual@1"])
        self.m.remote_command(["host", "Magma Llama", "kb_layout=qwerty", "keymap=leftalt:cmd"])
        self.m.remote_command(["set", "magma-llama-virtual", "kb_layout=dvorak", "keymap=caps:ctrl"])
        host = self.hosts()[0]
        disp = host["displays"][0]
        self.assertEqual(host["kb_layout"], "qwerty")
        self.assertEqual(host["keymap"], {"leftalt": "leftmeta"})
        self.assertEqual(disp["kb_layout"], "dvorak")
        self.assertEqual(disp["keymap"], {"capslock": "leftctrl"})

        listed = io.StringIO()
        with redirect_stdout(listed):
            self.m.remote_command(["list"])
        self.assertIn("kb dvorak keymap leftalt:leftmeta,capslock:leftctrl", listed.getvalue())

        self.m.remote_command(["set", "magma-llama-virtual", "kb_layout=none", "keymap="])
        self.m.remote_command(["host", "Magma Llama", "kb_layout=", "keymap=none"])
        host = self.hosts()[0]
        disp = host["displays"][0]
        self.assertNotIn("kb_layout", host)
        self.assertNotIn("keymap", host)
        self.assertNotIn("kb_layout", disp)
        self.assertNotIn("keymap", disp)

        with self.assertRaises(RuntimeError):
            self.m.remote_command(["set", "magma-llama-virtual", "kb_layout=azerty-bogus"])
        with self.assertRaises(RuntimeError):
            self.m.remote_command(["set", "magma-llama-virtual", "keymap=capslock:notakey"])


if __name__ == "__main__":
    unittest.main()
