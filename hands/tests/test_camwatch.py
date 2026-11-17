#!/usr/bin/env python3
"""Tests for hands/ft-camwatch: its decision logic with made-up inputs (worn or not, VR apps,
remote viewers, cooldown, once per failure), following a growing log, its settings, and the
re-sync loop: counting it, its notification and button, and the desktop's return. It never
notifies or restarts anything here.

  python3 hands/tests/test_camwatch.py
"""
import importlib.machinery
import importlib.util
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
HANDS = os.path.dirname(HERE)
sys.path.insert(0, HANDS)
sys.path.insert(0, HERE)
from test_camcheck import CLOSE, FAIL, GOOD_OPEN, RESUME, RESUME_OK, START  # noqa: E402

loader = importlib.machinery.SourceFileLoader("camwatch", os.path.join(HANDS, "ft-camwatch"))
spec = importlib.util.spec_from_loader("camwatch", loader)
cw = importlib.util.module_from_spec(spec)
loader.exec_module(cw)

ON = dict(cw.DEFAULTS, CAMWATCH_AUTO_RESTART="1", CAMWATCH_IDLE_S="60", CAMWATCH_COOLDOWN_MIN="30")
OFF = dict(cw.DEFAULTS)
IDLE = {"steamvr": True, "worn": False, "vr_apps": [], "remote": []}
WORN = dict(IDLE, worn=True)
F1, F2 = "a.log@17:02:47", "b.log@18:10:00"


def kinds(acts):
    return [k for k, _ in acts]


class Decide(unittest.TestCase):
    def setUp(self):
        self.mem = cw.new_memory()

    def d(self, now, failure, env=IDLE, conf=ON):
        return cw.decide(now, failure, env, self.mem, conf)

    def test_nothing_wrong(self):
        self.assertEqual(self.d(0, "", {}), [])
        self.assertEqual(self.mem["pending"], "")

    def test_notify_once(self):
        acts = self.d(0, F1, WORN, OFF)
        self.assertEqual(kinds(acts), ["log", "notify", "log"])
        self.assertIn("CAMWATCH_AUTO_RESTART=1", acts[2][1])
        self.assertEqual(self.d(2, F1, WORN, OFF), [])          # same failure, same reason: quiet
        self.assertEqual(self.d(4, F1, IDLE, OFF), [])          # off: never restarts, whatever the state
        self.assertEqual(self.d(1000, F1, IDLE, OFF), [])

    def test_notify_off(self):
        acts = self.d(0, F1, WORN, dict(OFF, CAMWATCH_NOTIFY="0"))
        self.assertNotIn("notify", kinds(acts))

    def test_never_while_worn(self):
        self.d(0, F1, WORN)
        for t in range(2, 4000, 2):
            self.assertNotIn("restart", kinds(self.d(t, F1, WORN)))
        self.assertIn("worn", self.mem["why"])

    def test_restart_after_idle_then_once(self):
        self.d(0, F1, WORN)
        self.assertNotIn("restart", kinds(self.d(10, F1, IDLE)))   # just taken off: wait
        self.assertIn("waiting", self.mem["why"])
        self.assertNotIn("restart", kinds(self.d(69, F1, IDLE)))
        acts = self.d(70, F1, IDLE)
        self.assertEqual(kinds(acts), ["log", "restart"])
        self.assertEqual(acts[1][1], F1)
        # the same failure again (the new XRService hasn't logged yet, or failed the same way)
        acts = self.d(72, F1, IDLE)
        self.assertNotIn("restart", kinds(acts))
        self.assertIn("restart the headset", self.mem["why"])
        for t in range(74, 10000, 500):
            self.assertNotIn("restart", kinds(self.d(t, F1, IDLE)))

    def test_put_on_resets_the_wait(self):
        self.d(0, F1, IDLE)
        self.d(50, F1, WORN)                                        # back on at 50 s
        self.assertNotIn("restart", kinds(self.d(70, F1, IDLE)))   # off again: the 60 s start over
        self.assertNotIn("restart", kinds(self.d(129, F1, IDLE)))
        self.assertIn("restart", kinds(self.d(131, F1, IDLE)))

    def test_cooldown(self):
        self.d(0, F1, IDLE)
        self.assertIn("restart", kinds(self.d(60, F1, IDLE)))
        # The restart's new XRService fails too: a new failure, but within the cooldown.
        self.d(90, "", IDLE)
        acts = self.d(120, F2, IDLE)
        self.assertIn("notify", kinds(acts))
        self.assertNotIn("restart", kinds(acts))
        self.assertNotIn("restart", kinds(self.d(60 + 30 * 60 - 2, F2, IDLE)))
        self.assertIn("cooldown", self.mem["why"])
        self.assertIn("restart", kinds(self.d(60 + 30 * 60, F2, IDLE)))

    def test_vr_app_remote_and_no_steamvr(self):
        for env, word in ((dict(IDLE, vr_apps=["AppId=450390"]), "VR app"),
                          (dict(IDLE, remote=["0A00000B:D2F0"]), "remote desktop"),
                          (dict(IDLE, steamvr=False), "SteamVR isn't running")):
            with self.subTest(word=word):
                self.mem = cw.new_memory()
                for t in range(0, 600, 2):
                    self.assertNotIn("restart", kinds(self.d(t, F1, env)))
                self.assertIn(word, self.mem["why"])
                # once it's gone (and the headset still off), the restart happens
                self.assertIn("restart", kinds(self.d(600, F1, IDLE)))

    def test_resolved(self):
        self.d(0, F1, WORN)
        acts = self.d(10, "", {})
        self.assertEqual(kinds(acts), ["log"])
        self.assertIn("no longer current", acts[0][1])
        self.assertEqual(self.mem["pending"], "")
        self.assertEqual(self.d(12, "", {}), [])

    def test_memory_round_trip(self):
        tmp = tempfile.mkdtemp(prefix="camwatch-test-")
        try:
            path = os.path.join(tmp, "state", "camwatch.json")
            self.d(0, F1, IDLE)
            self.d(60, F1, IDLE)
            cw.save_memory(self.mem, path)
            mem = cw.load_memory(path)
            self.assertEqual(mem["restarted"], {F1: 60})
            self.assertEqual(mem["notified"], [F1])
            # the watcher restarted: the same failure neither notifies nor restarts again
            acts = cw.decide(200, F1, IDLE, mem, ON)
            self.assertNotIn("notify", kinds(acts))
            self.assertNotIn("restart", kinds(acts))
            self.assertEqual(cw.load_memory(os.path.join(tmp, "missing.json"))["restarted"], {})
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


class Follow(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="camwatch-follow-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_growing_log(self):
        path = os.path.join(self.tmp, "x.log")
        f = cw.Follower(path)
        text = "\n".join(START + GOOD_OPEN) + "\n"
        with open(path, "w") as out:
            out.write(text)
        self.assertEqual(cw.current_failure(f.poll()), "")
        self.assertEqual(f.state.verdict()[0], "ok")
        # half a line, then the rest: read as one line
        more = "\n".join(CLOSE + RESUME + FAIL) + "\n"
        cut = more.index("Failed to load VCINT") + 5
        with open(path, "a") as out:
            out.write(more[:cut])
        self.assertEqual(cw.current_failure(f.poll()), "")
        with open(path, "a") as out:
            out.write(more[cut:])
        self.assertTrue(cw.current_failure(f.poll()).endswith("@11:30:01"))
        self.assertEqual(cw.current_failure(f.state, steamvr_running=False), "")
        with open(path, "a") as out:
            out.write("\n".join(CLOSE + RESUME_OK) + "\n")
        self.assertEqual(cw.current_failure(f.poll()), "")

    def test_new_log_starts_afresh(self):
        a, b = os.path.join(self.tmp, "a.log"), os.path.join(self.tmp, "b.log")
        with open(a, "w") as out:
            out.write("\n".join(START + GOOD_OPEN + CLOSE + RESUME + FAIL) + "\n")
        f = cw.Follower(a)
        self.assertTrue(cw.current_failure(f.poll()))
        with open(b, "w") as out:
            out.write("\n".join(START + GOOD_OPEN) + "\n")
        f.fixed = b   # as when xrservice.txt points at a new XRService's log
        self.assertEqual(cw.current_failure(f.poll()), "")
        self.assertEqual(f.path, b)


SYNC = ["Tue Nov 17 2026 11:03:22.201636 INFO: Sync done fully for cam upper_right",
        "Tue Nov 17 2026 11:03:22.201758 INFO: Sync done fully for cam upper_left"]
UNSYNC = ["Tue Nov 17 2026 11:03:22.049885 INFO: Set upper camera driver not in-sync"]
INSYNC = ["Tue Nov 17 2026 11:03:22.201795 INFO: Set upper camera driver in-sync"]


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


class SyncCount(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="camwatch-sync-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_window(self):
        loop = cw.SyncLoop()
        loop.add(0, 40)
        loop.add(30, 70)
        loop.add(31, 0)                      # nothing read: nothing kept
        self.assertEqual(len(loop.seen), 2)
        self.assertEqual(loop.rate(30), 110)
        self.assertEqual(loop.rate(60), 110)   # the window is the last 60 s, both ends in
        self.assertEqual(loop.rate(61), 70)
        self.assertEqual(loop.rate(91), 0)

    def test_follower_counts_only_what_grows(self):
        path = os.path.join(self.tmp, "x.log")
        clock = Clock()
        with open(path, "w") as out:        # a log that already holds a loop: says nothing about now
            out.write("\n".join(START + GOOD_OPEN + (UNSYNC + SYNC) * 300) + "\n")
        f = cw.Follower(path, clock)
        f.poll()
        self.assertEqual(f.sync_rate(), 0)
        self.assertEqual(f.state.verdict()[0], "ok")   # the re-syncs don't change camcheck's verdict
        clock.t += 2
        with open(path, "a") as out:        # a healthy wake: one re-sync of the two upper cameras
            out.write("\n".join(UNSYNC + SYNC + INSYNC) + "\n")
        f.poll()
        self.assertEqual(f.sync_rate(), 2)
        for _ in range(10):                 # the loop: 60 lines every 2 s
            clock.t += 2
            with open(path, "a") as out:
                out.write("\n".join((UNSYNC + SYNC) * 30) + "\n")
            f.poll()
        self.assertEqual(f.sync_rate(), 602)
        clock.t += 61                       # the cameras stopped (standby): it runs out
        f.poll()
        self.assertEqual(f.sync_rate(), 0)

    def test_new_log_starts_the_count_afresh(self):
        a, b = os.path.join(self.tmp, "a.log"), os.path.join(self.tmp, "b.log")
        clock = Clock()
        with open(a, "w") as out:
            out.write("\n".join(START + GOOD_OPEN) + "\n")
        f = cw.Follower(a, clock)
        f.poll()
        with open(a, "a") as out:
            out.write("\n".join(SYNC * 200) + "\n")
        f.poll()
        self.assertEqual(f.sync_rate(), 400)
        with open(b, "w") as out:           # SteamVR restarted: its start's own re-syncs don't count
            out.write("\n".join(START + GOOD_OPEN + SYNC * 3) + "\n")
        f.fixed = b
        f.poll()
        self.assertEqual(f.sync_rate(), 0)


class DecideSync(unittest.TestCase):
    def setUp(self):
        self.mem = cw.new_memory()

    def d(self, now, rate, note_up=False, conf=OFF):
        return cw.decide_sync(now, rate, note_up, self.mem, conf)

    def test_healthy_wakes_are_quiet(self):
        for now, rate in ((1000, 0), (1002, 6), (1004, 40), (1006, 99)):
            self.assertEqual(self.d(now, rate), [])
        self.assertIsNone(self.mem["sync_since"])

    def test_begins_notifies_once_and_ends(self):
        acts = self.d(1000, 230)
        self.assertEqual(kinds(acts), ["log", "notify_sync"])
        self.assertIn("230 re-syncs", acts[0][1])
        self.assertEqual(self.d(1002, 700, note_up=True), [])     # showing: nothing more
        self.assertEqual(self.d(1004, 60, note_up=True), [])      # slower, still over a quarter: still on
        acts = self.d(1006, 24, note_up=True)                     # under a quarter: over
        self.assertEqual(kinds(acts), ["log"])
        self.assertIn("stopped", acts[0][1])
        self.assertIsNone(self.mem["sync_since"])
        self.assertEqual(self.d(1008, 60), [])                    # and 60 doesn't begin one

    def test_again_only_once_closed_and_later(self):
        self.assertEqual(kinds(self.d(1000, 300)), ["log", "notify_sync"])
        self.assertEqual(self.d(1100, 300, note_up=False), [])            # closed, but only 100 s ago
        self.assertEqual(self.d(1700, 300, note_up=True), [])             # 10 min on, but still showing
        self.assertEqual(kinds(self.d(1700, 300, note_up=False)), ["notify_sync"])
        self.assertEqual(self.d(1702, 300, note_up=False), [])

    def test_a_new_loop_within_the_gap_waits(self):
        self.d(1000, 300)
        self.d(1100, 0)                                                   # standby: it stopped
        self.assertEqual(kinds(self.d(1200, 300)), ["log"])               # back on: logged, no notification yet
        self.assertEqual(kinds(self.d(1600, 300)), ["notify_sync"])

    def test_settings(self):
        self.assertEqual(kinds(self.d(1000, 300, conf=dict(OFF, CAMWATCH_NOTIFY="0"))), ["log"])
        self.mem = cw.new_memory()
        self.assertEqual(self.d(1000, 5000, conf=dict(OFF, CAMWATCH_SYNC_PER_MIN="0")), [])
        self.assertEqual(kinds(self.d(1000, 30, conf=dict(OFF, CAMWATCH_SYNC_PER_MIN="20"))), ["log", "notify_sync"])


class RelaunchWhen(unittest.TestCase):
    def test_waits_for_the_new_steamvr_to_settle(self):
        r = cw.Relaunch(111, 0.0)
        self.assertEqual(r.step(1, 111, True), "wait")      # the old SteamVR, still going down
        self.assertEqual(r.step(3, None, False), "wait")
        self.assertEqual(r.step(20, 222, False), "wait")    # the new XRService, no compositor yet
        self.assertEqual(r.step(30, 222, True), "wait")     # up: now 5 s more
        self.assertEqual(r.step(34, 222, True), "wait")
        self.assertEqual(r.step(35, 222, True), "start")

    def test_a_wobble_starts_the_settling_again(self):
        r = cw.Relaunch(111, 0.0)
        self.assertEqual(r.step(30, 222, True), "wait")
        self.assertEqual(r.step(33, 222, False), "wait")
        self.assertEqual(r.step(36, 222, True), "wait")
        self.assertEqual(r.step(40, 222, True), "wait")
        self.assertEqual(r.step(41, 222, True), "start")

    def test_gives_up(self):
        r = cw.Relaunch(111, 0.0)
        self.assertEqual(r.step(180, None, False), "wait")
        self.assertEqual(r.step(181, None, False), "gave up")
        self.assertEqual(cw.Relaunch(None, 0.0).step(5, 222, True), "wait")   # no old pid known: any XRService
        r = cw.Relaunch(None, 0.0)
        r.step(5, 222, True)
        self.assertEqual(r.step(10, 222, True), "start")


class Alert(unittest.TestCase):
    """SyncAlert.step with stand-ins: what it posts, what a press does, when the desktop returns."""

    def setUp(self):
        self.mem = cw.new_memory()
        self.calls = []
        self.note_state = None          # None: showing; True: its button pressed; False: closed
        self.xr, self.up, self.desktop_running = 111, True, True
        self.alert = cw.SyncAlert(do=self.do())

    def do(self):
        def post(conf):
            self.calls.append("post")
            self.note_state = None
            return object() if self.desktop_running else None
        return {"post": post, "pressed": lambda note: self.note_state,
                "drop": lambda note: self.calls.append("drop"),
                "restart": lambda: self.calls.append("restart") or True,
                "desktop": lambda: self.calls.append("desktop") or True,
                "xr_pid": lambda: self.xr, "up": lambda: self.up,
                "has_desktop": lambda: self.desktop_running}

    def step(self, t, rate, log="a.log", conf=OFF):
        self.alert.step(1000.0 + t, t, rate, log, self.mem, conf)

    def test_press_restarts_and_brings_the_desktop_back(self):
        self.step(0, 0)
        self.step(2, 300)
        self.assertEqual(self.calls, ["post"])
        self.step(4, 300)
        self.assertEqual(self.calls, ["post"])              # showing: no second one
        self.note_state = True
        self.step(6, 300)
        self.assertEqual(self.calls, ["post", "restart"])
        self.assertIsNone(self.alert.note)
        self.xr, self.up = None, False                      # SteamVR going down, and its log with it
        self.step(8, 0, log="b.log")
        self.xr, self.up = 222, True
        self.step(30, 0, log="b.log")
        self.step(34, 0, log="b.log")
        self.assertEqual(self.calls, ["post", "restart"])
        self.step(35, 3, log="b.log")
        self.assertEqual(self.calls, ["post", "restart", "desktop"])
        self.step(37, 0, log="b.log")
        self.assertEqual(self.calls, ["post", "restart", "desktop"])   # once

    def test_closed_without_the_button(self):
        self.step(0, 300)
        self.note_state = False
        self.step(2, 300)
        self.assertEqual(self.calls, ["post"])
        self.step(300, 300)
        self.assertEqual(self.calls, ["post"])              # not again within 10 min
        self.step(602, 300)
        self.assertEqual(self.calls, ["post", "post"])

    def test_no_desktop_tries_again_later(self):
        self.desktop_running = False
        self.step(0, 300)
        self.step(10, 300)
        self.assertEqual(self.calls, ["post"])
        self.step(31, 300)
        self.assertEqual(self.calls, ["post", "post"])
        self.desktop_running = True
        self.step(62, 300)
        self.assertEqual(self.calls, ["post"] * 3)
        self.step(64, 300)
        self.assertEqual(self.calls, ["post"] * 3)          # it's showing now

    def test_a_stale_notification_cant_restart_the_next_steamvr(self):
        self.step(0, 300)
        self.step(2, 0, log="b.log")                        # SteamVR was restarted some other way
        self.assertEqual(self.calls, ["post", "drop"])
        self.note_state = True                              # a late press goes nowhere
        self.step(4, 0, log="b.log")
        self.assertEqual(self.calls, ["post", "drop"])
        self.step(6, 300, log="b.log")                      # and a loop in the new one notifies at once
        self.assertEqual(self.calls, ["post", "drop", "post"])

    def test_desktop_left_to_autostart_or_to_you(self):
        for conf in (dict(OFF, AUTOSTART="1"), dict(OFF, CAMWATCH_RESTART_DESKTOP="0")):
            self.setUp()
            self.step(0, 300, conf=conf)
            self.note_state = True
            self.step(2, 300, conf=conf)
            self.assertEqual(self.calls, ["post", "restart"])
            self.assertIsNone(self.alert.relaunch)
        self.assertIn("starts again by itself", cw.sync_text(dict(OFF, AUTOSTART="1")))
        self.assertIn("starts again by itself", cw.sync_text(OFF))
        self.assertNotIn("starts again by itself", cw.sync_text(dict(OFF, CAMWATCH_RESTART_DESKTOP="0")))

    def test_dry_run_posts_nothing(self):
        self.alert = cw.SyncAlert(dry=True, do=self.do())
        self.step(0, 300)
        self.step(2, 300)
        self.assertEqual(self.calls, [])


class Settings(unittest.TestCase):
    def test_read_conf(self):
        tmp = tempfile.mkdtemp(prefix="camwatch-conf-")
        try:
            path = os.path.join(tmp, "frametop.conf")
            with open(path, "w") as f:
                f.write("# Frametop\nREMOTE=1   # comment\nCAMWATCH_AUTO_RESTART=1\nCAMWATCH_IDLE_S='90'\n"
                        "CAMWATCH_IGNORE_APPIDS=\"1, 2\"\nnot a setting\n")
            conf = cw.read_conf(path)
            self.assertEqual(conf["CAMWATCH_AUTO_RESTART"], "1")
            self.assertEqual(cw.conf_int(conf, "CAMWATCH_IDLE_S"), 90)
            self.assertEqual(conf["CAMWATCH_IGNORE_APPIDS"], "1, 2")
            self.assertEqual(cw.conf_int(conf, "CAMWATCH_COOLDOWN_MIN"), 30)   # the default
            self.assertEqual(cw.read_conf(os.path.join(tmp, "none"))["CAMWATCH_AUTO_RESTART"], "0")
            self.assertEqual(cw.conf_int({"CAMWATCH_IDLE_S": "soon"}, "CAMWATCH_IDLE_S"), 60)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_headset_worn(self):
        tmp = tempfile.mkdtemp(prefix="camwatch-bl-")
        orig = cw.process_running
        try:
            os.makedirs(os.path.join(tmp, "panel0"))
            bright = os.path.join(tmp, "panel0", "brightness")
            for value, compositor, worn in (("95", True, True), ("0", True, False), ("95", False, False)):
                with open(bright, "w") as f:
                    f.write(value + "\n")
                cw.process_running = lambda name, c=compositor: c and name == "vrcompositor"
                self.assertEqual(cw.headset_worn(tmp), worn, (value, compositor))
            self.assertFalse(cw.headset_worn(os.path.join(tmp, "none")))
        finally:
            cw.process_running = orig
            shutil.rmtree(tmp, ignore_errors=True)

    def test_remote_viewers(self):
        tmp = tempfile.mkdtemp(prefix="camwatch-tcp-")
        try:
            path = os.path.join(tmp, "tcp")
            with open(path, "w") as f:
                f.write("  sl  local_address rem_address   st tx_queue rx_queue\n"
                        "   0: 0100007F:170C 00000000:0000 0A 00000000:00000000\n"      # listening
                        "   1: 0B00640A:170C 0C00640A:D2F0 01 00000000:00000000\n"      # a viewer
                        "   2: 0B00640A:0016 0C00640A:D2F1 01 00000000:00000000\n")     # ssh
            self.assertEqual(cw.remote_viewers(5900, (path,)), ["0C00640A:D2F0"])
            self.assertEqual(cw.remote_viewers(5901, (path,)), [])
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
