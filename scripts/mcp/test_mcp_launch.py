"""The MCP scripts' launch never takes the foreground (game_mcp.BehindLaunch; ADR 0046 S17).

    python -B -m unittest discover -s scripts/mcp -p "test_*.py"

Over a stand-in for user32: the first window shown without activation, the foreground locked for
the start alone and let go however it ends, each window of the process kept at the bottom of the
z-order without activation on every pass (Godot raises a window it restyles), its taskbar button
kept from flashing through its first second (all again at the end), nothing done with --front or
off Windows, and both scripts' launch parsers taking --front.
"""
import os
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import editor_mcp  # noqa: E402
import game_mcp  # noqa: E402
from game_mcp import BehindLaunch  # noqa: E402

BACK = BehindLaunch.SWP_NOSIZE | BehindLaunch.SWP_NOMOVE | BehindLaunch.SWP_NOACTIVATE


class FakeUser32:
    """What the launch asks of user32, recorded; the process's windows given."""

    def __init__(self, lock_granted=True):
        self.calls = []
        self.lock_granted = lock_granted

    def LockSetForegroundWindow(self, code):  # noqa: N802 (the API's name)
        self.calls.append(("lock", code))
        return 1 if self.lock_granted or code == BehindLaunch.LSFW_UNLOCK else 0

    def SetWindowPos(self, hwnd, after, x, y, cx, cy, flags):  # noqa: N802
        self.calls.append(("pos", hwnd, after, flags))
        return 1

    def FlashWindowEx(self, info):  # noqa: N802
        flash = info._obj
        self.calls.append(("flash", flash.hwnd, flash.dwFlags))
        return 1


class Behind(BehindLaunch):
    def __init__(self, front, user32, windows):
        self.now = 0.0
        super().__init__(front, user32=user32, windows=True, clock=lambda: self.now, sleep=self.wait)
        self.shown = windows  # the windows the process has shown, as a list the test grows
        self.hidden = []  # Godot's windows not shown yet
        self.shows = {}  # hwnd -> when Godot shows it

    def windows_of(self, pid):
        return [(hwnd, True) for hwnd in self.shown] + [(hwnd, False) for hwnd in self.hidden]

    def wait(self, seconds):
        self.now += seconds
        for hwnd, at in list(self.shows.items()):
            if self.now >= at and hwnd in self.hidden:
                self.hidden.remove(hwnd)
                self.shown.append(hwnd)


class BehindLaunchTest(unittest.TestCase):
    def test_lock_for_the_start_alone(self):
        user32 = FakeUser32()
        behind = Behind(False, user32, [])
        behind.lock()
        self.assertTrue(behind.locked)
        behind.release()
        behind.release()
        self.assertEqual(user32.calls, [("lock", BehindLaunch.LSFW_LOCK), ("lock", BehindLaunch.LSFW_UNLOCK)])

    def test_a_refused_lock_is_not_let_go(self):
        user32 = FakeUser32(lock_granted=False)
        behind = Behind(False, user32, [])
        behind.lock()
        behind.release()
        self.assertEqual(user32.calls, [("lock", BehindLaunch.LSFW_LOCK)])

    def test_each_window_kept_at_the_bottom_without_activation(self):
        user32 = FakeUser32()
        windows = [101]
        behind = Behind(False, user32, windows)
        behind.tend(7)
        behind.tend(7)
        windows.append(202)
        behind.tend(7)
        pos = [call for call in user32.calls if call[0] == "pos"]
        self.assertEqual([call[1] for call in pos], [101, 101, 101, 202])
        self.assertTrue(all(call[2:] == (BehindLaunch.HWND_BOTTOM, BACK) for call in pos))

    def test_a_window_kept_from_flashing_through_its_first_second(self):
        user32 = FakeUser32()
        windows = [101]
        behind = Behind(False, user32, windows)
        behind.tend(7)
        behind.now = 0.5
        windows.append(202)
        behind.tend(7)
        behind.now = 1.2  # 101's first second is over, 202's is not
        behind.tend(7)
        behind.now = 2.0
        behind.tend(7)
        flashes = [call for call in user32.calls if call[0] == "flash"]
        self.assertEqual([call[1] for call in flashes], [101, 101, 202, 202])
        self.assertTrue(all(call[2] == BehindLaunch.FLASHW_STOP for call in flashes))
        user32.calls.clear()
        behind.tend(7, again=True)
        self.assertEqual(user32.calls, [("flash", 101, BehindLaunch.FLASHW_STOP),
                                        ("pos", 101, BehindLaunch.HWND_BOTTOM, BACK),
                                        ("flash", 202, BehindLaunch.FLASHW_STOP),
                                        ("pos", 202, BehindLaunch.HWND_BOTTOM, BACK)])

    def test_settle_waits_for_godot_to_show_its_window(self):
        user32 = FakeUser32()
        behind = Behind(False, user32, [])
        behind.GRACE_SECONDS = 0.2
        behind.hidden.append(9)
        behind.shows = {9: 0.5}
        behind.lock()
        behind.settle(7)
        # Shown at 0.5, its flashing stopped through its first second, then settled.
        self.assertGreater(behind.now, 1.5)
        self.assertLess(behind.now, 1.7)
        self.assertEqual(user32.calls[-1], ("lock", BehindLaunch.LSFW_UNLOCK))
        flashes = [call for call in user32.calls if call[0] == "flash"]
        self.assertTrue(flashes and all(call[1] == 9 for call in flashes))
        pos = [call for call in user32.calls if call[0] == "pos"]
        self.assertEqual(pos[0], ("pos", 9, BehindLaunch.HWND_BOTTOM, BACK))  # sent back before it shows

    def test_settle_never_waits_past_its_limit(self):
        user32 = FakeUser32()
        behind = Behind(False, user32, [])
        behind.hidden.append(9)  # never shown
        behind.lock()
        behind.settle(7)
        self.assertAlmostEqual(behind.now, BehindLaunch.SETTLE_LIMIT_SECONDS, delta=0.06)
        self.assertFalse(behind.locked)

    def test_settle_lets_go_even_when_tending_fails(self):
        user32 = FakeUser32()
        behind = Behind(False, user32, [5])
        behind.GRACE_SECONDS = 0.0
        behind.lock()
        behind.settle(7)
        self.assertFalse(behind.locked)
        self.assertEqual(user32.calls[-1], ("lock", BehindLaunch.LSFW_UNLOCK))
        self.assertIn(("pos", 5, BehindLaunch.HWND_BOTTOM, BACK), user32.calls)

        class Broken(Behind):
            def windows_of(self, pid):
                raise OSError("gone")

        broken_user32 = FakeUser32()
        broken = Broken(False, broken_user32, [])
        broken.GRACE_SECONDS = 0.0
        broken.lock()
        with self.assertRaises(OSError):
            broken.settle(7)
        self.assertFalse(broken.locked)

    def test_front_and_elsewhere_do_nothing(self):
        user32 = FakeUser32()
        front = Behind(True, user32, [9])
        front.lock()
        front.tend(7)
        front.settle(7)
        self.assertEqual(user32.calls, [])
        self.assertEqual(front.popen_kwargs(), {})
        self.assertFalse(BehindLaunch(False, windows=False).active)

    @unittest.skipUnless(os.name == "nt", "the start's window flags are Windows'")
    def test_the_first_window_shown_without_activation(self):
        kwargs = BehindLaunch(False).popen_kwargs()
        info = kwargs["startupinfo"]
        self.assertTrue(info.dwFlags & subprocess.STARTF_USESHOWWINDOW)
        self.assertEqual(info.wShowWindow, BehindLaunch.SW_SHOWNOACTIVATE)

    def test_both_launchers_take_front(self):
        game = game_mcp.build_parser().parse_args(["launch", "--front"])
        editor = editor_mcp.build_parser().parse_args(["launch", "--front"])
        self.assertTrue(game.front and editor.front)
        self.assertFalse(game_mcp.build_parser().parse_args(["launch"]).front)
        self.assertFalse(editor_mcp.build_parser().parse_args(["launch"]).front)


if __name__ == "__main__":
    unittest.main()
