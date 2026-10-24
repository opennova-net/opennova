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
