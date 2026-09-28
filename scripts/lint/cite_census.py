#!/usr/bin/env python3
"""Witness-address census (ADR 0043): the SET of original-binary addresses the
code trees cite never loses a member silently.

Every `[orig: Name @0xADDR]` citation (and every `0xADDR..0xADDR` range inside
one) names an address in the original binary that some line of ours is a
structural translation of. A structural refactor moves that line — into a
method, another translation unit, another directory — and MUST carry the cite
with it; a slice that loses an address either deleted verified dead code or
lost a witness. The census makes the difference visible: the baseline is the
map `address -> occurrence count` over engine/, godot/, apps/ and tests/;
a run compares the current map against it.

  --enforce        exit 1 when a baselined address no longer occurs anywhere
                   (a LOST witness). A count that merely dropped is reported
                   as a dedup (a doc-comment duplicate folded into the one
                   engine cite, ADR 0042 d7's ratified exception) and passes;
                   a new address is an added witness and passes.
  --write-baseline rewrite the baseline to the current map. The commit that
                   does so names every dropped address and the dead code it
                   went with.
  --audit-range R  the rename-phase diff guard (absorbed from the retired
                   host_lint.py --frozen-audit): fails if the range touches a
                   wire/retail-frozen scope, drops a wire-frozen string, or
                   loses a docs/ citation or divergence id that no longer
                   resolves anywhere under docs/. docs/ is the private docs
                   submodule: a range that moves its pointer is audited
                   through the submodule's own diff, so it must be checked
                   out. Local tool, not CI.

Default mode reports and exits 0.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BASELINE_PATH = Path(__file__).resolve().parent / "cite_census_baseline.json"

SCOPES = ("engine", "godot/src", "godot/game", "godot/probes",
          "apps", "tests")
SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".gd")
EXCLUDED_PARTS = ("third_party", "addons")

# The anchor address of a cite (`@0x52b630`, `@ 0x52b630`) and the far end of a
# cited range (`0x40F157..0x40F173`). Both are witnesses.
ANCHOR = re.compile(r"@\s*0x([0-9A-Fa-f]{4,8})\b")
RANGE_END = re.compile(r"\.\.\s*0x([0-9A-Fa-f]{4,8})\b")


def _in_build_dir(parts: tuple[str, ...]) -> bool:
    if not parts:
        return False
    if parts[0].startswith("build"):
        return True
    if len(parts) > 1 and parts[0] == "godot" and parts[1].startswith("build"):
        return True
    return len(parts) > 2 and parts[0] == "godot" and parts[1] == "src" and \
        parts[2].startswith("build")


def canonical(hex_digits: str) -> str:
    return f"0x{int(hex_digits, 16):x}"


def census() -> Counter:
    counts: Counter = Counter()
    for scope in SCOPES:
        root = REPO / scope
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if path.suffix.lower() not in SUFFIXES:
                continue
            parts = path.relative_to(REPO).parts
            if _in_build_dir(parts) or any(p in EXCLUDED_PARTS for p in parts):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for match in ANCHOR.finditer(text):
                counts[canonical(match.group(1))] += 1
            for match in RANGE_END.finditer(text):
                counts[canonical(match.group(1))] += 1
    return counts


def load_baseline() -> dict[str, int]:
    if not BASELINE_PATH.is_file():
        return {}
    data = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    return {k: int(v) for k, v in data.get("addresses", {}).items()}


def write_baseline(counts: Counter) -> None:
    payload = {
        "_comment": "scripts/lint/cite_census.py --write-baseline: witness address -> "
                    "occurrence count over engine/, godot/, apps/, tests/. A dropped "
                    "address is named in the commit that drops it (ADR 0043).",
        "addresses": dict(sorted(counts.items())),
    }
    BASELINE_PATH.write_text(json.dumps(payload, indent=1, sort_keys=False) + "\n",
                             encoding="utf-8", newline="\n")


def report(counts: Counter, baseline: dict[str, int], enforce: bool) -> int:
    lost = sorted(addr for addr in baseline if counts.get(addr, 0) == 0)
    dedup = sorted(addr for addr, base in baseline.items()
                   if 0 < counts.get(addr, 0) < base)
    added = sorted(addr for addr in counts if addr not in baseline)
    print(f"[cite-census] {len(counts)} distinct witness addresses, "
          f"{sum(counts.values())} occurrences; baseline {len(baseline)} addresses: "
          f"{len(lost)} lost, {len(dedup)} deduplicated, {len(added)} added")
    for addr in dedup[:40]:
        print(f"[cite-census][dedup] {addr}: {baseline[addr]} -> {counts[addr]}")
    if len(dedup) > 40:
        print(f"[cite-census][dedup] ... and {len(dedup) - 40} more")
    for addr in lost:
        print(f"[cite-census][LOST] {addr} (was cited {baseline[addr]} time(s))")
    if not baseline:
        print("[cite-census] NO BASELINE - run --write-baseline")
        return 1 if enforce else 0
    if lost:
        print("[cite-census] FAIL: a witness address disappeared from the code trees. "
              "Carry the cite with the code it cites, or (verified dead code only) "
              "bank the drop with --write-baseline and name the address in the commit.")
        return 1 if enforce else 0
    return 0


# --- the rename-phase diff guard (ex host_lint.py --frozen-audit) -------------

# Wire/retail-frozen strings: a rename phase must never make one of these
# disappear from the tree. The audit balances removed vs added diff lines: a
# token removed more times than it is re-added means a frozen name changed.
FROZEN_TOKENS = (
    "ClientHostRequest", "ClientHostUpdate", "ClientStopHosting",
    "ClientHostPlayerAdded", "ClientHostPlayerRemoved", "ServerHostResult",
    "HostAcceptEvent", "HostSessionAccept", "\"HostSetup\"", "_var_list(\"Host\"",
    "MULTI_PLAYER_HOST", "HG_SERVEPLAY", "HG_SERVEONLY", "HOST_URL",
    "HOSTKEY", "hostIp", "hostPort", "/api/hosts", "active_hosts",
    "host_players",
)
FROZEN_PATHS = ("fixtures/", "backend/migrations/",
                "apps/novaworld_server/templates/", "engine/net/napi/",
                "third_party/")
CITATION = re.compile(r"\[orig:[^\]]*\]")
DIVERGENCE_ID = re.compile(r"\bD-[A-Z]+-\d+\b")


def run_git(*args: str) -> str:
    return subprocess.run(
        ["git", *args], cwd=REPO, check=True,
        capture_output=True, text=True, encoding="utf-8").stdout


def diff_lines(diff_range: str, *paths: str, submodule_diff: bool = False):
    """(sign, path, text) for every added/removed line in the range; with
    submodule_diff a submodule pointer move expands into the submodule's diff."""
    args = ["diff", "-U0", *(["--submodule=diff"] if submodule_diff else []), diff_range]
    diff = run_git(*args, "--", *paths) if paths else run_git(*args)
    path = ""
    for raw in diff.splitlines():
        if raw.startswith("+++ b/"):
            path = raw[6:]
        elif raw.startswith(("+++", "---", "@@")):
            continue
        elif raw.startswith(("+", "-")):
            yield raw[0], path, raw[1:]


def docs_still_carry(item: str) -> bool:
    for root, _dirs, files in os.walk(REPO / "docs"):
        for fname in files:
            if not fname.endswith(".md"):
                continue
            try:
                body = (Path(root) / fname).read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if item in body:
                return True
    return False


def audit_range(diff_range: str) -> int:
    failures = 0
    touched = run_git("diff", "--name-only", diff_range, "--", *FROZEN_PATHS).splitlines()
    for path in touched:
        print(f"[cite-census][frozen] frozen scope touched: {path}")
        failures += 1

    # docs/ is the private docs submodule: its pointer move expands into its own
    # diff, so text moved into or within it is not read as lost. That expansion
    # needs docs/ checked out holding both ends of the range.
    moved = run_git("diff", "--name-only", diff_range, "--", "docs").strip()
    if moved and (not (REPO / "docs" / ".git").exists()
                  or "(commits not present)" in run_git("diff", "--submodule=log", diff_range, "--", "docs")):
        print("[cite-census][frozen] the range moves the docs/ pointer but docs/ is not "
              "checked out at both ends: `git submodule update --init docs` (or fetch in "
              "docs/), then re-audit")
        failures += 1
    docs_lines = list(diff_lines(diff_range, "docs", submodule_diff=True))

    removed: Counter = Counter()
    added: Counter = Counter()
    for sign, _path, text in [*diff_lines(diff_range, ":(exclude)docs"), *docs_lines]:
        bucket = removed if sign == "-" else added
        for token in FROZEN_TOKENS:
            bucket[token] += text.count(token)
    for token in FROZEN_TOKENS:
        if removed[token] > added[token]:
            print(f"[cite-census][frozen] wire-frozen string lost: {token} "
                  f"(-{removed[token]} +{added[token]})")
            failures += 1

    doc_removed: Counter = Counter()
    doc_added: Counter = Counter()
    for sign, _path, text in docs_lines:
        bucket = doc_removed if sign == "-" else doc_added
        for cite in CITATION.findall(text):
            bucket[cite] += 1
        for did in DIVERGENCE_ID.findall(text):
            bucket[did] += 1
    for item, count in doc_removed.items():
        if count <= doc_added[item]:
            continue
        if docs_still_carry(item):
            print(f"[cite-census][frozen] docs citation/ID moved (still resolves): "
                  f"{item} (-{count} +{doc_added[item]})")
            continue
        print(f"[cite-census][frozen] docs citation/ID lost: {item} "
              f"(-{count} +{doc_added[item]})")
        failures += 1

    if failures:
        print(f"[cite-census] audit-range FAIL: {failures} finding(s)")
        return 1
    print("[cite-census] audit-range clean")
    return 0


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--enforce", action="store_true",
                        help="exit 1 when a baselined address is no longer cited anywhere")
    parser.add_argument("--write-baseline", action="store_true",
                        help="rewrite the baseline to the current census")
    parser.add_argument("--audit-range", metavar="RANGE", default=None,
                        help="the rename-phase diff guard over a git range")
    args = parser.parse_args()

    if args.audit_range:
        return audit_range(args.audit_range)

    counts = census()
    if args.write_baseline:
        write_baseline(counts)
        print(f"[cite-census] baseline rewritten: {len(counts)} addresses, "
              f"{sum(counts.values())} occurrences")
        return 0
    return report(counts, load_baseline(), args.enforce)


if __name__ == "__main__":
    sys.exit(main())
