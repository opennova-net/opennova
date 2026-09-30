#!/usr/bin/env python3
"""Engine layering, enforced at include level (ADR 0040; ADR 0020's terrain
seam re-homed here by ADR 0029).

`engine/` is the ONE public include root: an engine header is always included
as `<group/lib/file.h>` with group in {base, formats, runtime, net}. The group
in the path is what makes the layering visible, so this check reads it:

  1. GROUP ORDER — the PUBLIC link chain of ADR 0029 d3 as flipped by
     ADR 0043 d4 (io -> crt -> formats -> base -> net -> runtime; net is
     wire only, the in-match session and replication live in runtime/):
       engine/base/io, engine/base/crt   include only base/io, base/crt
       engine/formats/**                 include base/io, base/crt, formats
       engine/base/**                    include base, formats
       engine/net/**                     include base, formats, net
       engine/runtime/**                 include base, formats, net, runtime
       engine/editor/**                  include base, formats, net, runtime, editor
     (the editor is the OpenNova Editor's portable core, ADR 0046 d3; nothing
     below it may include it)
  1b. NET-AGNOSTIC (ADR 0043 d4) — every engine/runtime lib except
     runtime/inmatch and runtime/replication stays free of net/,
     runtime/inmatch/ and runtime/replication/ includes: the world, the
     scripts, the mission kernel and every presentation compiler are
     headless facts the wire consumes, never the other way round.
  2. QUALIFIED — no include under engine/, apps/, tests/ or godot/src names an
     engine lib by its bare prefix (`<world/x.h>`); the group is mandatory. A
     quoted include that resolves locally (the includer's own directory, the
     godot/src binding root, tests/, apps/<app>/) is not an engine include.
  3. TERRAIN SEAM (ADR 0020) — runtime/inmatch, runtime/replication,
     runtime/wac, runtime/mission and runtime/world reach terrain only
     through runtime/terrain_query's seam headers; runtime/terrain and the
     terrain formats (cpt, til, trn, tpj, foliage) are forbidden there.
  4. GODOT-FREE — no include under engine/, apps/ or tests/ names godot; the
     engine's portability is a ratcheted property, not a re-verified one.
  5. BINDING ROOT — godot/src has no subdirectory named like an engine group,
     so a binding's quoted root-relative include can never alias an engine path.
  6. IMGUI CONTAINMENT (ADR 0042 d6) — an include of a Dear ImGui header (a
     path segment starting `imgui`/`imconfig`: imgui.h, imgui_internal.h,
     misc/cpp/imgui_stdlib.h, backends/imgui_impl_*.h, ...) is allowed only
     under engine/runtime/devtools/ and tests/devtools/; the engine's ImGui
     pass is the one dev-tools surface (previously the containment was a
     single CMake PRIVATE keyword). The engine's own `<runtime/devtools/
     imgui_abi.h>` seam is a group-qualified engine include, not an ImGui one.
  7. EDITOR RANK (ADR 0046 S13 D3) — inside engine/editor the editing model,
     the document types, the asset graph, the session and the windows are
     ranked model < documents < graph < session < ui: a ranked library
     includes only its own rank and below. graph/reference_kinds.h is a seam
     header any library may include (what a reference kind is, which the
     model's field schema names), like the terrain_query headers. The other
     editor libraries stay unranked. The upward includes the tree still makes
     are listed (EDITOR_RANK_ALLOWED), each with the slice that removes it; an
     entry the tree no longer makes is itself a violation, so the list only
     shrinks.

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
GROUPS = ("base", "formats", "runtime", "net", "editor")
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
    "net": {("base", None), ("formats", None), ("net", None)},
    "runtime": {("base", None), ("formats", None), ("net", None), ("runtime", None)},
    "editor": {("base", None), ("formats", None), ("net", None), ("runtime", None),
               ("editor", None)},
}

# Rule 1b: the runtime libs that carry the wire (ADR 0043 d4). Every other
# runtime lib is net-agnostic.
NET_AWARE_RUNTIME_LIBS = ("inmatch", "replication")
NET_AGNOSTIC_FORBIDDEN_PREFIXES = ("net/", "runtime/inmatch/", "runtime/replication/")

# Rule 3: the consumer trees on the far side of the seam. engine/runtime/world
# is ADR 0020's protagonist — the lib the query capability exists FOR — so it
# is scanned too.
SEAM_TREES = ("engine/runtime/inmatch", "engine/runtime/replication",
              "engine/runtime/wac", "engine/runtime/mission",
              "engine/runtime/world")
SEAM_FORBIDDEN_PREFIXES = ("runtime/terrain/", "runtime/terrain_query/",
                           "formats/cpt/", "formats/til/", "formats/trn/",
                           "formats/foliage/")
TERRAIN_QUERY_HEADERS = {
    "runtime/terrain_query/coords.h",
    "runtime/terrain_query/height_field.h",
    "runtime/terrain_query/surface_type_map.h",
    # The ONE owning cpt/trn(+charmap) field builder (ADR 0042 d4; format-free
    # store header only — terrain_field_build.h, the format-typed entry over
    # the parsed documents, stays off the seam).
    "runtime/terrain_query/terrain_field_store.h",
    "runtime/terrain_query/terrain_raycast.h",
    # The permanent scorch record + its two retail producers (ADR 0020 #5:
    # world-owned interfaces grow here; the registry/textures stay behind).
    "runtime/terrain_query/terrain_scorch_record.h",
}
# ADR 0043 (amending ADR 0020 d3): the mission kernel owns the terrain field's
# file entry, so runtime/mission alone may include the format-typed builder.
SEAM_TREE_EXTRA_HEADERS = {
    "engine/runtime/mission": {"runtime/terrain_query/terrain_field_build.h"},
}

# Rule 7: the editor's rank (ADR 0046 S13 D3).
EDITOR_RANK = {"model": 0, "documents": 1, "graph": 2, "session": 3, "ui": 4}
EDITOR_SEAM_HEADERS = {"editor/graph/reference_kinds.h"}
# (includer, included header): the upward includes the tree still makes.
EDITOR_RANK_ALLOWED = {
    # DocumentType::validate takes the project's graph; S13 D4's per-file
    # validate_file(const Document &) takes none.
    ("engine/editor/documents/document_types.h", "editor/graph/asset_graph.h"),
    # MnsDocument::style_value_use reads a variable's uses from the graph (S13
    # V1; S13 V3 moves it under field_on) and validate_styles its bindings
    # (S13 D4 moves the stylesheet's use checks into graph/use_checks).
    ("engine/editor/documents/mns_document.h", "editor/graph/asset_graph.h"),
}

INCLUDE_LINE = re.compile(r'^\s*#\s*include\s*([<"])([^<>"]+)[>"]')

# Rule 6: Dear ImGui stays behind the engine's dev-tools pass (ADR 0042 d6).
# Every form Dear ImGui ships matches — `imgui.h`, `imgui_internal.h`,
# `misc/cpp/imgui_stdlib.h`, `backends/imgui_impl_*.h`, `imconfig.h` — by its
# final path segment; `opennova_imconfig.h` deliberately does not (it names the
# project), and a group-qualified engine path (`runtime/devtools/imgui_abi.h`)
# is an engine include, checked by the group rules instead.
IMGUI_INCLUDE = re.compile(r"(?:^|/)(?:imgui|imconfig)[^/]*\.h$")
IMGUI_ALLOWED_TREES = ("engine/runtime/devtools", "tests/devtools",
                       # The OpenNova Editor's windows on the same pass (ADR 0046 d11;
                       # ADR 0042 d6 amended); godot/src/authoring stays behind the
                       # imgui_abi.h pointer seam like godot/src/devtools.
                       "engine/editor/ui", "tests/editor_ui")


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


def editor_lib(rel: Path) -> str | None:
    """The ranked editor library an engine/editor source belongs to, or None."""
    parts = rel.parts
    if len(parts) > 3 and parts[0] == "engine" and parts[1] == "editor" and parts[2] in EDITOR_RANK:
        return parts[2]
    return None


def scan() -> tuple[list[str], int]:
    libs = engine_libs()
    all_libs = set().union(*libs.values())
    violations: list[str] = []
    files = source_files()
    rank_allowed_seen: set[tuple[str, str]] = set()
    for rel in files:
        posix = rel.as_posix()
        try:
            text = (REPO / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        in_seam = any(posix == t or posix.startswith(t + "/") for t in SEAM_TREES)
        seam_extra = set().union(*(h for t, h in SEAM_TREE_EXTRA_HEADERS.items()
                                   if posix == t or posix.startswith(t + "/")))
        tree = includer_tree(rel)
        net_agnostic = tree == "runtime" and len(rel.parts) > 3 and \
                rel.parts[2] not in NET_AWARE_RUNTIME_LIBS
        editor_from = editor_lib(rel)
        for lineno, line in enumerate(text.splitlines(), 1):
            m = INCLUDE_LINE.match(line)
            if not m:
                continue
            quote, inc = m.group(1), m.group(2).replace("\\", "/").strip()
            where = f"{posix}:{lineno}: {line.strip()}"
            if rel.parts[0] in GODOT_FREE_ROOTS and "godot" in inc.lower():
                violations.append(f"[godot-free] {where}")
                continue
            # A quoted include that resolves locally is the includer's own
            # sibling / binding / test header (a quoted `"devtools/imgui_*.h"` is
            # the includer's own header, not Dear ImGui).
            resolves_locally = quote == '"' and any(
                    (r / inc).is_file() for r in local_roots(rel))
            if IMGUI_INCLUDE.search(inc) and not resolves_locally and \
                    inc.split("/")[0] not in GROUPS and not any(
                    posix == t or posix.startswith(t + "/")
                    for t in IMGUI_ALLOWED_TREES):
                violations.append(
                    f"[imgui-containment] {where} (imgui headers are allowed "
                    f"only under engine/runtime/devtools/, engine/editor/ui/ and their tests; "
                    f"ADR 0042 d6)")
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
                if net_agnostic and inc.startswith(NET_AGNOSTIC_FORBIDDEN_PREFIXES):
                    violations.append(
                            f"[net-agnostic] {where} (engine/{'/'.join(rel.parts[1:3])} is a "
                            f"headless runtime lib; only runtime/inmatch and "
                            f"runtime/replication carry the wire; ADR 0043 d4)")
                if in_seam and inc.startswith(SEAM_FORBIDDEN_PREFIXES) and \
                        inc not in TERRAIN_QUERY_HEADERS and inc not in seam_extra:
                    violations.append(f"[terrain-seam] {where}")
                if editor_from and group == "editor" and lib in EDITOR_RANK and \
                        EDITOR_RANK[lib] > EDITOR_RANK[editor_from] and inc not in EDITOR_SEAM_HEADERS:
                    if (posix, inc) in EDITOR_RANK_ALLOWED:
                        rank_allowed_seen.add((posix, inc))
                    else:
                        violations.append(
                                f"[editor-rank] {where} (engine/editor/{editor_from} may not include "
                                f"editor/{lib}: model < documents < graph < session < ui; ADR 0046 S13 D3)")
                continue
            if first in all_libs:
                if quote == '"' and any((r / inc).is_file() for r in local_roots(rel)):
                    continue  # a local, binding, or test-root include
                violations.append(f"[unqualified] {where} (engine headers are <group/lib/file.h>)")
    for includer, header in sorted(EDITOR_RANK_ALLOWED - rank_allowed_seen):
        violations.append(f"[editor-rank] {includer} no longer includes {header}: drop its "
                          f"EDITOR_RANK_ALLOWED entry")
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
              "a tree includes only the groups below it (ADR 0029 d3, net below runtime "
              "since ADR 0043 d4, editor above runtime since ADR 0046 d3); every runtime "
              "lib but inmatch/replication is "
              "net-agnostic; inmatch/replication/wac/mission/world reach terrain only "
              "through runtime/terrain_query's seam headers (ADR 0020); nothing under "
              "engine/, apps/ or tests/ includes godot; imgui headers stay under "
              "engine/runtime/devtools/ and tests/devtools/ (ADR 0042 d6); inside "
              "engine/editor, model < documents < graph < session < ui (ADR 0046 S13 D3).")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
