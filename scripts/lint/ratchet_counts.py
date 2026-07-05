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
            if "[orig" not in text:
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
