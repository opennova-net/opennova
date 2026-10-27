"""The perf cell's offline parts (perf_cell.py, docs/mcp.md "The perf cell").

    python -B -m unittest discover -s scripts/mcp -p "test_*.py"

game.cfg's lock word is set and every other byte kept. The table prints the skeleton's legs
that a record carries, the World tick's other legs heaviest first, and the per-frame counts.
xperf's stack report yields its inclusive table's rows.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import perf_cell  # noqa: E402


def leg(mean, p50, p95):
    return {"mean_ms": mean, "p50_ms": p50, "p95_ms": p95, "max_ms": p95, "p99_ms": p95, "samples": 10}


class LockWordTest(unittest.TestCase):
    CFG = b"// game.cfg\r\nfov = 90\r\nlock_framerate       = 1\r\nvsync = 0\r\n"

    def test_off_and_on_change_the_word_alone(self):
        off = perf_cell.with_lock(self.CFG, False)
        self.assertEqual(off, self.CFG.replace(b"= 1\r\nvsync", b"= 0\r\nvsync"))
        self.assertEqual(perf_cell.with_lock(off, True), self.CFG)

    def test_a_cfg_with_no_lock_word_is_refused(self):
        with self.assertRaises(perf_cell.CellError):
            perf_cell.with_lock(b"fov = 90\n", True)


class SummaryTableTest(unittest.TestCase):
    def cells(self):
        off = {"frame": leg(15.8, 15.5, 19.4), "frame_world": leg(10.2, 10.1, 12.7),
               "world_runtime": leg(4.9, 4.7, 7.2), "sim_advance": leg(3.2, 3.1, 4.7),
               "sim_step": leg(2.7, 2.6, 3.8), "world_water": leg(0.7, 0.7, 1.0),
               "world_slot_shadow": leg(0.6, 0.6, 0.9), "world_iris": leg(0.01, 0.01, 0.02),
               "draw": leg(2.4, 2.3, 3.0), "sim_ticks": {"mean": 1.03, "max": 2, "samples": 600}}
        on = dict(off, frame=leg(21.7, 19.5, 31.6), sim_frame_bank=leg(6.9, 5.0, 15.0),
                  sim_frame_lock_sleeps={"mean": 6.5, "max": 15, "samples": 400})
        return [{"lock": "off", "metrics": off, "frames": 634}, {"lock": "on", "metrics": on, "frames": 460}]

    def test_the_table_prints_the_legs_a_record_carries_in_the_skeleton_order(self):
        lines = perf_cell.summary_table(self.cells()).splitlines()
        names = [line.split()[0] for line in lines[1:]]
        self.assertEqual(names[:9], ["frame", "frame_world", "world_runtime", "sim_advance", "sim_frame_bank",
                                     "sim_step", "world_water", "world_slot_shadow", "draw"])
        self.assertNotIn("world_iris", names, "a world leg under the floor prints no row")
        self.assertNotIn("process_callbacks", names, "a leg no record carries prints no row")
        frame = lines[1]
        self.assertIn("15.80   15.50   19.40", frame)
        self.assertIn("21.70   19.50   31.60", frame)
        bank = next(line for line in lines if line.strip().startswith("sim_frame_bank"))
        self.assertIn("-", bank.split("|")[0], "the lock-off record has no bank: a dash")
        self.assertTrue(lines[2].startswith("    frame_world"), "the depth indents the leg")

    def test_the_counts_follow_the_legs(self):
        text = perf_cell.summary_table(self.cells())
        ticks = next(line for line in text.splitlines() if line.startswith("ticks per frame"))
        self.assertIn("1.03", ticks)
        sleeps = next(line for line in text.splitlines() if line.startswith("lock sleeps per frame"))
        self.assertIn("6.50", sleeps)
        self.assertIn("-", sleeps.split("|")[0])
        self.assertTrue(text.splitlines()[-1].startswith("frames sampled"))


class ButterflyTest(unittest.TestCase):
    HTML = ("<a id='TblMI'><h2>Modules</h2></a><table><tbody><tr><td>x</td><td>1</td><td>1.00%</td>"
            "<td>1</td></tr></tbody></table>"
            "<a id='TblSI'><h2>Functions by UniInclusive Hits</h2></a><table rules='all'><thead><tr>"
            "<th>function name</th></tr></thead><tbody>"
            "<tr><td><a href='#M1'>oneditord.dll</a>!<a href='#S1'>godot::GameWorld::tick</a></td><td>900</td>"
            "<td>90.00%</td><td>12</td><td>0x0</td><td>0x1</td><td>0x1</td></tr>"
            "<tr><td><a href='#M2'>ntdll.dll</a>!<a href='#S2'>RtlUserThreadStart</a></td><td>800</td>"
            "<td>80.00%</td><td>0</td><td>0x0</td><td>0x1</td><td>0x1</td></tr>"
            "</tbody></table><a id='TblSN'><table><tbody><tr><td>y</td><td>5</td><td>5.00%</td><td>5</td></tr>")

    def test_the_inclusive_table_alone_is_read(self):
        rows = perf_cell.parse_butterfly(self.HTML)
        self.assertEqual(rows, [("oneditord.dll!godot::GameWorld::tick", 900, 90.0, 12),
                                ("ntdll.dll!RtlUserThreadStart", 800, 80.0, 0)])

    def test_a_report_with_no_inclusive_table_reads_empty(self):
        self.assertEqual(perf_cell.parse_butterfly("<html></html>"), [])


if __name__ == "__main__":
    unittest.main()
