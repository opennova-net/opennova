#!/usr/bin/env python3
"""Link-graph seam check over the Shape A target graph.

The engine builds as six STATIC group targets plus two leaves
(engine/CMakeLists.txt): opennova_formats, opennova_base, opennova_runtime,
opennova_net, opennova_novaworld_service, opennova_editor (the OpenNova
Editor's core above runtime, ADR 0046) (+ the header-only opennova_io and
the opennova_crt STATIC leaf, the retail-parity CRT rand stream both formats
and runtime link).
The old ~400-target graph's per-format and terrain-seam rules moved to the
include-level lint (scripts/lint/include_graph_check.py); what survives at
TARGET level are the two structural seams:

  1. sqlite containment (Shape A guardrail): opennova_novaworld_service is the
     ONLY library that links opennova_sqlite. Any target whose transitive link
     closure reaches opennova_sqlite must either be the service itself or link
     the service DIRECTLY (its declared consumers: apps/novaworld_server and
     the service tests). Everything else — the engine groups, the godot
     adapter, the other apps/tests — is a violation.

  2. Forbidden edges: the game net stack never links the service leg (the
     service sits ABOVE opennova_net), and the godot adapter links
     session+gate via opennova_net, never the service.

PUBLIC vs PRIVATE keywords are deliberately not consulted: a STATIC library's
PRIVATE deps still propagate onto every downstream link line (as $<LINK_ONLY>
interface entries), so every target_link_libraries edge is a real closure edge.

Graph source, best available first (override with --source):

  graphviz  `cmake --graphviz` over an existing configured build tree
            (ground truth from CMake itself; default build/, or
            --build-dir). Re-runs configure; needs cmake on PATH. The root
            build tree does not contain the godot/src project, so the
            adapter rules only bite in files mode.
  files     a transitive walk of target_link_libraries() calls parsed
            from the repo's own CMake files — no build tree needed
            (what the pre-build CI lint step falls back to). Simple
            single-assignment set() variables (e.g. TARGET_NAME)
            are substituted so variable-named targets stay visible to
            the walk.

Soft mode (default) always exits 0. --enforce makes violations exit 1.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from collections import deque
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# Where the files-mode walk looks for CMake files. Vendored submodules are
# excluded except the ones the tree actually builds (sqlite/bcrypt/imgui).
SCAN_ROOTS = (
    "CMakeLists.txt",
    "engine",
    "apps",
    "tests",
    "tools",
    "godot/src",
    "third_party/sqlite",
    "third_party/bcrypt",
    "third_party/imgui",
    "third_party/miniz",
)

# --- The rules -------------------------------------------------------------

SQLITE_TARGET = "opennova_sqlite"
SERVICE_TARGET = "opennova_novaworld_service"
SQLITE_RULE_REASON = (
    "sqlite containment (Shape A guardrail): only opennova_novaworld_service "
    "links opennova_sqlite; consumers must link the service target directly"
)

# (reason, from-targets, to-targets, targets allowed to be absent from the
# graph without a warning — e.g. the adapter target lives in the godot/src
# CMake root, never in the repo-root build tree's graphviz dump).
FORBIDDEN: list[tuple[str, list[str], list[str], set[str]]] = [
    (
        "the game net stack never links the service leg "
        "(Shape A: opennova_novaworld_service sits ABOVE opennova_net)",
        ["opennova_net"],
        [SERVICE_TARGET],
        set(),
    ),
    (
        "net means wire (ADR 0043 d4): opennova_net never links "
        "opennova_runtime — the in-match session and replication sit ABOVE it",
        ["opennova_net", SERVICE_TARGET],
        ["opennova_runtime"],
        set(),
    ),
    (
        "the godot adapter links session+gate via opennova_net, "
        "never the service leg",
        ["opennova"],
        [SERVICE_TARGET],
        {"opennova"},  # defined only in the godot/src CMake root
    ),
    (
        "the OpenNova Editor's core sits ABOVE the engine (ADR 0046 d3): no engine "
        "group target links opennova_editor, so the game never carries editor code",
        ["opennova_formats", "opennova_base", "opennova_net", "opennova_runtime", SERVICE_TARGET],
        ["opennova_editor"],
        set(),
    ),
    (
        "the runtime GDExtension variant (the game, the Play child) never links the "
        "editor core (ADR 0046 d4): only opennova_editor_gdext does",
        ["opennova", "opennova_bindings"],
        ["opennova_editor"],
        {"opennova", "opennova_bindings"},  # defined only in the godot/src CMake root
    ),
]

LINK_KEYWORDS = {
    "PUBLIC", "PRIVATE", "INTERFACE",
    "LINK_PUBLIC", "LINK_PRIVATE", "LINK_INTERFACE_LIBRARIES",
    "debug", "optimized", "general",
}

DOT_NODE = re.compile(r'^\s*"(node\d+)"\s*\[\s*label\s*=\s*"([^"]+)"')
DOT_EDGE = re.compile(r'^\s*"(node\d+)"\s*->\s*"(node\d+)"')
TLL_CALL = re.compile(r"target_link_libraries\s*\(([^)]*)\)", re.DOTALL)
TARGET_DEF = re.compile(r"add_(?:library|executable)\s*\(\s*([A-Za-z0-9_.\-]+)")
SET_CALL = re.compile(r"set\s*\(\s*([A-Za-z0-9_]+)\s+([^)]+)\)", re.DOTALL)


def graph_from_graphviz(build_dir: Path) -> tuple[dict[str, set[str]], set[str]] | None:
    """(edges, known targets) from `cmake --graphviz`, or None if unavailable."""
    if not (build_dir / "CMakeCache.txt").is_file():
        return None
    if shutil.which("cmake") is None:
        return None
    dot_dir = build_dir / ".link_graph"
    dot_dir.mkdir(parents=True, exist_ok=True)
    dot = dot_dir / "graph.dot"
    proc = subprocess.run(
        ["cmake", f"--graphviz={dot}", str(build_dir)],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode != 0 or not dot.is_file():
        print("[linkgraph] cmake --graphviz failed "
              f"(exit {proc.returncode}) — falling back to the files walk")
        return None
    labels: dict[str, str] = {}
    edges: dict[str, set[str]] = {}
    for line in dot.read_text(encoding="utf-8", errors="replace").splitlines():
        m = DOT_NODE.match(line)
        if m:
            labels[m.group(1)] = m.group(2)
            continue
        m = DOT_EDGE.match(line)
        if m and m.group(1) in labels and m.group(2) in labels:
            edges.setdefault(labels[m.group(1)], set()).add(labels[m.group(2)])
    return edges, set(labels.values())


def strip_cmake_comments(text: str) -> str:
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def substitute_simple_vars(text: str) -> str:
    """Inline single-assignment set(VAR token...) values, one directory scope
    (= one file) at a time, so ${TARGET_NAME}-style links
    become visible to the walk. Variables with generator expressions, quotes
    around multiple values, or re-assignment stay unresolved (their calls are
    then skipped, as before)."""
    variables: dict[str, str] = {}
    assigned: set[str] = set()
    for m in SET_CALL.finditer(text):
        name, raw = m.group(1), m.group(2)
        tokens = [t.strip('"') for t in raw.split()]
        if name in assigned:
            variables.pop(name, None)  # re-assigned — ambiguous, drop it
            continue
        assigned.add(name)
        if any("$" in t or '"' in t for t in tokens):
            continue
        variables[name] = " ".join(tokens)
    for name, value in variables.items():
        text = text.replace("${" + name + "}", value)
    return text


def graph_from_files() -> tuple[dict[str, set[str]], set[str]]:
    """(edges, known targets) parsed from the repo's own CMake files."""
    edges: dict[str, set[str]] = {}
    known: set[str] = set()
    files: list[Path] = []
    for root in SCAN_ROOTS:
        path = REPO / root
        if path.is_file():
            files.append(path)
        elif path.is_dir():
            files.extend(path.rglob("CMakeLists.txt"))
            files.extend(path.rglob("*.cmake"))
    for path in files:
        try:
            text = strip_cmake_comments(path.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            continue
        text = substitute_simple_vars(text)
        for m in TARGET_DEF.finditer(text):
            known.add(m.group(1))
        for m in TLL_CALL.finditer(text):
            tokens = m.group(1).split()
            if not tokens:
                continue
            target, deps = tokens[0], tokens[1:]
            if "$" in target or '"' in target:
                continue  # unresolved variable-named target (foreach loops)
            for dep in deps:
                if dep in LINK_KEYWORDS or "$" in dep or '"' in dep:
                    continue
                edges.setdefault(target, set()).add(dep)
    return edges, known


def find_path(edges: dict[str, set[str]], start: str, goals: set[str]) -> list[str] | None:
    """BFS; the first path from start into goals, as a node list."""
    parent: dict[str, str] = {start: start}
    queue = deque([start])
    while queue:
        node = queue.popleft()
        if node in goals and node != start:
            path = [node]
            while path[-1] != start:
                path.append(parent[path[-1]])
            return list(reversed(path))
        for nxt in sorted(edges.get(node, ())):
            if nxt not in parent:
                parent[nxt] = node
                queue.append(nxt)
    return None


def check_sqlite_containment(edges: dict[str, set[str]]) -> list[str]:
    """Every target reaching opennova_sqlite must be the service itself or a
    DIRECT linker of the service (its declared exclusive consumers)."""
    violations: list[str] = []
    for target in sorted(edges):
        if target in (SQLITE_TARGET, SERVICE_TARGET):
            continue
        if SERVICE_TARGET in edges.get(target, ()):
            continue  # a declared service consumer — sqlite rides the service
        path = find_path(edges, target, {SQLITE_TARGET})
        if path:
            violations.append(f"{' -> '.join(path)}  [{SQLITE_RULE_REASON}]")
    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default=str(REPO / "build"),
                        help="configured build tree for the graphviz dump (default: build/)")
    parser.add_argument("--source", choices=("auto", "graphviz", "files"), default="auto",
                        help="graph source (default: graphviz when the build tree exists, else files)")
    parser.add_argument("--enforce", action="store_true",
                        help="violations exit 1 (CI runs this)")
    args = parser.parse_args()

    graph: tuple[dict[str, set[str]], set[str]] | None = None
    source = ""
    if args.source in ("auto", "graphviz"):
        graph = graph_from_graphviz(Path(args.build_dir))
        source = "graphviz"
        if graph is None and args.source == "graphviz":
            print("[linkgraph] no configured build tree for --source graphviz — skipping")
            return 0
    if graph is None:
        graph, source = graph_from_files(), "files"
    edges, known = graph

    violations: list[str] = []
    warned: set[str] = set()
    for reason, from_targets, to_targets, optional in FORBIDDEN:
        to_set = set(to_targets)
        for name in list(from_targets) + sorted(to_set):
            # A renamed/deleted target silently vacates its rule — surface
            # that, except for targets that legitimately live in the other
            # CMake root / behind a build option.
            if name not in known and name not in optional and name not in warned:
                warned.add(name)
                print(f"[linkgraph] WARNING: rule target '{name}' not in the "
                      f"{source} graph (renamed? update link_graph_check.py)")
        for src in from_targets:
            path = find_path(edges, src, to_set)
            if path:
                violations.append(f"{' -> '.join(path)}  [{reason}]")

    if SQLITE_TARGET not in known and SQLITE_TARGET not in warned:
        warned.add(SQLITE_TARGET)
        print(f"[linkgraph] WARNING: rule target '{SQLITE_TARGET}' not in the "
              f"{source} graph (renamed? update link_graph_check.py)")
    if SERVICE_TARGET not in known and SERVICE_TARGET not in warned:
        warned.add(SERVICE_TARGET)
        print(f"[linkgraph] WARNING: rule target '{SERVICE_TARGET}' not in the "
              f"{source} graph (renamed? update link_graph_check.py)")
    violations.extend(check_sqlite_containment(edges))

    edge_count = sum(len(v) for v in edges.values())
    print(f"[linkgraph] source {source}: {len(known)} targets, {edge_count} edges, "
          f"{len(violations)} forbidden path(s)")
    for v in violations:
        print(f"[linkgraph][forbidden] {v}")

    if violations and args.enforce:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
