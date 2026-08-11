#!/usr/bin/env python3
"""ADR 0020's terrain seam, enforced at include level: engine/net, wac and
mission reach terrain only through the four terrain_query headers. Moved here
from CMake-target level (link_graph_check.py) by ADR 0029 / Shape A, whose
group targets can no longer carry the seam."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# The consumer trees on the far side of the seam. engine/runtime/world is
# ADR 0020's protagonist — the lib the query capability exists FOR — so it
# is scanned too (0 violations at add time; this locks the property in).
SCAN_TREES = (
    "engine/net",
    "engine/runtime/wac",
    "engine/runtime/mission",
    "engine/runtime/world",
)

SOURCE_SUFFIXES = (".h", ".cpp", ".c", ".hpp", ".cc")
SKIP_PARTS = {"third_party", "build"}

# Include prefixes that live behind the seam. Since the 2026-08-10 engine
# flatten, terrain_query owns its own terrain_query/ include prefix
# (pre-flatten it shared terrain/ with the terrain lib as disjoint header
# sets), so terrain/ is fully forbidden and the seam surface is the exact
# terrain_query header set below. tpm/ (the TPM1 tile mesh) and dep/ (the
# depth-buffer intermediate) were blanket-forbidden under terrain/ before
# their formats extraction (ADR 0030) and stay forbidden by their new
# prefixes.
FORBIDDEN_PREFIXES = ("cpt/", "til/", "trn/", "tpj/", "foliage/", "terrain/",
                      "terrain_query/", "tpm/", "dep/")
TERRAIN_QUERY_HEADERS = {
    "terrain_query/coords.h",
    "terrain_query/height_field.h",
    "terrain_query/surface_type_map.h",
    "terrain_query/terrain_raycast.h",
}

INCLUDE_LINE = re.compile(r'^\s*#\s*include\s*[<"]([^<>"]+)[>"]')


def source_files() -> tuple[list[Path], list[str]]:
    """Source files under the scanned trees, plus any missing tree names."""
    files: list[Path] = []
    missing: list[str] = []
    for tree in SCAN_TREES:
        root = REPO / tree
        if not root.is_dir():
            missing.append(tree)
            continue
        for path in sorted(root.rglob("*")):
            if path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            if SKIP_PARTS.intersection(path.relative_to(REPO).parts):
                continue
            if path.is_file():
                files.append(path)
    return files, missing


def forbidden(header: str) -> bool:
    header = header.replace("\\", "/")
    if not header.startswith(FORBIDDEN_PREFIXES):
        return False
    return header not in TERRAIN_QUERY_HEADERS


def scan() -> tuple[list[str], int, list[str]]:
    """(violation lines, file count scanned, missing trees)."""
    violations: list[str] = []
    files, missing = source_files()
    for path in files:
        rel = path.relative_to(REPO).as_posix()
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            m = INCLUDE_LINE.match(line)
            if m and forbidden(m.group(1)):
                violations.append(f"{rel}:{lineno}: {line.strip()}")
    return violations, len(files), missing


def main() -> int:
    # Windows consoles default to cp1252; scanned lines may carry UTF-8.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--enforce", action="store_true",
                        help="violations exit 1 (what CI runs)")
    args = parser.parse_args()

    violations, scanned, missing = scan()
    for tree in missing:
        print(f"[include-graph] WARNING: scan tree missing: {tree} "
              "(moved? update SCAN_TREES — an absent tree vacates the seam)")
    print(f"[include-graph] scanned {scanned} file(s) under "
          f"{len(SCAN_TREES)} tree(s): {len(violations)} forbidden include(s)")
    for v in violations:
        print(f"[include-graph][forbidden] {v}")

    if violations and args.enforce:
        print("[include-graph] FAIL: net/wac/mission may reach terrain only "
              "through the terrain_query seam headers (terrain_query/coords.h, "
              "terrain_query/height_field.h, terrain_query/surface_type_map.h, "
              "terrain_query/terrain_raycast.h) — ADR 0020. Route the access "
              "through terrain_query, or move the logic behind the seam.")
        return 1
    if missing and args.enforce:
        print("[include-graph] FAIL: a scanned tree is missing — the seam "
              "gate would be silently vacated.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
