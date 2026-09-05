#!/usr/bin/env python3
"""Record and compare per-tick simulation digests across a structural slice
(ADR 0043; the instrument behind tests/mission/tick_digest_test.cpp).

A structural refactor proves it changed no behavior by producing the same
digest chain before and after. The synthetic mission's chain is committed in
the test itself; this script drives the RETAIL missions, whose digests depend
on the local toolchain's float codegen and are therefore compared on one
machine and never committed.

    python scripts/parity/tick_digest.py --record before      # on the base commit
    python scripts/parity/tick_digest.py --check before       # after the slice

Needs OPENNOVA_JO_DIR (docs/asset-gated-tests.md) and a built
tick_digest_test binary (scripts/build.sh --no-godot; --binary overrides the
path). Records live under build/tick_digest/<label>.json, outside the tree.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RECORD_DIR = REPO / "build" / "tick_digest"
DEFAULT_MISSIONS = ("00TRg.bms", "00TRa.bms", "04TR.bms")
DEFAULT_TICKS = 1500


def find_binary(explicit: str | None) -> Path | None:
    if explicit:
        p = Path(explicit)
        return p if p.is_file() else None
    candidates = [
        REPO / "build" / "tests" / "Release" / "tick_digest_test.exe",
        REPO / "build" / "tests" / "tick_digest_test.exe",
        REPO / "build" / "tests" / "tick_digest_test",
        REPO / "build" / "Release" / "tick_digest_test.exe",
    ]
    for c in candidates:
        if c.is_file():
            return c
    return None


def run_digest(binary: Path, mission: str, ticks: int, listen: bool) -> str | None:
    args = [str(binary), "--mission", mission, "--ticks", str(ticks)]
    if not listen:
        args.append("--no-listen")
    proc = subprocess.run(args, capture_output=True, text=True, encoding="utf-8",
                          errors="replace")
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[0] == "digest":
            return parts[3]
    # The binary resolves the retail root itself (tests/common/retail_paths.h)
    # and reports `SKIP: needs OPENNOVA_JO_DIR` when it is unset.
    print(f"[tick-digest] {mission}: no digest line (exit {proc.returncode}):\n{proc.stdout}")
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--record", metavar="LABEL", help="record digests under LABEL")
    mode.add_argument("--check", metavar="LABEL", help="compare digests against LABEL")
    parser.add_argument("--missions", nargs="*", default=list(DEFAULT_MISSIONS))
    parser.add_argument("--ticks", type=int, default=DEFAULT_TICKS)
    parser.add_argument("--no-listen", action="store_true",
                        help="the bare no-net tick instead of the SP listen frame")
    parser.add_argument("--binary", default=None, help="path to tick_digest_test")
    args = parser.parse_args()

    binary = find_binary(args.binary)
    if binary is None:
        print("[tick-digest] tick_digest_test binary not found (build first, or --binary)")
        return 1

    current: dict[str, str] = {}
    for mission in args.missions:
        digest = run_digest(binary, mission, args.ticks, not args.no_listen)
        if digest is None:
            return 1
        current[mission] = digest
        print(f"[tick-digest] {mission} x{args.ticks}: {digest}")

    label = args.record or args.check
    record_path = RECORD_DIR / f"{label}.json"
    if args.record:
        RECORD_DIR.mkdir(parents=True, exist_ok=True)
        record_path.write_text(json.dumps({"ticks": args.ticks, "listen": not args.no_listen,
                                           "digests": current}, indent=2) + "\n",
                               encoding="utf-8")
        print(f"[tick-digest] recorded {len(current)} mission(s) -> {record_path}")
        return 0

    if not record_path.is_file():
        print(f"[tick-digest] no record named {label} ({record_path})")
        return 1
    recorded = json.loads(record_path.read_text(encoding="utf-8"))
    if recorded.get("ticks") != args.ticks or recorded.get("listen") != (not args.no_listen):
        print("[tick-digest] the record was taken with different ticks/listen settings")
        return 1
    failures = 0
    for mission, digest in current.items():
        before = recorded["digests"].get(mission)
        if before == digest:
            print(f"[tick-digest] {mission}: identical")
        else:
            print(f"[tick-digest] {mission}: CHANGED {before} -> {digest}")
            failures += 1
    if failures:
        print(f"[tick-digest] FAIL: {failures} mission(s) changed behavior")
        return 1
    print("[tick-digest] all identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
