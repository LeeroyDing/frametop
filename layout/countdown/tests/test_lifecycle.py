"""Tests only the fake-OpenVR binary, never the real helper or headset services."""
import os
import pathlib
import signal
import shlex
import subprocess
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parent
COMPONENT = ROOT.parent


def font_path():
    """Use a local font for offline rendering; never download or change fonts."""
    explicit = os.environ.get("FT_COUNTDOWN_TEST_FONT")
    if explicit:
        path = pathlib.Path(explicit).expanduser()
        if not path.is_file():
            raise RuntimeError("FT_COUNTDOWN_TEST_FONT does not name a file")
        return path
    for name in (
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
    ):
        path = pathlib.Path(name)
        if path.is_file():
            return path
    raise RuntimeError("Set FT_COUNTDOWN_TEST_FONT to an installed TrueType font")


class LifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (COMPONENT / "build").mkdir(exist_ok=True)
        cls.build = tempfile.TemporaryDirectory(dir=COMPONENT / "build", prefix="test-fake-openvr-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.binary = pathlib.Path(cls.build.name) / "ft-timer-test"
        cls.font = font_path()
        stb = pathlib.Path(os.environ.get("STB_INCLUDE_DIR", str(COMPONENT / "build" / "include"))).expanduser()
        for name in ("stb_truetype.h", "stb_image_write.h"):
            if not (stb / name).is_file():
                raise RuntimeError(f"Missing {stb / name}; run layout/countdown/deps.sh or set STB_INCLUDE_DIR")
        # The test shim is first in the include search path. No OpenVR library
        # is linked; this binary cannot connect to SteamVR or a headset.
        command = shlex.split(os.environ.get("CXX", "c++")) + [
            "-std=c++17", "-O2", "-Wall", "-Wextra", "-pthread",
            "-I", str(ROOT / "cpp"), "-I", str(stb),
            str(COMPONENT / "ft-reset-timer.cpp"),
            "-o", str(cls.binary),
        ]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError("Fake-OpenVR compilation failed:\n" + result.stdout + result.stderr)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="countdown-lifecycle-")
        self.addCleanup(self.temp.cleanup)
        self.trace = pathlib.Path(self.temp.name) / "trace"
        self.env = {**os.environ, "COUNTDOWN_TRACE": str(self.trace)}
        self.command = [str(self.binary), "--font", str(self.font)]

    def lines(self):
        return self.trace.read_text().splitlines() if self.trace.exists() else []

    def deadline(self, seconds):
        # Match the helper's explicit clock on macOS too; production ft-layout
        # uses time.monotonic(), which is CLOCK_MONOTONIC on the Linux host.
        return ["--deadline", f"{time.clock_gettime(time.CLOCK_MONOTONIC) + seconds:.9f}"]

    def test_success_shows_three_seconds_and_cleans_up(self):
        start = time.monotonic()
        result = subprocess.run(self.command, env=self.env, capture_output=True, timeout=6)
        elapsed = time.monotonic() - start
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertGreaterEqual(elapsed, 3)
        self.assertLess(elapsed, 4)
        lines = self.lines()
        self.assertEqual(lines.count("upload"), 3)
        self.assertEqual(lines.count("show"), 1)
        self.assertEqual(lines[-3:], ["hide", "destroy", "shutdown"])
        for required in ("no-input", "non-interactive", "head-relative", "width-0.4"):
            self.assertIn(required, lines)
        self.assertTrue(any(line.startswith("frametop.guide.reset-countdown.") for line in lines))

    def test_notice_shows_once_for_its_seconds_and_cleans_up(self):
        command = self.command + ["--notice", "Hand tracking is slow", "--line", "The cameras are out of sync.",
                                  "--line", "Restart SteamVR to fix it.", "--seconds", "1"]
        start = time.monotonic()
        result = subprocess.run(command, env=self.env, capture_output=True, timeout=5)
        elapsed = time.monotonic() - start
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertGreaterEqual(elapsed, 1)
        self.assertLess(elapsed, 2)
        lines = self.lines()
        self.assertEqual(lines.count("upload"), 1)
        self.assertEqual(lines.count("show"), 1)
        self.assertEqual(lines[-3:], ["hide", "destroy", "shutdown"])
        for required in ("no-input", "non-interactive", "head-relative", "width-0.4"):
            self.assertIn(required, lines)
        # frametop.guide*: the pointer's rays pass through it, as through the countdown.
        self.assertTrue(any(line.startswith("frametop.guide.notice.") for line in lines))

    def test_notice_renders_offline_and_respects_its_deadline(self):
        out = pathlib.Path(self.temp.name) / "render"
        notice = ["--notice", "A title far too long to fit the panel at its own size, so it is drawn smaller",
                  "--line", "One line."]
        result = subprocess.run(self.command + notice + ["--render-dir", str(out)], env=self.env,
                                capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.lines(), [])                       # offline: OpenVR never starts
        self.assertEqual((out / "notice.png").read_bytes()[:8], b"\x89PNG\r\n\x1a\n")
        result = subprocess.run(self.command + notice + self.deadline(-1), env=self.env,
                                capture_output=True, timeout=2)
        self.assertEqual(result.returncode, 3, result.stderr)   # a late start shows nothing
        self.assertEqual(self.lines(), [])

    def test_notice_options_are_checked(self):
        for extra in (["--line", "no notice"], ["--seconds", "5"], ["--notice", "t", "--seconds", "0"],
                      ["--notice", "t", "--seconds", "61"], ["--notice", "t", "--check"],
                      ["--notice", "t"] + ["--line", "x"] * 5):
            with self.subTest(extra=extra):
                result = subprocess.run(self.command + extra, env=self.env, capture_output=True, timeout=3)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(self.lines(), [])

    def test_signal_hides_and_destroys(self):
        p = subprocess.Popen(self.command, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 2
            while "show" not in self.lines() and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertIn("show", self.lines())
            p.send_signal(signal.SIGTERM)
            p.communicate(timeout=2)
            self.assertEqual(p.returncode, 3)
            self.assertEqual(self.lines()[-3:], ["hide", "destroy", "shutdown"])
        finally:
            if p.poll() is None:
                p.kill()
                p.communicate()

    def test_upload_failure_still_cleans_up(self):
        result = subprocess.run(self.command, env={**self.env, "COUNTDOWN_FAIL": "upload"}, capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("show", self.lines())
        self.assertEqual(self.lines()[-3:], ["hide", "destroy", "shutdown"])

    def test_initialization_failure_does_not_show(self):
        result = subprocess.run(self.command, env={**self.env, "COUNTDOWN_FAIL": "init"}, capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.lines(), ["init"])

    def test_offline_check_never_initializes_openvr(self):
        result = subprocess.run(self.command + ["--check"], env=self.env, capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(self.lines(), [])

    def test_expired_deadline_never_initializes_openvr(self):
        result = subprocess.run(self.command + self.deadline(-1), env=self.env,
                                capture_output=True, timeout=2)
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertEqual(self.lines(), [])

    def test_runtime_deadline_hides_and_destroys_visible_overlay(self):
        env = {**self.env, "COUNTDOWN_EXPIRE_ON": "poll"}
        result = subprocess.run(self.command + self.deadline(60), env=env,
                                capture_output=True, timeout=2)
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertIn("show", self.lines())
        self.assertEqual(self.lines().count("upload"), 1)
        self.assertEqual(self.lines()[-3:], ["hide", "destroy", "shutdown"])

    def test_deadline_during_openvr_setup_does_not_show_stale_overlay(self):
        for operation in ("init", "create", "upload"):
            with self.subTest(operation=operation):
                self.trace.unlink(missing_ok=True)
                env = {**self.env, "COUNTDOWN_EXPIRE_ON": operation}
                result = subprocess.run(self.command + self.deadline(60), env=env,
                                        capture_output=True, timeout=2)
                self.assertEqual(result.returncode, 3, result.stderr)
                self.assertNotIn("show", self.lines())
                if operation == "init":
                    self.assertEqual(self.lines(), ["init", "shutdown"])
                else:
                    self.assertEqual(self.lines()[-3:], ["hide", "destroy", "shutdown"])

    def test_invalid_deadline_is_rejected_without_openvr(self):
        for value in ("nan", "inf", "-1", "123suffix"):
            with self.subTest(value=value):
                result = subprocess.run(self.command + ["--deadline", value], env=self.env,
                                        capture_output=True, timeout=2)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(self.lines(), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
