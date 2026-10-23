"""The scenario aim_at step's look arithmetic (scenario_aim.py).

    python -B -m unittest discover -s scripts/net -p "test_*.py"

The oracles are retail's own arithmetic, cited in scenario_aim.py: one tick's look of
dx px turns the +0x10 heading by (dx * (S << 11) + 0x8000) >> 16 units of 65536 BAM
(Input_ProcessMouseAxisBindings @0x49972E; case 166 @0x4E10A4..0x4E10A7), so the
rounding is half up, not symmetric; the bearing is trunc(atan2(dy, dx) * dbl_7C19D8)
(AI_FindBestTargetB @0x4671A9..0x4671C5); S is the current profile record's +0x590
word (Game_ApplySessionSettingsToGlobals @0x55161E).
"""
import contextlib
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scenario_aim  # noqa: E402


class LookTurnTest(unittest.TestCase):
    def test_default_word_turns_four_units_a_pixel(self):
        # S = 128: sens 0x40000, one px is 4.5 units before the floor.
        self.assertEqual(scenario_aim.look_turn_units(1, 128), 4)
        self.assertEqual(scenario_aim.look_turn_units(-1, 128), -4)
        self.assertEqual(scenario_aim.look_turn_units(1000, 128), 4000)
        self.assertEqual(scenario_aim.heading_after(0, 1, 128), -(4 << 16))

    def test_rounding_is_half_up(self):
        # S = 16: one px is exactly half a unit; +0.5 rounds to 1, -0.5 to 0.
        self.assertEqual(scenario_aim.look_turn_units(1, 16), 1)
        self.assertEqual(scenario_aim.look_turn_units(-1, 16), 0)
        self.assertEqual(scenario_aim.look_turn_units(3, 16), 2)
        self.assertEqual(scenario_aim.look_turn_units(-3, 16), -1)

    def test_heading_wraps_like_the_register(self):
        start = 0x7FFF0000
        self.assertEqual(scenario_aim.heading_after(start, -1, 128),
                         scenario_aim.wrap32(start + (4 << 16)))
        self.assertLess(scenario_aim.heading_after(start, -1, 128), 0)


class BearingTest(unittest.TestCase):
    def test_heading_convention(self):
        # +x is heading 0, +y a quarter turn, -x the half turn fistp stores as INT32_MIN.
        self.assertEqual(scenario_aim.bearing_bam(1.0, 0.0), 0)
        self.assertEqual(scenario_aim.bearing_bam(0.0, 1.0), 0x40000000)
        self.assertEqual(scenario_aim.bearing_bam(0.0, -1.0), -0x40000000)
        self.assertEqual(scenario_aim.bearing_bam(-1.0, 0.0), -0x80000000)

    def test_the_scale_is_dbl_7c19d8(self):
        self.assertEqual(struct.pack("<d", scenario_aim.BAM_PER_RADIAN).hex(),
                         "83c8c96d305fc441")

    def test_scale_invariant_over_q16_words(self):
        # The retail read hands Q16 Position words, ours world units: one bearing.
        self.assertEqual(scenario_aim.bearing_bam(2.078, 4.123),
                         scenario_aim.bearing_bam(2.078 * 65536, 4.123 * 65536))


class AimLookTest(unittest.TestCase):
    def test_quarter_turn_left(self):
        # Facing +x, a target on +y: the look moves the mouse left (negative px).
        aim = scenario_aim.aim_look(0, (0.0, 0.0), (0.0, 10.0), 128)
        self.assertEqual(aim["look_px"], -4096)
        self.assertEqual(aim["look_steps"], [-4096])
        self.assertEqual(aim["turn_units"], -16384)
        self.assertEqual(aim["heading_after_bam"], 0x40000000)
        self.assertEqual(aim["residual_bam"], 0)

    def test_the_look_lands_nearest(self):
        # Every px either side of the pick leaves the heading at least as far off.
        for heading, target, word in ((0x12345678, (-3.0, 7.5), 128),
                                      (-0x6000_0000, (5.0, -0.25), 200),
                                      (0x0004_0000, (1.0, 0.0001), 37)):
            aim = scenario_aim.aim_look(heading, (0.0, 0.0), target, word)
            for other in (aim["look_px"] - 1, aim["look_px"] + 1):
                after = scenario_aim.heading_after(heading, other, word)
                self.assertGreaterEqual(
                    abs(scenario_aim.wrap32(after - aim["bearing_bam"])),
                    abs(aim["residual_bam"]))
            self.assertLessEqual(abs(aim["residual_bam"]), (word * 2048 // 2) + 0x8000)

    def test_the_short_way_round(self):
        # Facing just past the half turn, a target just short of it: a small left turn.
        aim = scenario_aim.aim_look(-0x7FF00000, (0.0, 0.0), (-1.0, -0.01), 128)
        self.assertLess(abs(aim["look_px"]), 600)

    def test_a_large_look_splits_into_same_tick_steps(self):
        # S = 1: a half turn needs 32768 * 32 px, more than one onHook step carries.
        aim = scenario_aim.aim_look(0, (0.0, 0.0), (-1.0, 0.000001), 1)
        self.assertEqual(sum(aim["look_steps"]), aim["look_px"])
        self.assertTrue(all(abs(step) <= 32767 for step in aim["look_steps"]))
        self.assertGreater(len(aim["look_steps"]), 1)

    def test_refusals(self):
        with self.assertRaises(ValueError):
            scenario_aim.aim_look(0, (0.0, 0.0), (1.0, 1.0), 0)
        with self.assertRaises(ValueError):
            scenario_aim.aim_look(0, (2.0, 3.0), (2.0, 3.0), 128)


def player_sav(slot: int, word: int) -> bytes:
    """A five-record player.sav (16-byte header, 0x3C80-byte records, 8-byte trailer)
    whose record `slot` holds `word` at +0x590."""
    data = bytearray(16 + 5 * 0x3C80 + 8)
    data[0:8] = b"FPBC0211"
    struct.pack_into("<i", data, 16 + slot * 0x3C80 + 0x590, word)
    return bytes(data)


class LookTurnedTest(unittest.TestCase):
    # x9's revive pre-flight (push 203, RR, the retail host's aims; LP-s12 item a):
    # aim 1 read 0x40000000 and looked onto 1352925184; aim 2 read 1352935218 (turned).
    # The medic then died; aim 3 read 1352925183 after aim 2's look onto 973350706.
    def test_a_look_that_reached_the_body(self):
        out = scenario_aim.look_turned(1073741824, 1352935218, 1352925184)
        self.assertTrue(out["ok"])
        self.assertEqual(out["turned_bam"], 279193394)
        self.assertEqual(out["expected_bam"], 279183360)

    def test_a_dead_body_drifts_but_does_not_turn(self):
        out = scenario_aim.look_turned(1352935218, 1352925183, 973350706)
        self.assertFalse(out["ok"])
        self.assertEqual(out["turned_bam"], -10035)
        self.assertEqual(out["expected_bam"], -379584512)

    def test_half_the_turn_in_its_direction(self):
        self.assertTrue(scenario_aim.look_turned(0, 50, 100)["ok"])
        self.assertFalse(scenario_aim.look_turned(0, 49, 100)["ok"])
        self.assertFalse(scenario_aim.look_turned(0, -100, 100)["ok"])
        self.assertTrue(scenario_aim.look_turned(5, 5, 5)["ok"])  # a look of no turn

    def test_the_turn_wraps_like_the_register(self):
        out = scenario_aim.look_turned(0x7FFF0000, -0x7FFF0000, -0x7FFE0000)
        self.assertTrue(out["ok"])
        self.assertEqual(out["turned_bam"], 0x20000)


class ProfileWordTest(unittest.TestCase):
    def test_the_current_record_word(self):
        self.assertEqual(scenario_aim.profile_mouse_sensitivity(player_sav(2, 200), 2), 200)
        self.assertEqual(scenario_aim.profile_mouse_sensitivity(player_sav(0, 128), 0), 128)

    def test_slot_from_game_cfg(self):
        self.assertEqual(scenario_aim.profile_slot('player_index = 3\r\nname = "x"\r\n'), 3)
        self.assertEqual(scenario_aim.profile_slot('  player_index="2"\n'), 2)
        self.assertEqual(scenario_aim.profile_slot("player_index = 9\n"), 0)
        with self.assertRaises(ValueError):
            scenario_aim.profile_slot("player_index = 1\nplayer_index = 2\n")
        with self.assertRaises(ValueError):
            scenario_aim.profile_slot("name = x\n")

    def test_slot_five_names_no_record(self):
        with self.assertRaises(ValueError):
            scenario_aim.profile_mouse_sensitivity(player_sav(0, 128), 5)


class PoseWordsTest(unittest.TestCase):
    def test_the_drivers_words(self):
        # A yaw-0 pose reads heading 0x40000000 (x7's revive pre-flight); medic_revive's
        # pose (29.93, -188.0) truncates to its 16.16 words.
        self.assertEqual(scenario_aim.pose_words(29.93, -188.0, 0.0),
                         {"x_q16": 1961492, "y_q16": -12320768, "heading_bam": 0x40000000})
        self.assertEqual(scenario_aim.pose_words(0.0, 0.0, 90.0)["heading_bam"], 0)
        self.assertEqual(scenario_aim.pose_words(0.0, 0.0, 180.0)["heading_bam"], -0x40000000)
        self.assertEqual(scenario_aim.pose_words(0.0, 0.0, 270.0)["heading_bam"], -0x80000000)
        self.assertEqual(scenario_aim.pose_words(0.0, 0.0, 288.0)["heading_bam"],
                         scenario_aim.wrap32(int(162.0 * 4294967296.0 / 360.0)))
        self.assertEqual(scenario_aim.pose_words(-0.5, 1.99999, -90.0),
                         {"x_q16": -32768, "y_q16": 131071, "heading_bam": -0x80000000})

    def test_refuses_a_non_finite_pose(self):
        with self.assertRaises(ValueError):
            scenario_aim.pose_words(float("nan"), 0.0, 0.0)


class CommandLineTest(unittest.TestCase):
    def run_main(self, argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(scenario_aim.main(argv), 0)
        return json.loads(out.getvalue())

    def test_look(self):
        out = self.run_main(["look", "--heading-bam=0", "--actor=-1.5,-2", "--target=-1.5,8",
                             "--sensitivity=128"])
        self.assertEqual(out["look_px"], -4096)

    def test_pose(self):
        self.assertEqual(self.run_main(["pose", "--x=29.93", "--y=-188.0", "--yaw-deg=0"]),
                         {"x_q16": 1961492, "y_q16": -12320768, "heading_bam": 1073741824})

    def test_turned(self):
        out = self.run_main(["turned", "--before-bam=1352935218", "--after-bam=1352925183",
                             "--expected-bam=973350706"])
        self.assertEqual(out, {"turned_bam": -10035, "expected_bam": -379584512, "ok": False})

    def test_profile_word(self):
        with tempfile.TemporaryDirectory() as root:
            copy = Path(root)
            (copy / "game.cfg").write_bytes(b"player_index = 1\r\n")
            (copy / "player.sav").write_bytes(player_sav(1, 96))
            self.assertEqual(self.run_main(["profile-word", f"--copy={copy}"]),
                             {"slot": 1, "mouse_sensitivity": 96})


if __name__ == "__main__":
    unittest.main()
