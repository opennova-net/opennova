"""Pins scripts/lint/cite_images.py's rule: which image a cited address is in.

Run: python -B -m unittest discover -s scripts/lint -p "test_*.py" (CI's gates job).
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cite_images  # noqa: E402

ANCHOR = re.compile(r"@\s*0x([0-9A-Fa-f]{4,8})\b")


def bound(text: str) -> list[tuple[str, str]]:
    """(address, image) for every `@ 0xADDR` in text; "-" for the default image."""
    out = []
    for m in ANCHOR.finditer(text):
        b = cite_images.image_at(text, m.start(), m.end())
        out.append((m.group(1).lower(), b[0] if b else "-"))
    return out


class ImageAt(unittest.TestCase):
    def test_tail_qualifier_binds_the_marker(self):
        self.assertEqual(
            bound("// [orig: A @0x4fcd84..0x4fcd9b, B @0x431dee (dfx2.exe)]"),
            [("4fcd84", "dfx2.exe"), ("431dee", "dfx2.exe")])
        self.assertEqual(bound("// [orig: X @0x100107ae, misldr.dll]"), [("100107ae", "misldr.dll")])
        self.assertEqual(bound("// [orig: Foo @0x12345 (demo)]"), [("12345", "jodemo.exe")])

    def test_each_clause_keeps_its_own_image(self):
        text = ("// [orig: sub_10007AB0 @0x10007ab0 (the ';' @0x10007b25), misldr.dll; "
                "sub_463050 @0x463050 (@0x4630c9), JOTACmed.exe]")
        self.assertEqual(bound(text), [("10007ab0", "misldr.dll"), ("10007b25", "misldr.dll"),
                                       ("463050", "JOTACmed.exe"), ("4630c9", "JOTACmed.exe")])
        self.assertEqual(bound("// [orig: A @0x1234 (retail); B @0x4567 (dfx2.exe)]"),
                         [("1234", "Jointops.exe"), ("4567", "dfx2.exe")])

    def test_image_before_the_name(self):
        self.assertEqual(bound("// [orig: JOTACmed.exe sub_44E640 @ 0x44e640, from S @ 0x4546b5]"),
                         [("44e640", "JOTACmed.exe"), ("4546b5", "JOTACmed.exe")])
        self.assertEqual(bound("// [orig: gobj sub_1000B4A1 @0x1000b4c8]"), [("1000b4c8", "gobj.dll")])

    def test_a_prose_mention_qualifies_nothing(self):
        self.assertEqual(
            bound("// [orig: S @0x512100 - the producer is the service (NWJoin.dll PUB* cookies)]"),
            [("512100", "-")])
        self.assertEqual(bound("// past TrnGen's draw-back [orig: T @ 0x603635..0x6037A8]."),
                         [("603635", "-")])
        # a marker that qualifies two images binds only its qualified addresses
        self.assertEqual(
            bound("// [orig: U @ 0x54dfb0 (retail) - keys NK@0x7d3f30. (jodemo: A@0x514c40.)]"),
            [("54dfb0", "Jointops.exe"), ("7d3f30", "-"), ("514c40", "jodemo.exe")])
        self.assertEqual(
            bound("// U @0x54dfb0 in retail Jointops.exe (jodemo: A@0x514c40)"),
            [("54dfb0", "-"), ("514c40", "jodemo.exe")])

    def test_separate_markers_on_one_line_are_separate(self):
        self.assertEqual(
            bound("// [orig: M @ 0x454630 (JOTACmed.exe)] then [orig: F @0x401000]"),
            [("454630", "JOTACmed.exe"), ("401000", "-")])

    def test_a_wrapped_marker_closes_at_its_own_bracket(self):
        text = "// [orig: A `test byte ptr [eax+54h], 20h` @0x43296C,\n// B @0x54172D (dfx2.exe)]"
        self.assertEqual(bound(text), [("43296c", "dfx2.exe"), ("54172d", "dfx2.exe")])

    def test_a_qualifier_on_a_wrapped_line_still_binds(self):
        # the census reads the raw file: the comment lead-in of the continuation line is no text
        text = ("// polytrn_scale (128 where it has none [orig:\n"
                "// JOTACmed.exe, the arm @ 0x464a38; the default, sub_464B60 @ 0x464bb6]).")
        self.assertEqual(bound(text), [("464a38", "JOTACmed.exe"), ("464bb6", "JOTACmed.exe")])
        self.assertEqual(
            bound("// [orig: YUV_init @0x30019F00, binkw32.dll -- BinkCopyToBufferRect @0x30013220]"),
            [("30019f00", "binkw32.dll"), ("30013220", "binkw32.dll")])

    def test_spelling(self):
        self.assertTrue(cite_images.misspelled("gobj.dll", "gobj"))
        self.assertTrue(cite_images.misspelled("JOTACmed.exe", "JOTACmed"))
        self.assertFalse(cite_images.misspelled("JOTACmed.exe", "JOTACmed.exe"))
        self.assertFalse(cite_images.misspelled("dfbhd.exe", "dfbhd"))   # an established form
        self.assertFalse(cite_images.misspelled("UPDATE.EXE", ""))        # a record's default


if __name__ == "__main__":
    unittest.main()
