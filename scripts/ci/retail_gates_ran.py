#!/usr/bin/env python3
"""Prove the asset-gated tests RAN once the reference data is mounted.

A gated test that cannot see its data reports Skipped (exit 77) or prints a
`SKIP-LEG:` line and passes, so a green run alone never shows that the retail
legs executed (docs/asset-gated-tests.md). CI mounts the two private reference
repositories and then runs this over ctest's JUnit report (`scripts/build.sh`
writes build/Testing/ctest.xml) and the GUT log: every test a mounted root
gates must have run, and no mixed test may have skipped a leg naming a
mounted root.

    python scripts/ci/retail_gates_ran.py --junit build/Testing/ctest.xml \
        --roots jo_dir,jo_assets
    python scripts/ci/retail_gates_ran.py --gut-log gut.log --roots jo_dir,jo_assets

Exit 1 on any gap, listing it. The expectations below are the root -> test
matrix of docs/asset-gated-tests.md; `--check-docs` (the lint job) proves the
two name the same ctests per root.
"""
from __future__ import annotations

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT_VARS = {
    "jo_dir": "OPENNOVA_JO_DIR",
    "jo_assets": "OPENNOVA_JO_ASSETS",
}

# Fully gated ctests: `***Skipped` with the root mounted is a gap.
MUST_RUN = {
    "jo_dir": [
        "rtxt_jo_install_sweep", "env_jo_install", "bink_retail", "sbf_jo_install_sweep",
        "mission_ai_path_conformance", "truck_dismount", "ai_threat", "ladder_00tra",
        "truck_rest_00tra", "ai_muzzle_pose", "vehicle_ride_00tra", "defense_00trg",
        "lose_flow_04tr", "lose_flow_00tra", "particle_gore_set_catalog", "minefield_retail", "watercraft_02tr",
        # Served by the install (00TRg through revx02) or by the extracted tree.
        "npruntime_authored_payload_00trg",
    ],
    "jo_assets": [
        "root_motion", "anim_positions_from_model_corpus", "anim_reload_clips_us01",
        "anim_weapon_action_clips", "wac_corpus", "cpt_jo_assets_sweep", "lwf_jo_assets_sweep",
        "ai_corpse", "parachute_09tr", "rock_collision_00trg", "soak_00trg", "native_assets_00trg",
        "npruntime_remote_body_state", "npruntime_held_weapon_attach",
        "npruntime_authored_payload_00trg",
        # The shipped .bms missions loose at the tree's root.
        "mission_corpus",
        # The reference fixture set (<assets>/fixtures/**): the fifteen revx02 menus,
        # the shipped MP5 rig map, the BINOC rig and its twist.
        "mnu_compat", "mnu_coverage", "adm_parse", "simassets_adm_skeletal_clips_weapon_channel",
        "def_parse_hudpos",
    ],
}

# Mixed ctests: a `SKIP-LEG:` line naming the mounted root is a gap.
MIXED = {
    "jo_dir": [
        "terrain_tile_composer", "ground_conform", "minimap_overlay", "score_roundtrip",
        "playersav_weapon_sav", "npruntime_weapon_table",
        # The packed install's .mnu sweep.
        "mnu_compat",
    ],
    "jo_assets": [
        "occlusion_armry", "particle_smoke_all_fixtures", "sound_profile",
        "def_parse_items", "infantry", "minimap_overlay",
        # The reference fixture set (<assets>/fixtures/**) legs.
        "dbf_roundtrip", "cbin_roundtrip", "mission_mis_idempotency", "avatars_parse",
        "avatars_roundtrip", "mus_parse", "mus_compat", "mus_decompile", "mus_roundtrip",
        "mus_names_roundtrip", "mus_entry_roundtrip", "mus_encode_idempotence", "mus_vm",
        "mns_document", "bad_parse", "anim_sample",
        # The shipped weapon.def / ammo.def pins.
        "def_parse_weapons", "def_parse_ammo", "npruntime_weapon_table", "npruntime_handshake_server",
    ],
}

# Gated tests the reference data cannot serve yet; reported, never a gap.
# Empty: every gated test runs with the two roots mounted.
KNOWN_ABSENT: dict[str, str] = {}


def check_junit(path: Path, roots: list[str]) -> list[str]:
    gaps: list[str] = []
    tree = ET.parse(path)
    cases: dict[str, ET.Element] = {}
    for case in tree.getroot().iter("testcase"):
        cases[case.get("name", "")] = case
    for root in roots:
        var = ROOT_VARS[root]
        for name in MUST_RUN[root]:
            case = cases.get(name)
            if case is None:
                gaps.append(f"{name}: not in the ctest report (renamed or unregistered?)")
            elif case.find("skipped") is not None or case.get("status") != "run":
                gaps.append(f"{name}: skipped although {var} is mounted "
                            f"(status={case.get('status')})")
        for name in MIXED[root]:
            case = cases.get(name)
            if case is None:
                gaps.append(f"{name}: not in the ctest report (renamed or unregistered?)")
                continue
            out = case.findtext("system-out") or ""
            for line in out.splitlines():
                if line.startswith("SKIP-LEG:") and var in line:
                    gaps.append(f"{name}: {line.strip()}")
    for name, why in KNOWN_ABSENT.items():
        case = cases.get(name)
        if case is not None and case.find("skipped") is not None:
            print(f"[retail-gates] known absent: {name} ({why})")
    return gaps


def check_gut_log(path: Path, roots: list[str]) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    text = re.sub(r"\x1b\[[0-9;]*m", "", text)
    vars_ = [ROOT_VARS[r] for r in roots]
    gaps = []
    # A gated GUT test pends per function ("[Pending]") or skips its whole
    # script from should_skip_script() ("[Script skipped]"); either line names
    # the root it needs (RetailData.fixture_pending_text).
    for line in text.splitlines():
        if ("[Pending]" in line or "[Script skipped]" in line) and any(v in line for v in vars_):
            gaps.append(line.strip())
    return sorted(set(gaps))


REPO = Path(__file__).resolve().parents[2]
DOC = REPO / "docs" / "asset-gated-tests.md"
CMAKE = REPO / "tests" / "CMakeLists.txt"


def registered_ctests() -> set[str]:
    """The ctest names tests/CMakeLists.txt registers literally (foreach-generated
    names are not resolved; a doc token that is not a literal registration is
    simply not compared)."""
    text = CMAKE.read_text(encoding="utf-8", errors="replace")
    names = set(re.findall(r"add_test\(NAME\s+([A-Za-z0-9_]+)", text))
    names |= set(re.findall(r"opennova_add_gated_test\(\s*([A-Za-z0-9_]+)", text))
    return names


def check_docs(junit: Path | None) -> list[str]:
    """The docs matrix and the tables above name the same ctests per root:
    every table name is in its root's row, and every ctest the row names is in
    that root's table (the ctest universe is the JUnit report when given, else
    the literal registrations; a foreach-generated name outside both is not
    compared in that direction)."""
    text = DOC.read_text(encoding="utf-8", errors="replace")
    if junit is not None:
        ctests = {case.get("name", "") for case in ET.parse(junit).getroot().iter("testcase")}
    else:
        ctests = registered_ctests()
    gaps: list[str] = []
    for root, var in ROOT_VARS.items():
        row = next((line for line in text.splitlines() if line.startswith(f"| `{var}` |")), None)
        if row is None:
            gaps.append(f"docs: no matrix row for {var}")
            continue
        doc_tokens = set(re.findall(r"`([A-Za-z0-9_]+)`", row))
        table_names = set(MUST_RUN[root]) | set(MIXED[root])
        for name in sorted(table_names - doc_tokens):
            gaps.append(f"{name}: in the {root} table here, not in the {var} row of docs/asset-gated-tests.md")
        for name in sorted((doc_tokens & ctests) - table_names - set(KNOWN_ABSENT)):
            gaps.append(f"{name}: in the {var} row of docs/asset-gated-tests.md, not in the {root} table here")
    return gaps


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--junit", type=Path, help="ctest --output-junit report")
    ap.add_argument("--gut-log", type=Path, help="the GUT run's captured output")
    ap.add_argument("--roots", default="jo_dir,jo_assets",
                    help="comma-separated mounted roots (jo_dir, jo_assets)")
    ap.add_argument("--check-docs", action="store_true",
                    help="the tables here name the same ctests as the docs matrix rows "
                         "(lint mode; --junit widens the ctest universe to the report)")
    args = ap.parse_args()
    roots = [r.strip() for r in args.roots.split(",") if r.strip()]
    unknown = [r for r in roots if r not in ROOT_VARS]
    if unknown:
        print(f"[retail-gates] unknown roots: {', '.join(unknown)}")
        return 2
    if args.check_docs:
        gaps = check_docs(args.junit)
        if gaps:
            print(f"[retail-gates] FAIL: {len(gaps)} mismatch(es) between the tables and docs/asset-gated-tests.md:")
            for gap in gaps:
                print(f"  {gap}")
            return 1
        print("[retail-gates] OK: the tables and the docs matrix name the same ctests")
        return 0
    if args.junit is None and args.gut_log is None:
        print("[retail-gates] nothing to check: pass --junit and/or --gut-log")
        return 2
    gaps: list[str] = []
    if args.junit is not None:
        gaps += check_junit(args.junit, roots)
    if args.gut_log is not None:
        gaps += check_gut_log(args.gut_log, roots)
    if gaps:
        print(f"[retail-gates] FAIL: {len(gaps)} gated test(s) did not run with the data mounted:")
        for gap in gaps:
            print(f"  {gap}")
        return 1
    checked = sum(len(MUST_RUN[r]) + len(MIXED[r]) for r in roots)
    print(f"[retail-gates] OK: {checked} gated expectation(s) over {', '.join(roots)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
