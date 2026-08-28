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
        --roots jo_dir,jo_assets,mission_corpus
    python scripts/ci/retail_gates_ran.py --gut-log gut.log --roots jo_dir,jo_assets,mission_corpus

Exit 1 on any gap, listing it. The expectations below are the root -> test
matrix of docs/asset-gated-tests.md; keep the two in step.
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
    "mission_corpus": "OPENNOVA_MISSION_CORPUS",
}

# Fully gated ctests: `***Skipped` with the root mounted is a gap.
MUST_RUN = {
    "jo_dir": [
        "rtxt_jo_install_sweep", "env_jo_install", "bink_retail", "sbf_jo_install_sweep",
        "mission_ai_path_conformance", "truck_dismount", "ai_threat", "ladder_00tra",
        "truck_rest_00tra", "ai_muzzle_pose", "vehicle_ride_00tra", "defense_00trg",
        "lose_flow_04tr", "particle_gore_set_catalog",
    ],
    "jo_assets": [
        "root_motion", "anim_positions_from_model_corpus", "anim_reload_clips_us01",
        "anim_weapon_action_clips", "wac_corpus", "cpt_jo_assets_sweep", "lwf_jo_assets_sweep",
        "ai_corpse", "rock_collision_00trg", "soak_00trg", "native_assets_00trg",
        "npruntime_remote_body_state", "npruntime_held_weapon_attach",
        "npruntime_authored_payload_00trg",
    ],
    "mission_corpus": ["mission_corpus"],
}

# Mixed ctests: a `SKIP-LEG:` line naming the mounted root is a gap.
MIXED = {
    "jo_dir": [
        "terrain_tile_composer", "ground_conform", "minimap_overlay", "score_roundtrip",
        "playersav_weapon_sav", "npruntime_weapon_table",
    ],
    "jo_assets": [
        "occlusion_armry", "threedi_panm_ctrl", "particle_smoke_all_fixtures", "sound_profile",
        "def_parse_items", "infantry", "minimap_overlay",
    ],
    "mission_corpus": [],
}

# Gated tests the reference data cannot serve yet; reported, never a gap.
KNOWN_ABSENT = {
    "mission_coop_convoy": "05TRcoop.bms is on no known retail mount or corpus",
    "mission_script_report": "05TRcoop.bms is on no known retail mount or corpus",
    "bunker_walkin": "05TRcoop.bms is on no known retail mount or corpus",
    "netsim_client_replica_pipeline_capture_parent_follow":
        "needs <OPENNOVA_CAPTURES>/golden/retail-vehicle-session.pcapng beside items.def; "
        "captures never ride CI",
}


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
    for line in text.splitlines():
        if "[Pending]" in line and any(v in line for v in vars_):
            gaps.append(line.strip())
    return sorted(set(gaps))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--junit", type=Path, help="ctest --output-junit report")
    ap.add_argument("--gut-log", type=Path, help="the GUT run's captured output")
    ap.add_argument("--roots", default="jo_dir,jo_assets,mission_corpus",
                    help="comma-separated mounted roots (jo_dir, jo_assets, mission_corpus)")
    args = ap.parse_args()
    roots = [r.strip() for r in args.roots.split(",") if r.strip()]
    unknown = [r for r in roots if r not in ROOT_VARS]
    if unknown:
        print(f"[retail-gates] unknown roots: {', '.join(unknown)}")
        return 2
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
