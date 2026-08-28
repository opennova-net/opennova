#!/usr/bin/env python3
"""Engine layering, enforced at include level (ADR 0040; ADR 0020's terrain
seam re-homed here by ADR 0029).

`engine/` is the ONE public include root: an engine header is always included
as `<group/lib/file.h>` with group in {base, formats, runtime, net}. The group
in the path is what makes the layering visible, so this check reads it:

  1. GROUP ORDER — the PUBLIC link chain of ADR 0029 d3
     (io -> crt -> formats -> base -> runtime -> net):
       engine/base/io, engine/base/crt   include only base/io, base/crt
       engine/formats/**                 include base/io, base/crt, formats
       engine/base/**                    include base, formats
       engine/runtime/**                 include base, formats, runtime
       engine/net/**                     include base, formats, runtime, net
  2. QUALIFIED — no include under engine/, apps/, tests/ or godot/src names an
     engine lib by its bare prefix (`<world/x.h>`); the group is mandatory. A
     quoted include that resolves locally (the includer's own directory, the
     godot/src binding root, tests/, apps/<app>/) is not an engine include.
  3. TERRAIN SEAM (ADR 0020) — engine/net, runtime/wac, runtime/mission and
     runtime/world reach terrain only through runtime/terrain_query's seam
     headers; runtime/terrain and the terrain formats (cpt, til, trn, tpj,
     foliage) are forbidden there.
  4. GODOT-FREE — no include under engine/, apps/ or tests/ names godot; the
     engine's portability is a ratcheted property, not a re-verified one.
  5. BINDING ROOT — godot/src has no subdirectory named like an engine group,
     so a binding's quoted root-relative include can never alias an engine path.

Modes:
  (default)   report violations; exit 0
  --enforce   exit 1 on any violation (what CI runs)
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ENGINE = REPO / "engine"
GROUPS = ("base", "formats", "runtime", "net")
SCAN_ROOTS = ("engine", "apps", "tests", "godot/src")
GODOT_FREE_ROOTS = ("engine", "apps", "tests")
# ADR 0040: a quoted include names a same-directory sibling and nothing else; a
# `../` reach is the unqualified include in disguise (tests keep their
# "../common/..." support includes: the test tree is not an include root).
PARENT_RELATIVE_FORBIDDEN_ROOTS = ("engine", "apps", "godot")
SOURCE_SUFFIXES = (".h", ".hpp", ".hh", ".c", ".cc", ".cpp", ".cxx", ".inl")
SKIP_PARTS = {"third_party"}

# Rule 1: what each engine tree may include, as (group, lib-or-None) pairs.
ALLOWED = {
    "base/io": {("base", "io"), ("base", "crt")},
    "base/crt": {("base", "io"), ("base", "crt")},
    "formats": {("base", "io"), ("base", "crt"), ("formats", None)},
    "base": {("base", None), ("formats", None)},
    "runtime": {("base", None), ("formats", None), ("runtime", None)},
    "net": {("base", None), ("formats", None), ("runtime", None), ("net", None)},
}

# Rule 3: the consumer trees on the far side of the seam. engine/runtime/world
# is ADR 0020's protagonist — the lib the query capability exists FOR — so it
# is scanned too.
SEAM_TREES = ("engine/net", "engine/runtime/wac", "engine/runtime/mission",
              "engine/runtime/world")
SEAM_FORBIDDEN_PREFIXES = ("runtime/terrain/", "runtime/terrain_query/",
                           "formats/cpt/", "formats/til/", "formats/trn/",
                           "formats/tpj/", "formats/foliage/")
TERRAIN_QUERY_HEADERS = {
    "runtime/terrain_query/coords.h",
    "runtime/terrain_query/height_field.h",
    "runtime/terrain_query/surface_type_map.h",
    "runtime/terrain_query/terrain_raycast.h",
    # The permanent scorch record + its two retail producers (ADR 0020 #5:
    # world-owned interfaces grow here; the registry/textures stay behind).
    "runtime/terrain_query/terrain_scorch_record.h",
}

INCLUDE_LINE = re.compile(r'^\s*#\s*include\s*([<"])([^<>"]+)[>"]')


def engine_libs() -> dict[str, set[str]]:
    return {g: {p.name for p in (ENGINE / g).iterdir() if p.is_dir()} for g in GROUPS}


def _skip(rel: Path) -> bool:
    # Directory parts only: a FILE named build_*.cpp is source, not build output.
    return any(part in SKIP_PARTS or part.startswith("build") for part in rel.parts[:-1])


def source_files() -> list[Path]:
    files: list[Path] = []
    for root in SCAN_ROOTS:
        base = REPO / root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix.lower() not in SOURCE_SUFFIXES or not path.is_file():
                continue
            rel = path.relative_to(REPO)
            if _skip(rel):
                continue
            files.append(rel)
    return files


def local_roots(rel: Path) -> list[Path]:
    roots = [REPO / rel.parent]
    if rel.parts[0] == "godot":
        roots.append(REPO / "godot" / "src")
    elif rel.parts[0] == "tests":
        roots.append(REPO / "tests")
    elif rel.parts[0] == "apps" and len(rel.parts) > 2:
        roots.append(REPO / "apps" / rel.parts[1])
        roots.append(REPO / "apps" / "common")
    return roots


def includer_tree(rel: Path) -> str | None:
    """The ALLOWED key for an engine source, or None outside engine/."""
    if rel.parts[0] != "engine" or len(rel.parts) < 3:
        return None
    group, lib = rel.parts[1], rel.parts[2]
    if group == "base" and lib in ("io", "crt"):
        return f"base/{lib}"
    return group


def allowed(tree: str, group: str, lib: str) -> bool:
    return any(g == group and (l is None or l == lib) for g, l in ALLOWED[tree])


def scan() -> tuple[list[str], int]:
    libs = engine_libs()
    all_libs = set().union(*libs.values())
    violations: list[str] = []
    files = source_files()
    for rel in files:
        posix = rel.as_posix()
        try:
            text = (REPO / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        in_seam = any(posix == t or posix.startswith(t + "/") for t in SEAM_TREES)
        tree = includer_tree(rel)
        for lineno, line in enumerate(text.splitlines(), 1):
            m = INCLUDE_LINE.match(line)
            if not m:
                continue
            quote, inc = m.group(1), m.group(2).replace("\\", "/").strip()
            where = f"{posix}:{lineno}: {line.strip()}"
            if rel.parts[0] in GODOT_FREE_ROOTS and "godot" in inc.lower():
                violations.append(f"[godot-free] {where}")
                continue
            if quote == '"' and "../" in inc and rel.parts[0] in PARENT_RELATIVE_FORBIDDEN_ROOTS:
                violations.append(f"[parent-relative] {where} (only a same-directory sibling may be a quoted include; ADR 0040)")
                continue
            parts = [p for p in inc.split("/") if p]
            if len(parts) < 2:
                continue
            first = parts[0]
            if first in GROUPS and (ENGINE / inc).is_file():
                group, lib = parts[0], parts[1]
                if tree is not None and not allowed(tree, group, lib):
                    violations.append(f"[group-order] {where} (engine/{tree} may not include {group}/{lib})")
                if in_seam and inc.startswith(SEAM_FORBIDDEN_PREFIXES) and inc not in TERRAIN_QUERY_HEADERS:
                    violations.append(f"[terrain-seam] {where}")
                continue
            if first in all_libs:
                if quote == '"' and any((r / inc).is_file() for r in local_roots(rel)):
                    continue  # a local, binding, or test-root include
                violations.append(f"[unqualified] {where} (engine headers are <group/lib/file.h>)")
    for name in sorted(GROUPS):
        if (REPO / "godot" / "src" / name).exists():
            violations.append(f"[binding-root] godot/src/{name}/ is named like an engine group")
    return violations, len(files)


def main() -> int:
    # Windows consoles default to cp1252; scanned lines may carry UTF-8.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--enforce", action="store_true",
                        help="violations exit 1 (what CI runs)")
    args = parser.parse_args()

    violations, scanned = scan()
    print(f"[include-graph] scanned {scanned} file(s) under {len(SCAN_ROOTS)} root(s): "
          f"{len(violations)} violation(s)")
    for v in violations:
        print(f"[include-graph]{v}")
    if violations and args.enforce:
        print("[include-graph] FAIL: engine headers are included as <group/lib/file.h>; "
              "a tree includes only the groups below it (ADR 0029 d3); net/wac/mission/"
              "world reach terrain only through runtime/terrain_query's seam headers "
              "(ADR 0020); nothing under engine/, apps/ or tests/ includes godot.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
