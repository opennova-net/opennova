#!/usr/bin/env python3
"""Select and attest the core and retail test suites without private data in core.

CTest labels and the Godot test directory layout are the source of truth:
tests/retail/ holds the retail scripts, and a windowed/ directory at either
level (tests/windowed/, tests/retail/windowed/) holds the graphics scripts
that need a live RenderingDevice and run only with --windowed. Reports must
contain every selected test; an empty or incomplete run never passes.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

REPO = Path(__file__).resolve().parents[2]
ROOTS = ("OPENNOVA_JO_DIR", "OPENNOVA_JO_ASSETS")


def godot_scripts(repo: Path, suite: str, *, windowed: bool = False) -> list[str]:
    scripts = []
    for path in sorted((repo / "godot/tests").rglob("*_test.gd")):
        rel = path.relative_to(repo / "godot")
        dirs = list(rel.parts[1:-1])
        retail = dirs[:1] == ["retail"]
        is_windowed = dirs[1 if retail else 0:][:1] == ["windowed"]
        if is_windowed != windowed:
            continue
        if suite == "all" or retail == (suite == "retail"):
            scripts.append("res://" + rel.as_posix())
    return scripts


def godot_methods(repo: Path, scripts: list[str]) -> dict[str, list[str]]:
    return {script: re.findall(r"^func (test_\w+)\(", (
        repo / "godot" / script.removeprefix("res://")).read_text(encoding="utf-8"),
        re.MULTILINE) for script in scripts}


def layout_errors(repo: Path) -> list[str]:
    errors = []
    for script in godot_scripts(repo, "core") + godot_scripts(repo, "core", windowed=True):
        source = (repo / "godot" / script.removeprefix("res://")).read_text(encoding="utf-8")
        code = "\n".join(line for line in source.splitlines() if not line.lstrip().startswith("#"))
        if "RetailData." in code or "PresenterFixture.stage(" in code or "InstalledCombatHud." in code:
            errors.append(f"core script reads retail data: {script}")
    # The native migration record (docs/asset-gated-tests.md): every mixed
    # entry stays registered through opennova_add_mixed_test and every source
    # file it names still exists.
    record = repo / "scripts/ci/native_migration.json"
    if record.is_file():
        data = json.loads(record.read_text(encoding="utf-8"))
        cmake = repo / "tests/CMakeLists.txt"
        forms = re.findall(r"opennova_add_mixed_test\(\s*([\w${}]+)",
                           cmake.read_text(encoding="utf-8") if cmake.is_file() else "")
        registered = [re.compile("^" + re.sub(r"\\\$\\\{\w+\\\}", r"\\w+", re.escape(form)) + "$")
                      for form in forms]
        errors += [f"native migration entry is not a mixed ctest: {name}"
                   for name in data["mixed_entries"] if not any(p.match(name) for p in registered)]
        errors += [f"native migration source missing: {source}"
                   for source in data["sources"] if not (repo / source).is_file()]
    # Historical assertion homes are reviewable without becoming the selector.
    manifest = repo / "scripts/ci/retail_migration.json"
    if manifest.is_file():
        for row in json.loads(manifest.read_text(encoding="utf-8")):
            for suite in ("core", "retail"):
                script = row[f"{suite}_script"]
                if script is None:
                    continue
                path = repo / script
                actual = re.findall(r"^func (test_\w+)\(", path.read_text(encoding="utf-8"),
                                    re.MULTILINE) if path.is_file() else []
                errors += [f"migrated method missing: {script}:{name}"
                           for name in row[f"{suite}_methods"] if name not in actual]
    return errors


def ctest_inventory(build: Path, suite: str) -> list[str]:
    command = ["ctest", "--test-dir", str(build), "-C", "Release", "--show-only=json-v1"]
    if suite != "all":
        command += ["-L" if suite == "retail" else "-LE", "^retail$"]
    data = json.loads(subprocess.check_output(command, text=True, encoding="utf-8"))
    return [test["name"] for test in data["tests"]]


def check_roots() -> list[str]:
    roots = {"OPENNOVA_JO_DIR": os.environ.get("OPENNOVA_JO_DIR", ""),
             "OPENNOVA_JO_ASSETS": os.environ.get("OPENNOVA_JO_ASSETS", "")}
    return [f"{name} must name an existing directory" for name, value in roots.items()
            if not value or not Path(value).is_dir()]


def check_report(report: Path, expected: list[str], *, godot: bool, suite: str,
                 methods: dict[str, list[str]] | None = None,
                 allowed_skipped_scripts: set[str] | None = None,
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
        skipped_scripts = allowed_skipped_scripts or set()
        missing = [name for name in expected if not any(
            actual == name or actual.startswith(name + ".") for actual in seen)
                   and name not in skipped_scripts]
        errors += [f"unexpected script: {name}" for name in sorted(seen)
                   if not any(name == script or name.startswith(script + ".") for script in expected)]
        for script, names in (methods or {}).items():
            script = script.removeprefix("res://")
            if script in skipped_scripts:
                continue
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
        output = case.findtext("system-out") or ""
        skipped = case.find("skipped")
        skip_text = "" if skipped is None else ET.tostring(skipped, encoding="unicode")
        if suite == "retail" and skipped is not None:
            errors.append(f"retail test skipped: {name}")
        elif windowed and skipped is not None and not (
                suite == "all" and any(root in skip_text for root in ROOTS)):
            # A windowed script exists to draw: a pending there (no
            # RenderingDevice, the wrong renderer) is missing coverage. Local
            # `all` keeps its explicit missing-data skips, as headless does.
            errors.append(f"windowed test skipped: {name}")
        if suite != "all" and ("SKIP-LEG:" in output or any(root in skip_text for root in ROOTS)):
            errors.append(f"retail coverage not exercised: {name}")
        if suite == "core" and "SKIP: needs" in output:
            errors.append(f"gated test in core: {name}")
    return errors


def skipped_retail_scripts(log: Path) -> set[str]:
    """Local --suite all may report an explicit missing-data script skip."""
    current = ""
    skipped = set()
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        line = re.sub(r"\x1b\[[0-9;]*m", "", line).strip()
        if line.startswith("res://tests/"):
            current = line.removeprefix("res://")
        if current.startswith("tests/retail/") and "[Script skipped]" in line and any(root in line for root in ROOTS):
            skipped.add(current)
    return skipped


def check_log(path: Path, suite: str, *, windowed: bool = False) -> list[str]:
    errors = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = re.sub(r"\x1b\[[0-9;]*m", "", line)
        if "SCRIPT ERROR:" in line or ("Ignoring script" in line and "does not extend GutTest" in line):
            errors.append(line.strip())
        pending = "[Pending]" in line or "[Script skipped]" in line
        if suite != "all" and ("SKIP-LEG:" in line or (
                pending and any(root in line for root in ROOTS))):
            errors.append(line.strip())
        elif windowed and pending and not (suite == "all" and any(root in line for root in ROOTS)):
            errors.append(line.strip())
    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--suite", choices=("core", "retail", "all"), required=True)
    ap.add_argument("--godot-config", type=Path)
    ap.add_argument("--inventory", type=Path)
    ap.add_argument("--build", type=Path)
    ap.add_argument("--report", type=Path)
    ap.add_argument("--gut-log", type=Path)
    ap.add_argument("--require-roots", action="store_true")
    ap.add_argument("--check-layout", action="store_true")
    ap.add_argument("--windowed", action="store_true",
                    help="select only the suite's graphics scripts (tests/windowed/ for core, "
                         "tests/retail/windowed/ for retail); a pending test then fails")
    args = ap.parse_args()
    errors = check_roots() if args.require_roots else []
    if args.check_layout:
        errors += layout_errors(REPO)
    if args.godot_config:
        expected = godot_scripts(REPO, args.suite, windowed=args.windowed)
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
        expected = ctest_inventory(args.build, args.suite)
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
        allowed = skipped_retail_scripts(args.gut_log) if args.gut_log and args.suite == "all" else set()
        if not args.report.is_file():
            errors.append(f"missing report: {args.report}")
        else:
            errors += check_report(args.report, expected, godot=args.gut_log is not None,
                                   suite=args.suite, methods=methods, allowed_skipped_scripts=allowed,
                                   windowed=args.windowed)
    if args.gut_log:
        errors += check_log(args.gut_log, args.suite, windowed=args.windowed)
    for error in errors:
        print(f"[test-suites] {error}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
