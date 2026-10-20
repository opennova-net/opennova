#!/usr/bin/env python3
"""Select and attest the test suite, its ctest rows and its Godot scripts.

CTest labels and the Godot test directory layout are the source of truth:
tests/windowed/ holds the graphics scripts that need a live RenderingDevice
and run only with --windowed. Reports must contain every selected test; an
empty or incomplete run never passes. No test reads retail data
(--check-layout).
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

REPO = Path(__file__).resolve().parents[2]


# The GUT suite's CI shards, each on its own runner: `shells`, the scripts under
# the directories of the shells and what drives them (tests/authoring the
# editor's, tests/game the game shell's, tests/mcp their MCP endpoints',
# tests/probes the runtime probes' contracts), and `world`, every other script.
# A script is in one or the other by its first directory under tests/, so the
# two partition the suite by construction. On the Windows runs of 2026-10-18
# they split 406 s of scripts 207 s / 199 s.
GODOT_SHELL_DIRS = ("authoring", "game", "mcp", "probes")
GODOT_SHARDS = ("shells", "world")


def godot_shard(script: str) -> str:
    """The GUT shard a script (res://tests/...) runs in."""
    dirs = script.removeprefix("res://").split("/")[1:-1]
    return "shells" if dirs[:1] and dirs[0] in GODOT_SHELL_DIRS else "world"


def godot_scripts(repo: Path, *, windowed: bool = False, shard: str | None = None) -> list[str]:
    if shard is not None and shard not in GODOT_SHARDS:
        raise ValueError("a Godot shard is shells or world")
    scripts = []
    for path in sorted((repo / "godot/tests").rglob("*_test.gd")):
        rel = path.relative_to(repo / "godot")
        if (list(rel.parts[1:-1])[:1] == ["windowed"]) == windowed:
            scripts.append("res://" + rel.as_posix())
    if shard is not None:
        scripts = [script for script in scripts if godot_shard(script) == shard]
    return scripts


def godot_methods(repo: Path, scripts: list[str]) -> dict[str, list[str]]:
    return {script: re.findall(r"^func (test_\w+)\(", (
        repo / "godot" / script.removeprefix("res://")).read_text(encoding="utf-8"),
        re.MULTILINE) for script in scripts}


# No test reads retail data (the suite runs on minted and authored fixtures and
# the tree's own assets/): nothing under tests/ or godot/tests/ names a retail
# root variable or the retired retail-root helpers.
RETAIL_READS = ("OPENNOVA_JO_", "retail_paths", "retail_mission_files", "RetailData")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".gd", ".py", ".cmake", ".txt", ".json"}


def layout_errors(repo: Path) -> list[str]:
    errors = []
    for top in ("tests", "godot/tests"):
        for path in sorted((repo / top).rglob("*")):
            if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for token in RETAIL_READS:
                if token in text:
                    errors.append(f"test reads retail data ({token}): {path.relative_to(repo).as_posix()}")
    return errors


# The ctest suite's CI shards, each on its own runner: the `editor` label
# (tests/editor/CMakeLists.txt gives it to every row there) and the rest. Every
# row has the label or not, so the two partition the suite by construction.
SHARDS = {
    "editor": ["-L", "^editor$"],
    "rest": ["-LE", "^editor$"],
}


def ctest_selection(shard: str | None = None) -> list[str]:
    """The ctest label arguments that select the suite, or one shard of it."""
    return list(SHARDS[shard]) if shard is not None else []


def ctest_inventory(build: Path, shard: str | None = None) -> list[str]:
    command = ["ctest", "--test-dir", str(build), "-C", "Release", "--show-only=json-v1"]
    command += ctest_selection(shard)
    data = json.loads(subprocess.check_output(command, text=True, encoding="utf-8"))
    return [test["name"] for test in data["tests"]]


def check_report(report: Path, expected: list[str], *, godot: bool,
                 methods: dict[str, list[str]] | None = None,
                 windowed: bool = False) -> list[str]:
    cases = list(ET.parse(report).getroot().iter("testcase"))
    errors = []
    if not expected:
        errors.append("suite selection is empty")
    if not cases:
        errors.append("test report is empty")
    if godot:
        # GUT's classname is the script path (possibly followed by an inner class).
        seen = {case.get("classname", "").replace("\\", "/").removeprefix("res://") for case in cases}
        expected = [name.removeprefix("res://") for name in expected]
        missing = [name for name in expected if not any(
            actual == name or actual.startswith(name + ".") for actual in seen)]
        errors += [f"unexpected script: {name}" for name in sorted(seen)
                   if not any(name == script or name.startswith(script + ".") for script in expected)]
        for script, names in (methods or {}).items():
            script = script.removeprefix("res://")
            actual = {case.get("name", "") for case in cases
                      if case.get("classname", "").removeprefix("res://") == script}
            errors += [f"method not in report: {script}:{name}" for name in names if name not in actual]
    else:
        seen = {case.get("name", "") for case in cases}
        missing = sorted(set(expected) - seen)
        unexpected = sorted(seen - set(expected))
        errors += [f"unexpected test: {name}" for name in unexpected]
    errors += [f"not in report: {name}" for name in missing]
    for case in cases:
        name = case.get("classname", "") + ":" + case.get("name", "")
        if case.find("failure") is not None or case.find("error") is not None:
            errors.append(f"failed: {name}")
        if windowed and case.find("skipped") is not None:
            # A windowed script exists to draw: a pending there (no
            # RenderingDevice, the wrong renderer) is missing coverage.
            errors.append(f"windowed test skipped: {name}")
        # A row whose data is absent (the service's database and Redis tests
        # without their servers) prints "SKIP: needs" and exits 77: a skip
        # proves nothing, so the suite fails it.
        if "SKIP: needs" in (case.findtext("system-out") or ""):
            errors.append(f"test skipped: {name}")
    return errors


def check_log(path: Path, *, windowed: bool = False) -> list[str]:
    errors = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = re.sub(r"\x1b\[[0-9;]*m", "", line)
        if "SCRIPT ERROR:" in line or ("Ignoring script" in line and "does not extend GutTest" in line):
            errors.append(line.strip())
        elif windowed and ("[Pending]" in line or "[Script skipped]" in line):
            errors.append(line.strip())
    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--shard", choices=tuple(SHARDS) + GODOT_SHARDS,
                    help="one CI shard: of the ctest selection and inventory (editor, rest), "
                         "or, with --godot-config, of the Godot scripts (shells, world)")
    ap.add_argument("--selection", action="store_true",
                    help="print the suite's (or shard's) ctest label arguments, one per line")
    ap.add_argument("--godot-config", type=Path)
    ap.add_argument("--inventory", type=Path)
    ap.add_argument("--build", type=Path)
    ap.add_argument("--report", type=Path)
    ap.add_argument("--gut-log", type=Path)
    ap.add_argument("--check-layout", action="store_true",
                    help="fail any test under tests/ or godot/tests/ that reads retail data")
    ap.add_argument("--windowed", action="store_true",
                    help="select only the graphics scripts (tests/windowed/); a pending test then fails")
    args = ap.parse_args()
    if args.shard and (args.shard in GODOT_SHARDS) != bool(args.godot_config):
        ap.error("--shard shells|world splits the Godot scripts (--godot-config), editor|rest the ctest rows")
    if args.selection:
        for arg in ctest_selection(args.shard):
            print(arg)
        return 0
    errors = layout_errors(REPO) if args.check_layout else []
    if args.godot_config:
        expected = godot_scripts(REPO, windowed=args.windowed, shard=args.shard)
        if not expected:
            errors.append("suite selection is empty")
        config = {"dirs": [], "tests": expected, "prefix": "", "suffix": "_test.gd",
                  "include_subdirs": False, "should_exit": True,
                  "junit_xml_file": str(args.report.resolve()) if args.report else "",
                  "junit_xml_timestamp": False}
        args.godot_config.parent.mkdir(parents=True, exist_ok=True)
        args.godot_config.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
        if args.inventory:
            args.inventory.write_text(json.dumps({"scripts": expected,
                "methods": godot_methods(REPO, expected)}, indent=2) + "\n", encoding="utf-8")
    elif args.build and not args.report:
        expected = ctest_inventory(args.build, args.shard)
        if not expected:
            errors.append("suite selection is empty")
        if args.inventory:
            args.inventory.parent.mkdir(parents=True, exist_ok=True)
            args.inventory.write_text(json.dumps(expected, indent=2) + "\n", encoding="utf-8")
    elif args.report:
        if not args.inventory:
            ap.error("--report requires --inventory")
        inventory = json.loads(args.inventory.read_text(encoding="utf-8"))
        expected = inventory["scripts"] if isinstance(inventory, dict) else inventory
        methods = inventory.get("methods") if isinstance(inventory, dict) else None
        if not args.report.is_file():
            errors.append(f"missing report: {args.report}")
        else:
            errors += check_report(args.report, expected, godot=args.gut_log is not None,
                                   methods=methods, windowed=args.windowed)
    if args.gut_log:
        errors += check_log(args.gut_log, windowed=args.windowed)
    for error in errors:
        print(f"[test-suites] {error}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
