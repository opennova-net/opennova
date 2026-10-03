"""The MCP launch starts behind every other window and never takes the foreground (game_mcp.BehindLaunch).

    python -B -m unittest discover -s scripts/mcp -p "test_*.py"

Over a stand-in for user32: the launcher's foreground rights read before the start, the lock taken
(waited for, with rights, while another launch holds it) and taken again on every pass, the first
window shown without activation, each window kept at the bottom of the z-order without activation
on every pass (Godot raises a window it restyles), posted so the launcher never waits on the game,
its taskbar button kept from flashing through its first second (all again at the end), the lock
let go however the start ends, a window the user brought up left alone, nothing done with --front
or off Windows, and `launch --front` and `run` parsed.
"""
import os
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import game_mcp  # noqa: E402
from game_mcp import BehindLaunch  # noqa: E402

BACK = (BehindLaunch.SWP_NOSIZE | BehindLaunch.SWP_NOMOVE | BehindLaunch.SWP_NOACTIVATE
        | BehindLaunch.SWP_ASYNCWINDOWPOS)  # posted: the launcher never waits on the game
GAME = 7  # the game's pid
LAUNCHER = 3  # the launcher's own pid


class FakeUser32:
    """What the launch asks of user32, recorded; the foreground and the lock given."""

    def __init__(self, rights=True, lock_free_after=0):
        self.calls = []
        self.rights = rights
        self.lock_free_after = lock_free_after  # LSFW_LOCK refused this many times (another launch's lock)
        self.held = False
        self.foreground_pid = 99  # the maintainer's window

    def AllowSetForegroundWindow(self, pid):  # noqa: N802 (the API's name)
        self.calls.append(("allow", pid))
        return 1 if self.rights else 0

    def LockSetForegroundWindow(self, code):  # noqa: N802
        self.calls.append(("lock", code))
        if code == BehindLaunch.LSFW_UNLOCK:
            granted, self.held = self.held, False
            return 1 if granted else 0
        if not self.rights or self.held:
            return 0
        if self.lock_free_after > 0:
            self.lock_free_after -= 1
            return 0
        self.held = True
        return 1

    def GetForegroundWindow(self):  # noqa: N802
        return 500

    def GetWindowThreadProcessId(self, hwnd, owner):  # noqa: N802
        owner._obj.value = self.foreground_pid if hwnd == 500 else GAME
        return 1

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
        super().__init__(front, user32=user32, windows=True, clock=lambda: self.now, sleep=self.wait, pid=LAUNCHER)
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


def started(user32, windows):
    """A launch prepared as cmd_launch prepares it, its calls so far cleared."""
    behind = Behind(False, user32, windows)
    behind.prepare()
    user32.calls.clear()
    return behind


class PrepareTest(unittest.TestCase):
    def test_the_rights_read_and_the_lock_taken(self):
        user32 = FakeUser32()
        behind = Behind(False, user32, [])
        behind.prepare()
        self.assertEqual(user32.calls, [("allow", LAUNCHER), ("lock", BehindLaunch.LSFW_LOCK)])
        self.assertTrue(behind.rights and behind.locked)

    def test_without_rights_nothing_waits(self):
        user32 = FakeUser32(rights=False)
        behind = Behind(False, user32, [])
        behind.prepare()
        self.assertEqual(user32.calls, [("allow", LAUNCHER), ("lock", BehindLaunch.LSFW_LOCK)])
        self.assertFalse(behind.rights or behind.locked)
        self.assertEqual(behind.now, 0.0)

    def test_with_rights_another_launch_lock_is_waited_for(self):
        user32 = FakeUser32(lock_free_after=5)
        behind = Behind(False, user32, [])
        behind.prepare()
        self.assertTrue(behind.locked)
        self.assertAlmostEqual(behind.now, 0.5)

    def test_a_lock_never_free_is_waited_for_within_the_limit(self):
        user32 = FakeUser32(lock_free_after=10 ** 6)
        behind = Behind(False, user32, [])
        behind.prepare()
        self.assertFalse(behind.locked)
        self.assertAlmostEqual(behind.now, BehindLaunch.LOCK_WAIT_SECONDS, delta=0.11)

    @unittest.skipUnless(os.name == "nt", "the start's window flags are Windows'")
    def test_the_first_window_shown_without_activation(self):
        info = BehindLaunch(False).popen_kwargs()["startupinfo"]
        self.assertTrue(info.dwFlags & subprocess.STARTF_USESHOWWINDOW)
        self.assertEqual(info.wShowWindow, BehindLaunch.SW_SHOWNOACTIVATE)


class LockTest(unittest.TestCase):
    def test_released_once(self):
        user32 = FakeUser32()
        behind = started(user32, [])
        behind.release()
        behind.release()
        self.assertEqual(user32.calls, [("lock", BehindLaunch.LSFW_UNLOCK)])

    def test_a_refused_lock_is_neither_retaken_nor_let_go(self):
        user32 = FakeUser32(rights=False)
        behind = started(user32, [5])
        behind.tend(GAME)
        behind.release()
        self.assertNotIn(("lock", BehindLaunch.LSFW_LOCK), user32.calls)
        self.assertNotIn(("lock", BehindLaunch.LSFW_UNLOCK), user32.calls)

    def test_retaken_on_every_pass_after_the_system_lets_it_go(self):
        user32 = FakeUser32()
        behind = started(user32, [])
        behind.tend(GAME)
        self.assertEqual(user32.calls, [("lock", BehindLaunch.LSFW_LOCK)])  # still held: refused
        user32.held = False  # the user switched windows
        behind.tend(GAME)
        self.assertTrue(user32.held)


class TendTest(unittest.TestCase):
    def test_each_window_kept_at_the_bottom_without_activation(self):
        user32 = FakeUser32()
        windows = [101]
        behind = started(user32, windows)
        behind.tend(GAME)
        behind.tend(GAME)
        windows.append(202)
        behind.tend(GAME)
        pos = [call for call in user32.calls if call[0] == "pos"]
        self.assertEqual([call[1] for call in pos], [101, 101, 101, 202])
        self.assertTrue(all(call[2:] == (BehindLaunch.HWND_BOTTOM, BACK) for call in pos))

    def test_a_window_kept_from_flashing_through_its_first_second(self):
        user32 = FakeUser32()
        windows = [101]
        behind = started(user32, windows)
        behind.tend(GAME)
        behind.now = 0.5
        windows.append(202)
        behind.tend(GAME)
        behind.now = 1.2  # 101's first second is over, 202's is not
        behind.tend(GAME)
        behind.now = 2.0
        behind.tend(GAME)
        flashes = [call for call in user32.calls if call[0] == "flash"]
        self.assertEqual([call[1] for call in flashes], [101, 101, 202, 202])
        self.assertTrue(all(call[2] == BehindLaunch.FLASHW_STOP for call in flashes))
        user32.calls.clear()
        behind.tend(GAME, again=True)
        self.assertEqual(user32.calls, [("lock", BehindLaunch.LSFW_LOCK),
                                        ("flash", 101, BehindLaunch.FLASHW_STOP),
                                        ("pos", 101, BehindLaunch.HWND_BOTTOM, BACK),
                                        ("flash", 202, BehindLaunch.FLASHW_STOP),
                                        ("pos", 202, BehindLaunch.HWND_BOTTOM, BACK)])

    def test_a_window_the_user_brought_up_is_left_alone(self):
        user32 = FakeUser32()
        user32.foreground_pid = GAME
        behind = started(user32, [101])
        behind.tend(GAME)
        self.assertEqual([call for call in user32.calls if call[0] != "lock"], [])


class SettleTest(unittest.TestCase):
    def test_settle_waits_for_godot_to_show_its_window(self):
        user32 = FakeUser32()
        behind = started(user32, [])
        behind.GRACE_SECONDS = 0.2
        behind.hidden.append(9)
        behind.shows = {9: 0.5}
        behind.settle(GAME)
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
        behind = started(user32, [])
        behind.hidden.append(9)  # never shown
        behind.settle(GAME)
        self.assertAlmostEqual(behind.now, BehindLaunch.SETTLE_LIMIT_SECONDS, delta=0.06)
        self.assertFalse(behind.locked or user32.held)

    def test_settle_lets_go_even_when_tending_fails(self):
        class Broken(Behind):
            def windows_of(self, pid):
                raise OSError("gone")

        user32 = FakeUser32()
        broken = Broken(False, user32, [])
        broken.prepare()
        broken.GRACE_SECONDS = 0.0
        with self.assertRaises(OSError):
            broken.settle(GAME)
        self.assertFalse(broken.locked or user32.held)


class LaunchersTest(unittest.TestCase):
    def test_front_and_elsewhere_do_nothing(self):
        user32 = FakeUser32()
        front = Behind(True, user32, [9])
        front.prepare()
        front.tend(GAME)
        front.settle(GAME)
        self.assertEqual(user32.calls, [])
        self.assertEqual(front.popen_kwargs(), {})
        self.assertFalse(BehindLaunch(False, windows=False).active)

    def test_the_launch_takes_front(self):
        self.assertTrue(game_mcp.build_parser().parse_args(["launch", "--front"]).front)
        self.assertFalse(game_mcp.build_parser().parse_args(["launch"]).front)

    def test_a_run_returns_its_command_exit_code(self):
        command = [sys.executable, "-c", "import sys; sys.exit(3)"]
        self.assertEqual(game_mcp.main(["run", "--front", "--"] + command), 3)


if __name__ == "__main__":
    unittest.main()
