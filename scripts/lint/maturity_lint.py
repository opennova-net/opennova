#!/usr/bin/env python3
"""Maturity-program diff-scoped lint (docs/maturity-program.md, STD-1).

Checks ONLY lines ADDED in the given diff range — untouched code is never
flagged (the ratchet in ratchet_counts.py covers the stock):

  dict-contract (ADR 0017): new Dictionary-shaped public contracts in
      godot/modtools/ or godot/engine/ GDScript — a public `-> Dictionary`
      return, a public `var x: Dictionary`, or a `const NAME := {` map
      table, all at CLASS level (column 0). Indented declarations are
      function-locals — annotating a local as Dictionary is not a new
      contract (the Wave-1 soft run flagged 36 such locals; that was the
      lint's bug, fixed at the Wave-2 flip). Inner-class members are the
      accepted blind spot of the column-0 heuristic. New contracts are
      typed records (RefCounted/Resource); Dictionaries belong only at
      transport/serialization edges, which go in the allowlist (substring
      match on "path|line"). Moved legacy declarations re-flag under a
      diff-scoped lint (a move reads as an add) — allowlist them or take
      the maintainer escape hatch in the PR that moves them.
  magic-number (advisory, never fails): added lines carrying bare numeric
      literals outside const/enum/citation contexts. Heuristic by design —
      it informs review, it does not gate.

Range resolution: --range wins; else origin/master...HEAD when available;
else HEAD~1..HEAD; else skip cleanly (fresh shallow clones).

Soft mode (default) always exits 0. --enforce makes dict-contract findings
exit 1 (the Wave-2 flip per the umbrella doc).
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BASELINE_PATH = Path(__file__).resolve().parent / "maturity_baseline.json"

LINT_SCOPES = ("godot/modtools/", "godot/engine/")

# Column 0 only: class-level declarations. GDScript function bodies are
# indented, so an indented match is a local, not a contract.
DICT_RETURN = re.compile(r"^(?:static\s+)?func\s+([a-z][a-z0-9_]*)\s*\(.*->\s*Dictionary\b")
DICT_PUBVAR = re.compile(r"^(?:@export\s+)?var\s+([a-z][a-z0-9_]*)\s*:\s*Dictionary\b")
DICT_CONST_TABLE = re.compile(r"^const\s+(_?[A-Z][A-Z0-9_]*)\s*:?=\s*\{")

# 2+ digit bare literals (ints or floats), skipping obvious non-magic lines.
MAGIC_NUMBER = re.compile(r"(?<![\w.])\d{2,}(?:\.\d+)?(?![\w.])")
MAGIC_EXEMPT = re.compile(r"const\s|enum\s|\[orig|^\s*#|^\s*//|preload\(|Color\(|Vector2i?\(|Vector3\(")


def run_git(*args: str) -> str:
    return subprocess.run(
        ["git", *args], cwd=REPO, check=True,
        capture_output=True, text=True, encoding="utf-8").stdout


def resolve_range(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    for candidate in ("origin/master...HEAD", "HEAD~1..HEAD"):
        probe = candidate.split("...")[0].split("..")[0]
        try:
            subprocess.run(["git", "rev-parse", "--verify", "--quiet", probe],
                           cwd=REPO, check=True, capture_output=True)
            return candidate
        except subprocess.CalledProcessError:
            continue
    return None


def added_lines(diff_range: str) -> list[tuple[str, int, str]]:
    """(path, line_number, text) for every added line in scope."""
    out: list[tuple[str, int, str]] = []
    diff = run_git("diff", "-U0", diff_range, "--", *LINT_SCOPES)
    path, lineno = "", 0
    for raw in diff.splitlines():
        if raw.startswith("+++ b/"):
            path = raw[6:]
        elif raw.startswith("@@"):
            m = re.search(r"\+(\d+)", raw)
            lineno = int(m.group(1)) if m else 0
        elif raw.startswith("+") and not raw.startswith("+++"):
            out.append((path, lineno, raw[1:]))
            lineno += 1
        elif raw.startswith("-") or raw.startswith(" "):
            pass
    return out


# Promoted named constants (W1-4): these witnessed values have exactly one
# canonical home per language and NEW bare literals of them are hard findings.
#   7597        the NovaWorld gate UDP port    gate_probe.h / net_ports.h / HostSessionConfig
#   32768/32787 the retail LAN host port range net_ports.h / HostSessionConfig
#   0x30020     the retail Co-op g_GameType    HostSessionConfig.GAME_TYPE_COOP
PROMOTED_LITERAL = re.compile(r"(?<![\w.])(?:7597|32768|32787|0x30020)(?![\w.])", re.IGNORECASE)
PROMOTED_SCOPES = ("godot/engine/", "godot/game/", "godot/modtools/", "libs/", "apps/")
PROMOTED_SUFFIXES = (".gd", ".cpp", ".h", ".hpp", ".c")
PROMOTED_CANONICAL = (
    "godot/engine/world/host_session_config.gd",
    "libs/novaworld/include/novaworld/gate_probe.h",
    "libs/npwire/include/npwire/net_ports.h",
)
PROMOTED_EXEMPT = re.compile(r"^\s*#|^\s*//|\[orig|\bconst\s|\bconstexpr\s|#define\s")


def promoted_literal_findings(diff_range: str) -> list[str]:
    findings: list[str] = []
    diff = run_git("diff", "-U0", diff_range, "--", *PROMOTED_SCOPES)
    path, lineno = "", 0
    for raw in diff.splitlines():
        if raw.startswith("+++ b/"):
            path = raw[6:]
        elif raw.startswith("@@"):
            m = re.search(r"\+(\d+)", raw)
            lineno = int(m.group(1)) if m else 0
        elif raw.startswith("+") and not raw.startswith("+++"):
            text = raw[1:]
            if path.endswith(PROMOTED_SUFFIXES) and path not in PROMOTED_CANONICAL \
                    and PROMOTED_LITERAL.search(text) and not PROMOTED_EXEMPT.search(text):
                findings.append(f"{path}:{lineno}: {text.strip()}")
            lineno += 1
        elif raw.startswith("-") or raw.startswith(" "):
            pass
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--range", dest="diff_range", default=None,
                        help="git diff range (default: origin/master...HEAD)")
    parser.add_argument("--enforce", action="store_true",
                        help="dict-contract findings exit 1 (Wave-2 flip)")
    args = parser.parse_args()

    diff_range = resolve_range(args.diff_range)
    if diff_range is None:
        print("[lint] no usable diff base (shallow clone?) — skipping")
        return 0

    config = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    allow = config.get("dict_contract_allowlist", [])

    dict_findings: list[str] = []
    magic_findings: list[str] = []

    for path, lineno, text in added_lines(diff_range):
        if not path.endswith(".gd"):
            continue
        where = f"{path}:{lineno}"
        for pattern in (DICT_RETURN, DICT_PUBVAR, DICT_CONST_TABLE):
            m = pattern.match(text)
            if m:
                key = f"{path}|{text.strip()}"
                if any(entry in key for entry in allow):
                    break
                dict_findings.append(f"{where}: {text.strip()}")
                break
        if MAGIC_NUMBER.search(text) and not MAGIC_EXEMPT.search(text):
            magic_findings.append(f"{where}: {text.strip()}")

    print(f"[lint] range {diff_range}: "
          f"{len(dict_findings)} dict-contract finding(s), "
          f"{len(magic_findings)} magic-number line(s) (advisory)")
    for f in dict_findings:
        print(f"[lint][dict-contract] {f}")
        print("[lint]   ADR 0017: new cross-object contracts are typed records "
              "(RefCounted/Resource); Dictionaries only at transport edges "
              "(allowlist in scripts/lint/maturity_baseline.json).")
    for f in magic_findings[:20]:
        print(f"[lint][magic-number] {f}")
    if len(magic_findings) > 20:
        print(f"[lint][magic-number] ... and {len(magic_findings) - 20} more")

    promoted_findings = promoted_literal_findings(diff_range)
    for f in promoted_findings:
        print(f"[lint][promoted-literal] {f}")
        print("[lint]   this value has a canonical named home "
              "(HostSessionConfig / novaworld gate_probe.h / npwire net_ports.h) "
              "— reference it instead of re-minting the literal.")

    if (dict_findings or promoted_findings) and args.enforce:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
