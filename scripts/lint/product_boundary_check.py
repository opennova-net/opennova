#!/usr/bin/env python3
"""Product/engine source-tree boundary check (ADR 0015/0016).

The three Godot source trees ship in different products (feature-tagged main
scenes + export exclude_filters, ADR 0015), and dependencies point
applications -> adapter -> engine (ADR 0016). Concretely:

  godot/engine/    ships in BOTH products: may reference neither product tree
  godot/game/      the game shell: may not reference res://modtools/
  godot/modtools/  ONED: may not reference res://game/

A finding is the literal substring "res://modtools/" or "res://game/" in a
scanned source file of the wrong tree -- load()/preload() calls, .tscn/.tres
ext_resource paths, C++ string literals, comments included (a commented
cross-product path is one uncomment away from a package that null-loads at
runtime; reword it or allowlist it). The export exclude_filters and the
packaging boot smoke only catch boot-reachable loads at package time; this
lint makes the direction rule mechanical at PR time, over lazily-loaded
paths too.

Allowlist: "product_boundary_allowlist" in scripts/lint/maturity_baseline.json,
substring-matched against "path|line" (the dict-contract lint's convention).
The standing entry is the See-in-game launcher, which passes the game's main
scene path to a separate spawned process -- an argument, never a load.

Soft mode (default) always exits 0. --enforce makes findings exit 1.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BASELINE_PATH = Path(__file__).resolve().parent / "maturity_baseline.json"

# (tree, forbidden res:// prefixes for files in that tree)
SCOPES = (
    ("godot/engine", ("res://modtools/", "res://game/")),
    ("godot/game", ("res://modtools/",)),
    ("godot/modtools", ("res://game/",)),
)

# Source and scene formats that can carry a res:// reference. Deliberately
# not .md/.txt (docs may name paths) and not .import/.uid (per-asset
# metadata, always same-tree).
SCAN_SUFFIXES = {
    ".gd", ".tscn", ".tres", ".gdshader", ".cfg", ".json",
    ".cpp", ".h", ".hpp", ".c", ".cc",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--enforce", action="store_true",
                        help="findings exit 1")
    args = parser.parse_args()

    config = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    allow = config.get("product_boundary_allowlist", [])

    findings: list[str] = []
    scanned = 0
    for tree, forbidden in SCOPES:
        root = REPO / tree
        if not root.is_dir():
            print(f"[boundary] WARNING: {tree} missing (moved? update SCOPES)")
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file() or path.suffix not in SCAN_SUFFIXES:
                continue
            scanned += 1
            rel = path.relative_to(REPO).as_posix()
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for lineno, line in enumerate(text.splitlines(), start=1):
                hit = next((p for p in forbidden if p in line), None)
                if hit is None:
                    continue
                key = f"{rel}|{line.strip()}"
                if any(entry in key for entry in allow):
                    continue
                findings.append(f"{rel}:{lineno}: {hit} [{line.strip()[:120]}]")

    print(f"[boundary] {scanned} files scanned: {len(findings)} cross-product "
          f"reference(s)")
    for f in findings:
        print(f"[boundary][forbidden] {f}")
    if findings:
        print("[boundary]   ADR 0016: engine/ references neither product tree; "
              "game/ and modtools/ never reference each other. Move the code "
              "to the tree that owns it (shared -> godot/engine/), or "
              "allowlist a sanctioned non-load mention in "
              "scripts/lint/maturity_baseline.json.")

    if findings and args.enforce:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
