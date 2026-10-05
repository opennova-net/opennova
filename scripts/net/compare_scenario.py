#!/usr/bin/env python3
"""Compare network-parity scenario runs (ADR 0050 rung R5).

Each argument is a run-summary.json that scripts/net/run_parity_topology.ps1
wrote for a -Scenario run. For every run this decodes the evidence capture
with `opennova-wire --scenario-events`, keeps the events between the scenario's start
and the end of its settle, and names the actor and its pool-0 index: a joiner
by its first C2S 0x0C uplink's handle, the host by roster slot 0's.

Every run must have played the same script (its run-root copy's mission,
game type, actor, prelude, setup, steps and settle). The expectations come
from the scenario's source file (or --scenario), so they can be tightened
without replaying. A run passes when `expect.sequence` matches its events in order.
Across runs, every matched event's fields and every `expect.count_kinds` count
must equal the reference run's (RR when present, else the first argument),
except the capture-local fields (frame, ts_ns, session, pos) and the
scenario's cited `expect.mask` fields.

Pattern fields: a string must equal the event's value, "actor" names the
actor's handle or pool-0 index, and a list accepts any of its values.

usage: compare_scenario.py [--wire EXE] [--scenario FILE] [--out REPORT.json]
                           SUMMARY.json...
Exit status 0 when every run passes and agrees with the reference.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_WIRE = REPO / "build" / "apps" / "wire" / "Release" / "opennova-wire.exe"
# Fields that differ between captures of the same play: capture order and
# clock, the client port, and world positions (each topology spawns anew).
CAPTURE_LOCAL = {"frame", "ts_ns", "session", "decode", "pos"}
HANDLE_FIELDS = {"entity", "shooter", "target", "handle", "carrier", "sender",
                 "vehicle", "victim_slot", "zone"}
INDEX_FIELDS = {"attacker", "victim", "aux"}
PLAYED_FIELDS = ("mission", "game_type", "actor", "prelude", "setup", "steps",
                 "settle_seconds")


def parse_utc_ns(text: str) -> int:
    """PowerShell's round-trip ("o") UTC timestamp, to Unix nanoseconds."""
    match = re.fullmatch(
        r"(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d+))?(Z|[+-]00:00)", text)
    if not match:
        raise ValueError(f"not a UTC round-trip timestamp: {text!r}")
    year, month, day, hour, minute, second = (int(match.group(i)) for i in range(1, 7))
    fraction = (match.group(7) or "").ljust(9, "0")[:9]
    stamp = dt.datetime(year, month, day, hour, minute, second, tzinfo=dt.timezone.utc)
    return int(stamp.timestamp()) * 1_000_000_000 + int(fraction)


def decode_events(wire: Path, capture: Path) -> list[dict[str, str]]:
    result = subprocess.run([str(wire), str(capture), "--scenario-events"],
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"opennova-wire failed on {capture}: {result.stderr.strip()}")
    events = []
    for line in result.stdout.splitlines():
        if not line.startswith("SCENARIO_EVENT "):
            continue
        fields = dict(part.split("=", 1) for part in line.split()[1:])
        events.append(fields)
    return events


def actor_of(events: list[dict[str, str]], actor: str) -> tuple[str, str]:
    """The driven player's handle (hex word) and pool-0 index (decimal): a
    joiner's is its first C2S 0x0C uplink's, the host's is roster slot 0's."""
    for event in events:
        if actor == "joiner" and event["dir"] == "C" and event["kind"] == "carrier":
            handle = int(event["handle"], 16)
            break
        if (actor == "host" and event["dir"] == "S" and event["kind"] == "player_sync"
                and event.get("slot") == "0" and "entity" in event):
            handle = int(event["entity"], 16)
            break
    else:
        raise RuntimeError(f"the capture does not name the {actor} actor")
    index = str(handle & 0x0FFF) if handle >> 12 == 0 else ""
    return f"0x{handle:04x}", index


def normalize(event: dict[str, str], actor: tuple[str, str]) -> dict[str, str]:
    handle, index = actor
    out = dict(event)
    for key, value in event.items():
        if key in HANDLE_FIELDS and value == handle:
            out[key] = "actor"
        elif key in INDEX_FIELDS and index and value == index:
            out[key] = "actor"
    return out


def matches(pattern: dict, event: dict[str, str]) -> bool:
    for key, want in pattern.items():
        have = event.get(key)
        if isinstance(want, list):
            if have not in [str(item) for item in want]:
                return False
        elif have != str(want):
            return False
    return True


def match_sequence(patterns: list[dict], events: list[dict[str, str]]) -> list:
    """The first in-order match of every pattern (None where one is missing)."""
    found: list = []
    cursor = 0
    for pattern in patterns:
        hit = None
        for position in range(cursor, len(events)):
            if matches(pattern, events[position]):
                hit = position
                break
        found.append(hit)
        if hit is not None:
            cursor = hit + 1
    return found


def analyze(summary_path: Path, wire: Path, expectations: Path | None) -> dict:
    summary = json.loads(summary_path.read_text(encoding="utf-8-sig"))
    witness = summary.get("scenario_witness")
    if not witness:
        raise RuntimeError(f"{summary_path} is not a scenario run (no scenario_witness)")
    played_path = Path(witness["path"])
    digest = hashlib.sha256(played_path.read_bytes()).hexdigest()
    if digest != witness["sha256"]:
        raise RuntimeError(f"{played_path} changed since run {summary['run_id']}")
    played = json.loads(played_path.read_text(encoding="utf-8"))
    source = expectations or Path(witness["source"])
    scenario = json.loads(source.read_text(encoding="utf-8"))
    events = decode_events(wire, Path(summary["evidence_capture"]))
    actor = actor_of(events, played.get("actor", "joiner"))
    start = parse_utc_ns(witness["started_utc"])
    stop = parse_utc_ns(witness["settled_utc"])
    window = [normalize(event, actor) for event in events
              if start <= int(event["ts_ns"]) <= stop]
    expect = scenario.get("expect", {})
    hits = match_sequence(expect.get("sequence", []), window)
    counts = {kind: sum(1 for event in window if event["kind"] == kind)
              for kind in expect.get("count_kinds", [])}
    return {
        "run_id": summary["run_id"],
        "topology": summary["topology"],
        "summary": str(summary_path),
        "scenario": witness["name"],
        "played": {key: played.get(key) for key in PLAYED_FIELDS},
        "expectations": str(source),
        "capture": summary["evidence_capture"],
        "perspective": summary.get("evidence_perspective", ""),
        "actor": {"handle": actor[0], "pool0_index": actor[1]},
        "driver": witness.get("driver", ""),
        "window_events": len(window),
        "matched": [window[hit] if hit is not None else None for hit in hits],
        "missing": [index for index, hit in enumerate(hits) if hit is None],
        "counts": counts,
        "expect": expect,
    }


def comparable(event: dict[str, str], masked: set[str]) -> dict[str, str]:
    return {key: value for key, value in event.items()
            if key not in CAPTURE_LOCAL and key not in masked}


def compare(reference: dict, cell: dict) -> list[str]:
    mask = reference["expect"].get("mask", {})
    differences = []
    for index, (want, have) in enumerate(zip(reference["matched"], cell["matched"])):
        if want is None or have is None:
            continue
        masked = set(mask.get(want["kind"], []))
        left, right = comparable(want, masked), comparable(have, masked)
        if left != right:
            keys = sorted(key for key in set(left) | set(right) if left.get(key) != right.get(key))
            differences.append(
                f"sequence[{index}] {want['kind']}: "
                + ", ".join(f"{key} {left.get(key)} != {right.get(key)}" for key in keys))
    for kind, want in reference["counts"].items():
        if cell["counts"].get(kind) != want:
            differences.append(f"count {kind}: {want} != {cell['counts'].get(kind)}")
    return differences


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("summaries", nargs="+", type=Path)
    parser.add_argument("--wire", type=Path, default=DEFAULT_WIRE)
    parser.add_argument("--scenario", type=Path,
                        help="expectations file (default: each run's scenario source)")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()

    cells = [analyze(path, args.wire, args.scenario) for path in args.summaries]
    if any(cell["played"] != cells[0]["played"] for cell in cells):
        raise SystemExit("the runs played different scripts")
    reference = next((cell for cell in cells if cell["topology"] == "RR"), cells[0])
    ok = True
    for cell in cells:
        cell["passed_expectations"] = not cell["missing"]
        cell["differences"] = [] if cell is reference else compare(reference, cell)
        cell_ok = cell["passed_expectations"] and not cell["differences"]
        ok = ok and cell_ok
        status = "PASS" if cell_ok else "FAIL"
        print(f"{status} {cell['topology']} {cell['run_id']} ({cell['perspective']}, "
              f"actor {cell['actor']['handle']}, {cell['window_events']} events)")
        for index in cell["missing"]:
            print(f"  missing sequence[{index}]: {json.dumps(cell['expect']['sequence'][index])}")
        for difference in cell["differences"]:
            print(f"  vs {reference['topology']}: {difference}")
    report = {
        "schema": "opennova.parity-scenario-compare.v1",
        "scenario": cells[0]["scenario"],
        "reference": reference["run_id"],
        "ok": ok,
        "cells": cells,
    }
    if args.out:
        args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
