#!/usr/bin/env python3
"""Maturity-program link-graph forbidden edges (docs/maturity-program.md, LIBS-1).

Asserts that no forbidden (from-target, to-target) pair from
scripts/lint/forbidden_edges.json is connected in the TRANSITIVE link
closure of the CMake target graph. PUBLIC vs PRIVATE keywords are
deliberately not consulted: a STATIC library's PRIVATE deps still
propagate onto every downstream link line (as $<LINK_ONLY> interface
entries), so every target_link_libraries edge is a real closure edge.

Graph source, best available first (override with --source):

  graphviz  `cmake --graphviz` over an existing configured build tree
            (ground truth from CMake itself; default build/, or
            --build-dir). Re-runs configure; needs cmake on PATH.
  files     a transitive walk of target_link_libraries() calls parsed
            from the repo's own CMake files — no build tree needed
            (what the pre-build CI lint step falls back to).

Soft mode (default) always exits 0. --enforce makes violations exit 1
(the umbrella's enforcement table flips this check hard after its soft
wave).
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from collections import deque
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SPEC_PATH = Path(__file__).resolve().parent / "forbidden_edges.json"

# Where the files-mode walk looks for CMake files. Vendored submodules are
# excluded except the two the net stack actually builds (sqlite/bcrypt).
SCAN_ROOTS = (
    "CMakeLists.txt",
    "libs",
    "apps",
    "tests",
    "tools",
    "godot/engine",
    "third_party/sqlite",
    "third_party/bcrypt",
)

LINK_KEYWORDS = {
    "PUBLIC", "PRIVATE", "INTERFACE",
    "LINK_PUBLIC", "LINK_PRIVATE", "LINK_INTERFACE_LIBRARIES",
    "debug", "optimized", "general",
}

DOT_NODE = re.compile(r'^\s*"(node\d+)"\s*\[\s*label\s*=\s*"([^"]+)"')
DOT_EDGE = re.compile(r'^\s*"(node\d+)"\s*->\s*"(node\d+)"')
TLL_CALL = re.compile(r"target_link_libraries\s*\(([^)]*)\)", re.DOTALL)
TARGET_DEF = re.compile(r"add_(?:library|executable)\s*\(\s*([A-Za-z0-9_.\-]+)")


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
        for m in TARGET_DEF.finditer(text):
            known.add(m.group(1))
        for m in TLL_CALL.finditer(text):
            tokens = m.group(1).split()
            if not tokens:
                continue
            target, deps = tokens[0], tokens[1:]
            if "$" in target or '"' in target:
                continue  # variable-named target (none of the spec'd libs are)
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default=str(REPO / "build"),
                        help="configured build tree for the graphviz dump (default: build/)")
    parser.add_argument("--source", choices=("auto", "graphviz", "files"), default="auto",
                        help="graph source (default: graphviz when the build tree exists, else files)")
    parser.add_argument("--enforce", action="store_true",
                        help="violations exit 1 (the post-soft-wave flip)")
    args = parser.parse_args()

    spec = json.loads(SPEC_PATH.read_text(encoding="utf-8"))
    rules = spec.get("forbidden", [])

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
    for rule in rules:
        reason = rule.get("reason", "")
        to_set = set(rule.get("to", []))
        for name in list(rule.get("from", [])) + sorted(to_set):
            # A renamed/deleted target silently vacates its rule — surface that.
            if name not in known and name not in warned:
                warned.add(name)
                print(f"[linkgraph] WARNING: spec target '{name}' not in the "
                      f"{source} graph (renamed? update forbidden_edges.json)")
        for src in rule.get("from", []):
            path = find_path(edges, src, to_set)
            if path:
                violations.append(f"{' -> '.join(path)}  [{reason}]")

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
