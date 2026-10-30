"""Pins scripts/ida/cite_sweep.py's classifier: how a marker's (name, address) pair reads
against one IDB join row. The join itself needs a live IDA; the classifier does not.

Run: python -B -m unittest discover -s scripts/ida -p "test_*.py" (CI's gates job).
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cite_sweep  # noqa: E402


def row(nm="", fs=None, fn="", defined=True, tea=None, ref=None, member=False):
    """One IDB join row: [name at the address, its function's start and name, defined,
    the cited name's own address, the nearest referencing site, a struct member]."""
    return [nm, fs, fn, defined, tea, ref, member]


def classify(tok, addr, info):
    return cite_sweep.classify(tok, addr, "", info)


class SnakeCaseAtAnEntry(unittest.TestCase):
    def test_a_stale_snake_name_at_a_function_entry_is_a_mismatch(self):
        info = row(nm="KeyBinding_MergeWeaponSlotParams", fs="0x54c3f0",
                   fn="KeyBinding_MergeWeaponSlotParams")
        self.assertEqual(classify("merge_weapon_slot_params", "0x54c3f0", info), "name-mismatch")

    def test_a_snake_name_the_idb_carries_elsewhere(self):
        info = row(nm="Render_TerrainDecal", fs="0x5b0000", fn="Render_TerrainDecal", tea="0x5c0000")
        self.assertEqual(classify("render_terrain_decal", "0x5b0000", info), "name-elsewhere")

    def test_a_snake_name_over_an_unnamed_entry_is_an_idb_autoname(self):
        info = row(nm="sub_71C550", fs="0x71c550", fn="sub_71C550")
        self.assertEqual(classify("emit_message", "0x71c550", info), "idb-autoname")

    def test_a_snake_word_inside_a_function_stays_a_site_cite(self):
        # `the slot's scale_y @0x580a90`: a field named at a site, not the function's name
        info = row(fs="0x580a80", fn="GameFont_MeasureCharHeight")
        self.assertEqual(classify("scale_y", "0x580a90", info), "site-only")

    def test_a_snake_struct_member_at_an_entry_stays_a_site_cite(self):
        info = row(nm="Foo_Bar", fs="0x500000", fn="Foo_Bar", member=True)
        self.assertEqual(classify("scale_y", "0x500000", info), "site-only")

    def test_a_demangled_names_bare_leaf_stays_a_site_cite(self):
        nm = "?jpeg_read_header@D3DX@@YGHPAUjpeg_decompress_struct@1@H@Z"
        info = row(nm=nm, fs="0x71aca0", fn=nm)
        self.assertEqual(classify("jpeg_read_header", "0x71aca0", info), "site-only")

    def test_a_snake_short_form_of_the_entry(self):
        info = row(nm="Config_parse_line", fs="0x600000", fn="Config_parse_line")
        self.assertEqual(classify("parse_line", "0x600000", info), "short-form")


class AutoNames(unittest.TestCase):
    def test_an_auto_name_the_idb_still_carries_is_owed_a_name(self):
        # the IDB side omits a key whose IDB name is the cited one; main() joins it as entry_info
        info = cite_sweep.entry_info("sub_57FCE0", "0x57fce0")
        self.assertEqual(classify("sub_57FCE0", "0x57fce0", info), "idb-autoname")

    def test_an_auto_name_cited_inside_its_unnamed_function(self):
        info = row(fs="0x57fce0", fn="sub_57FCE0", tea="0x57fce0")
        self.assertEqual(classify("sub_57FCE0", "0x57fd10", info), "idb-autoname")

    def test_an_auto_name_the_idb_has_since_named(self):
        info = row(nm="Entity_Destroy", fs="0x4b0000", fn="Entity_Destroy")
        self.assertEqual(classify("sub_4B0000", "0x4b0000", info), "code-autoname")

    def test_a_data_auto_name_the_idb_still_uses(self):
        self.assertEqual(classify("flt_7C3B94", "0x7c3b94", cite_sweep.entry_info("flt_7C3B94", "0x7c3b94")), "ok")
        info = row(fs="0x4a0000", fn="Foo_Bar", tea="0x7c3b94", ref="0x4a0010")
        self.assertEqual(classify("flt_7C3B94", "0x4a0010", info), "call-site")

    def test_a_named_functions_thunk_carries_its_name(self):
        self.assertEqual(classify("j_Entity_Destroy", "0x401234",
                                  cite_sweep.entry_info("j_Entity_Destroy", "0x401234")), "ok")
        self.assertEqual(classify("j_sub_4B0000", "0x401234",
                                  cite_sweep.entry_info("j_sub_4B0000", "0x401234")), "idb-autoname")


class ImportCalls(unittest.TestCase):
    """`call ds:__imp_GetTickCount` calls GetTickCount: the IDB names the thunk `GetTickCount`
    and the table entry `__imp_GetTickCount`, so the call-site test reads the entry's name."""

    def test_an_import_entry_answers_to_its_symbol(self):
        self.assertIn("GetTickCount", cite_sweep.import_forms("__imp_GetTickCount"))
        self.assertIn("D3DXCreateTexture", cite_sweep.import_forms("__imp__D3DXCreateTexture@36"))

    def test_a_plain_name_is_no_import_entry(self):
        self.assertEqual(cite_sweep.import_forms("GetTickCount"), set())
        self.assertEqual(cite_sweep.import_forms(""), set())

    def test_the_import_call_reads_as_a_call_site(self):
        # the join row refs_symbol returns for `GetTickCount @0x4c408a`: the cited name lives at the
        # thunk 0x61d0d0, the site's own instruction calls through the table entry
        info = row(fs="0x4c4060", fn="CNetPlayer_SetGameState", tea="0x61d0d0", ref="0x4c408a")
        self.assertEqual(classify("GetTickCount", "0x4c408a", info), "call-site")
        info = row(fs="0x4c4060", fn="CNetPlayer_SetGameState", tea="0x61d0d0")
        self.assertEqual(classify("GetTickCount", "0x4c408a", info), "name-elsewhere")


class GodotLegs(unittest.TestCase):
    """A reverse link to a godot/ file stands while that file names the symbols the line names
    for it: the Godot side carries no marker (godot_orig_cites only shrinks)."""

    FILES = {
        "godot/src/network/lan_session.cpp": "void LanSession::stop_enumerator() {}\nvoid LanSession::_process(double) {}",
        "godot/game/mp_menu_companion.gd": "func _on_lan_join() -> void:\n\tpass",
        "engine/net/npwire/lan_discovery.h": "class LanDiscoveryBrowser { void stop(); };",
        # a caller: it names LanSession and calls stop_enumerator, it defines neither
        "godot/src/world/game_world.cpp": "void GameWorld::leave_lan() {\n\tLanSession *s = lan_session();\n"
                                          "\ts->stop_enumerator();\n}",
    }

    def read(self, path):
        return self.FILES.get(path, "")

    def test_a_leg_still_in_its_file_stands(self):
        line = "godot::LanSession::_process (the window's end; L2-s17) @ godot/src/network/lan_session.cpp"
        self.assertTrue(cite_sweep.godot_leg_named(line, "godot/src/network/lan_session.cpp", self.read))
        line = "MpMenuCompanion._on_lan_join (the row index the join walks to) @ godot/game/mp_menu_companion.gd"
        self.assertTrue(cite_sweep.godot_leg_named(line, "godot/game/mp_menu_companion.gd", self.read))

    def test_every_symbol_must_be_named(self):
        line = ("godot::LanSession::_process + godot::LanSession::destroy_pre_dial_session (state 9) "
                "@ godot/src/network/lan_session.cpp")
        self.assertFalse(cite_sweep.godot_leg_named(line, "godot/src/network/lan_session.cpp", self.read))

    def test_ida_names_and_file_names_are_prose(self):
        line = ("godot::LanSession::stop_enumerator (CNapiNetwork_DrainPendingDataTransfers, the keylist.key and "
                "CC.BIN reads) @ godot/src/network/lan_session.cpp")
        self.assertEqual(cite_sweep.leg_symbols(line, "godot/src/network/lan_session.cpp")[1],
                         ["godot::LanSession::stop_enumerator"])

    def test_only_godot_paths_and_named_symbols(self):
        line = "opennova::LanDiscoveryBrowser::stop @ engine/net/npwire/lan_discovery.h"
        self.assertFalse(cite_sweep.godot_leg_named(line, "engine/net/npwire/lan_discovery.h", self.read))
        line = "the click godot/game/mp_menu_companion.gd _on_lan_join"
        self.assertFalse(cite_sweep.godot_leg_named(line, "godot/game/mp_menu_companion.gd", self.read))

    def test_a_list_shares_its_symbols(self):
        line = ("opennova::LanDiscoveryBrowser::stop + godot::LanSession::stop_enumerator "
                "@ engine/net/npwire/lan_discovery.h, godot/src/network/lan_session.cpp")
        self.assertTrue(cite_sweep.godot_leg_named(line, "godot/src/network/lan_session.cpp", self.read))

    def test_a_mention_passes_for_a_leg_that_moved_the_known_gap(self):
        # The rule tests a mention, not a definition: a link left on a file that only calls the
        # symbol (the leg moved to lan_session.cpp, the old file still calls it) passes. A moved
        # Godot leg is found by reading; this pins the gap so a stricter rule shows as a change here.
        line = "godot::LanSession::stop_enumerator (the window's end) @ godot/src/world/game_world.cpp"
        self.assertTrue(cite_sweep.godot_leg_named(line, "godot/src/world/game_world.cpp", self.read))
        # A file that no longer names the symbol at all still fails.
        line = "godot::LanSession::stop_enumerator (the window's end) @ godot/src/world/world_scene.cpp"
        self.assertFalse(cite_sweep.godot_leg_named(line, "godot/src/world/world_scene.cpp", self.read))


class MarkerSubjects(unittest.TestCase):
    """A marker's first address is the function it is about; the rest are the sites and callees
    its prose names."""

    def test_the_first_address_of_each_marker(self):
        lines = ["// [orig: Foo_Bar @0x500000 - the call Baz_Qux @0x500010]; [orig: Qux_Quux @0x600000]"]
        got = [(tok, subject) for _, _, tok, _, _, subject in cite_sweep.extract_lines("engine/x.cpp", lines)]
        self.assertEqual(got, [("Foo_Bar", True), ("Baz_Qux", False), ("Qux_Quux", True)])

    def test_a_record_cites_outside_markers(self):
        # a docs table cell and a backticked prose cite are joined; a bare prose @0x is not
        line = "| D-SND-8 | `Sound_ApplyDopplerShift` @ 0x527f60 doppler, the call @ 0x52896a |"
        got = [(tok, addr) for _, _, tok, addr, _, _ in cite_sweep.extract_lines("docs/audio/x-re.md", [line])]
        self.assertEqual(got, [("Sound_ApplyDopplerShift", "0x527f60")])
        # code joins markers only
        self.assertEqual(list(cite_sweep.extract_lines("engine/x.cpp", ["// Foo_Bar @0x500000"])), [])

    def test_a_wrapped_markers_lines_are_read_once(self):
        # the continuation lines of a wrapped marker are its own, never an unmarked record line
        lines = ["the item's z (`[orig: JOTACmed.exe sub_44E640 @", "0x44e640, from MIS_SaveDocument @ 0x4546b5; sub_44E6A0 @",
                 "0x44e6a0]`); then `Foo_Bar @0x401000`"]
        got = [(tok, n, ctx) for _, n, tok, _, ctx, _ in cite_sweep.extract_lines("docs/x-re.md", lines)]
        self.assertEqual([g[0] for g in got], ["sub_44E640", "MIS_SaveDocument", "sub_44E6A0", "Foo_Bar"])
        self.assertTrue(all(ctx == "(other-image) JOTACmed.exe" for tok, n, ctx in got if tok != "Foo_Bar"))

    def test_a_wrapped_qualifier_binds_its_cite(self):
        lines = ["exchanged as 16.16 (`sub_401C23 @0x401C23", "(Nile.exe)`), each a 16.16 value.",
                 "The loader `Foo_Bar @0x401000` reads them."]
        got = [(tok, ctx) for _, n, tok, _, ctx, _ in cite_sweep.extract_lines("docs/world/x-re.md", lines)]
        self.assertEqual(got, [("sub_401C23", "(other-image) Nile.exe"), ("Foo_Bar", "")])

    def test_a_record_section_names_its_default_image(self):
        lines = ["## 1. Launch", "`UI_LaunchUpdateProcess @ 0x55b0b0`", "## 2. Inputs", "`Cupdate2Dlg_ReadWizard @ 0x405000`"]
        got = [ctx for *_, ctx, _ in cite_sweep.extract_lines("docs/update/updater-re.md", lines)]
        self.assertEqual(got, ["", "(other-image) UPDATE.EXE"])

    def test_a_bare_site_marker_is_its_own_subject(self):
        got = [subject for *_, subject in cite_sweep.extract_lines("engine/x.cpp", ["// [orig: @0x500010]"])]
        self.assertEqual(got, [True])


class ReverseLinks(unittest.TestCase):
    """classify_reimpl: an IDB `reimpl:` link joined against the tree, with the planned rewrite."""

    START, END = "0x7671b0", "0x767200"   # AudioChannel_InitAll's range in these rows

    def classify(self, link, pairs, tracked, held=()):
        rows = [[self.START, self.START, self.END, "AudioChannel_InitAll", 0, "reimpl: " + link]]
        n, out, edits = cite_sweep.classify_reimpl(rows, pairs, tracked,
                                                   history=lambda paths: set(paths) & set(held))
        return [(r[0], r[4]) for r in out], edits

    @staticmethod
    def cite(path, addr, subject):
        return (path, 1, "AudioChannel_InitAll", addr, "", subject)

    def test_a_path_no_landed_commit_held_is_unlanded(self):
        rows, _ = self.classify("Mixer::init_all @ engine/runtime/audio/mixer.cpp", [], [])
        self.assertEqual(rows, [("reimpl-unlanded", "")])

    def test_a_path_the_history_held_is_stale(self):
        held = ["engine/runtime/audio/mixer.cpp"]
        rows, _ = self.classify("Mixer::init_all @ engine/runtime/audio/mixer.cpp", [], [], held)
        self.assertEqual(rows, [("reimpl-stale", "")])

    def test_an_include_root_path_is_its_engine_path(self):
        tracked = ["engine/base/platform/input_sample.h"]
        pairs = [self.cite("engine/base/platform/input_sample.h", "0x7671c0", True)]
        rows, _ = self.classify("InputState @ base/platform/input_sample.h", pairs, tracked)
        self.assertEqual(rows, [])
        # gone under both spellings, and held under its engine path: stale, not unlanded
        rows, _ = self.classify("InputState @ base/platform/input_sample.h", [], [],
                                ["engine/base/platform/input_sample.h"])
        self.assertEqual(rows, [("reimpl-stale", "")])

    def test_a_mention_in_another_functions_marker_is_no_home(self):
        # world.h names AudioChannel_InitAll @0x7671B0 inside Input_HandleActionBinding's marker
        tracked = ["engine/runtime/world/world.h"]
        pairs = [("engine/runtime/world/world.h", 1, "Input_HandleActionBinding", "0x49ad40", "", True),
                 self.cite("engine/runtime/world/world.h", "0x7671b0", False)]
        rows, edits = self.classify("Mixer::init_all @ engine/runtime/audio/mixer.cpp", pairs, tracked)
        self.assertEqual(rows, [("reimpl-unlanded", "")])
        self.assertEqual(edits, [])

    def test_the_file_whose_marker_is_about_the_function_is_the_home(self):
        tracked = ["engine/runtime/world/world.h", "engine/runtime/audio/channels.cpp"]
        pairs = [self.cite("engine/runtime/world/world.h", "0x7671b0", False),
                 self.cite("engine/runtime/audio/channels.cpp", "0x7671b0", True)]
        rows, edits = self.classify("Mixer::init_all @ engine/runtime/audio/mixer.cpp", pairs, tracked)
        self.assertEqual(rows, [("reimpl-unlanded", "engine/runtime/audio/channels.cpp")])
        self.assertEqual(edits, [(self.START, 0, "reimpl: Mixer::init_all @ engine/runtime/audio/channels.cpp")])

    def test_the_editor_is_no_home_for_a_port(self):
        # the blank menu factory's marker says why its screens keep a close command: no port
        tracked = ["engine/editor/blank/blank_mission_menus.cpp"]
        pairs = [self.cite("engine/editor/blank/blank_mission_menus.cpp", "0x7671b0", True)]
        rows, edits = self.classify("x @ engine/runtime/menu/death_screen.cpp", pairs, tracked)
        self.assertEqual((rows, edits), ([("reimpl-unlanded", "")], []))

    def test_a_moved_link_follows_only_a_lone_engine_home(self):
        tracked = ["godot/src/audio/old_home.cpp", "engine/runtime/audio/channels.cpp"]
        pairs = [self.cite("engine/runtime/audio/channels.cpp", "0x7671d0", True)]
        rows, _ = self.classify("x @ godot/src/audio/old_home.cpp", pairs, tracked)
        self.assertEqual(rows, [("reimpl-moved", "engine/runtime/audio/channels.cpp")])
        pairs = [self.cite("engine/runtime/audio/channels.cpp", "0x7671d0", False)]
        rows, _ = self.classify("x @ godot/src/audio/old_home.cpp", pairs, tracked)
        self.assertEqual(rows, [("reimpl-moved", "")])


class Unchanged(unittest.TestCase):
    def test_the_accepted_forms(self):
        self.assertEqual(classify("Foo_Bar", "0x500000", cite_sweep.entry_info("Foo_Bar", "0x500000")), "ok")
        self.assertEqual(classify(None, "0x500010", cite_sweep.entry_info(None, "0x500010")), "site-only")
        self.assertEqual(classify("Foo_Bar", "0x500010", row(fs="0x500000", fn="Foo_Bar", tea="0x500000")),
                         "inner-site")
        self.assertEqual(classify("MergeSlot", "0x500000", row(nm="Foo_MergeSlot", fs="0x500000", fn="Foo_MergeSlot")),
                         "short-form")
        self.assertEqual(classify("the", "0x500010", row(fs="0x500000", fn="Foo_Bar")), "site-only")

    def test_the_defects(self):
        self.assertEqual(classify("Foo_Old", "0x500000", row(nm="Foo_Bar", fs="0x500000", fn="Foo_Bar")),
                         "name-mismatch")
        self.assertEqual(classify("Foo_Bar", "0x500000", row(nm="", fs=None, fn="", defined=False)), "undefined")
        self.assertEqual(classify("Foo_Bar", "0x1000", row()), "malformed")
        self.assertEqual(classify("Foo_Bar", "0x10001000", row()), "unqualified-image")


if __name__ == "__main__":
    unittest.main()
