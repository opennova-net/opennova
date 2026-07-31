#!/usr/bin/env python3
"""Maturity-program ratchet counters (docs/maturity-program.md, STD-1).

Counts debt classes that must never INCREASE, against the committed baseline
in maturity_baseline.json:

  test_private_pokes    lines in godot/tests/**/*.gd that access an
                        _underscore member of ANOTHER object (self._ excluded)
                        -- ADR 0018: each is a missing public seam.
  libs_uncited_src_files  files under libs/<lib>/src with zero "[orig"
                        citations, excluding the allowlisted infra libs
                        (citation is inapplicable there) -- the faithful-port
                        rule's coverage floor.

Modes:
  (default)         report counts vs baseline; exit 0 regardless (soft mode)
  --enforce         exit 1 if any counter exceeds its baseline (the ratchet)
  --write-baseline  rewrite the baseline to current counts (maintainer
                    action; log the bump in docs/maturity-program.md)

Decreases are reported and SHOULD be committed into the baseline by the
slice that earned them (run --write-baseline in that slice) so the ratchet
only ever tightens.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BASELINE_PATH = Path(__file__).resolve().parent / "maturity_baseline.json"

# Matches an _member access on another object. "self._" hits are stripped
# before this runs; remaining `._name` is a foreign private access.
PRIVATE_POKE = re.compile(r"\._[a-z]")
SELF_POKE = re.compile(r"self\._")


def count_test_private_pokes() -> int:
    count = 0
    for path in (REPO / "godot" / "tests").rglob("*.gd"):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line in text.splitlines():
            stripped = SELF_POKE.sub("", line)
            if PRIVATE_POKE.search(stripped):
                count += 1
    return count


def count_libs_uncited_src_files(allowlist: set[str]) -> int:
    count = 0
    libs = REPO / "libs"
    for lib_dir in sorted(p for p in libs.iterdir() if p.is_dir()):
        if lib_dir.name in allowlist:
            continue
        src = lib_dir / "src"
        if not src.is_dir():
            continue
        for path in src.rglob("*"):
            if path.suffix.lower() not in (".c", ".cc", ".cpp"):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if "[orig:" not in text:
                count += 1
    return count


LIBS_PRINT = re.compile(
    r"(?<![\w:])(?:std::)?fprintf\s*\(\s*(?:stderr|stdout)\b"
    r"|(?<![\w:.])(?:std::)?printf\s*\("
    r"|std::cout\b|std::cerr\b"
    r"|(?<![\w:.])puts\s*\(")


def count_libs_stdout_prints() -> int:
    """Console writes inside libs/ (W1-3): libraries route diagnostics through
    the io/log.h sink and stay silent by default — the embedder installs the sink.
    FILE*-parameter writers (fprintf(fp, ...)) are deliberately not matched."""
    count = 0
    libs = REPO / "libs"
    for sub in ("src", "include"):
        for path in libs.glob(f"*/{sub}/**/*"):
            if path.suffix.lower() not in (".c", ".cc", ".cpp", ".h", ".hpp"):
                continue
            if path.name == "log.h" and path.parent.name == "io":
                continue  # the sink's own vsnprintf lives here
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for line in text.splitlines():
                code = line.split("//", 1)[0]
                if LIBS_PRINT.search(code):
                    count += 1
    return count


GD_PRINT = re.compile(r"(?:^|[^_a-zA-Z\"])(?:print|prints|printerr|print_rich|print_debug)\s*\(")
# CLI drivers whose stdout IS the product; everything else in the shipping
# godot layer routes through push_error/push_warning, print_verbose, or the
# F3 debug system.
GD_PRINT_ALLOWLIST = {"godot/modtools/tools/screenshot_capture.gd"}
CPP_CONSOLE = re.compile(
    r"UtilityFunctions::print(?!_verbose)\s*\(|UtilityFunctions::printerr\s*\("
    r"|(?<![\w:])print_line\s*\(|\bWARN_PRINT(?:_ONCE|_ED)?\s*\(|\bERR_PRINT(?:_ONCE|_ED)?\s*\(")


def count_gd_prints_outside_debug() -> int:
    """Raw print() family in the shipping godot layer (W1-2): the sanctioned
    channels are push_error/push_warning, print_verbose, and the F3 overlay.
    godot/tests and the GUT addon are out of scope (probes print by design)."""
    count = 0
    for sub in ("engine", "game", "modtools"):
        for path in (REPO / "godot" / sub).rglob("*.gd"):
            rel = path.relative_to(REPO).as_posix()
            if rel in GD_PRINT_ALLOWLIST:
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for line in text.splitlines():
                code = line.split("#", 1)[0]
                if "print_verbose" in code:
                    code = code.replace("print_verbose", "")
                if GD_PRINT.search(code):
                    count += 1
    return count


HAS_METHOD_GUARD = re.compile(r"(?<!\w)has_method\s*\(")


def count_has_method_guards() -> int:
    """Duck-type guards in the shipping godot layer (W4-2): the floor is the
    documented kept set (harness seams, workspace capability hooks, dynamic
    dispatch) - not zero. class_has_method is excluded by the word boundary."""
    count = 0
    for sub in ("engine", "game", "modtools"):
        for path in (REPO / "godot" / sub).rglob("*.gd"):
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for line in text.splitlines():
                count += len(HAS_METHOD_GUARD.findall(line.split("#", 1)[0]))
    return count


OVERSIZE_CPP_LINE_LIMIT = 2500


def count_oversize_cpp_files() -> int:
    """Oversized translation units (W3-7, the W3 closer): the god-file splits
    leave two residual offenders; no .cpp under libs/, apps/, or godot/engine
    may grow past 2500 lines without splitting first."""
    count = 0
    for root in ("libs", "apps", "godot/engine"):
        for path in (REPO / root).rglob("*.cpp"):
            parts = path.relative_to(REPO).parts
            if "build" in parts:  # local CMake/godot-cpp build output, not source
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if len(text.splitlines()) > OVERSIZE_CPP_LINE_LIMIT:
                count += 1
    return count


OVERSIZE_GD_LINE_LIMIT = 1200


def count_oversize_gd_files() -> int:
    """Oversized GDScript files (W4-6, the W4 closer): the W4 god-file splits
    leave seven residual offenders; no .gd under godot/engine, godot/game, or
    godot/modtools may grow past 1200 lines without splitting first.
    godot/tests is deliberately out of scope: eleven test files already exceed
    the limit and the test refit is ONED-TST's concern, not this ratchet's."""
    count = 0
    for root in ("godot/engine", "godot/game", "godot/modtools"):
        for path in (REPO / root).rglob("*.gd"):
            parts = path.relative_to(REPO).parts
            if "addons" in parts or "build" in parts:  # vendored addons / build output, not source
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if len(text.splitlines()) > OVERSIZE_GD_LINE_LIMIT:
                count += 1
    return count


def count_cpp_binding_console_writes() -> int:
    """Console writes in the GDExtension bindings (W1-2): error paths use
    push_error/push_warning (the engine's error channel); narration uses
    print_verbose. Raw print/printerr/print_line and the WARN/ERR_PRINT
    macros are ratcheted at zero."""
    count = 0
    for path in (REPO / "godot" / "engine").rglob("*"):
        if path.suffix.lower() not in (".cpp", ".h", ".hpp"):
            continue
        parts = path.relative_to(REPO).parts
        if "build" in parts:  # local CMake/godot-cpp build output, not source
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line in text.splitlines():
            code = line.split("//", 1)[0]
            if CPP_CONSOLE.search(code):
                count += 1
    return count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--enforce", action="store_true",
                        help="exit 1 when a counter exceeds its baseline")
    parser.add_argument("--write-baseline", action="store_true",
                        help="rewrite the baseline to the current counts")
    args = parser.parse_args()

    config = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    allowlist = set(config.get("citation_allowlist_libs", []))
    baseline = config.get("counters", {})

    current = {
        "test_private_pokes": count_test_private_pokes(),
        "libs_uncited_src_files": count_libs_uncited_src_files(allowlist),
        "libs_stdout_prints": count_libs_stdout_prints(),
        "gd_prints_outside_debug": count_gd_prints_outside_debug(),
        "cpp_binding_console_writes": count_cpp_binding_console_writes(),
        "oversize_cpp_files": count_oversize_cpp_files(),
        "oversize_gd_files": count_oversize_gd_files(),
        "has_method_guards": count_has_method_guards(),
    }

    if args.write_baseline:
        config["counters"] = current
        BASELINE_PATH.write_text(
            json.dumps(config, indent=2, sort_keys=True) + "\n",
            encoding="utf-8", newline="\n")
        print(f"[ratchet] baseline rewritten: {current}")
        return 0

    failed = False
    for name, value in current.items():
        base = baseline.get(name)
        if base is None:
            print(f"[ratchet] {name}: {value} (NO BASELINE — run --write-baseline)")
            failed = True
            continue
        delta = value - base
        marker = "OK" if delta <= 0 else "INCREASED"
        print(f"[ratchet] {name}: {value} (baseline {base}, {delta:+d}) {marker}")
        if delta > 0:
            failed = True
        elif delta < 0:
            print(f"[ratchet]   improvement — commit it: "
                  f"python scripts/lint/ratchet_counts.py --write-baseline")

    if failed and args.enforce:
        print("[ratchet] FAIL: a counter increased (or lacks a baseline). "
              "Fix the regression, or a maintainer bumps the baseline and "
              "logs it in docs/maturity-program.md.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
