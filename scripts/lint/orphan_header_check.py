#!/usr/bin/env python3
"""Orphan engine headers (docs/maturity-program.md, STD-1): an `engine/**`
header that no source outside `tests/` (and outside its own translation unit)
includes is dead policy code with a passing test — the pattern the #554 and
#576 post-merge rounds found in eleven and four headers respectively. Such a
header may exist only while it is explicitly STAGED: its leading comment must
carry the literal `STAGED, NOT WIRED` marker naming the live owner that will
consume it.

A header is INCLUDED by a file when an `#include "..."` / `#include <...>` line
in that file names a path whose trailing components equal the header's
repo-relative path suffix (`world/foo.h` matches `engine/runtime/world/foo.h`;
a bare `"foo.h"` matches a header anywhere inside the including file's own
lib, `engine/<group>/<lib>/`, which is what the per-lib include dirs
resolve). Two headers that share a suffix both count as included (the check
never reports a false orphan on an ambiguous include).

Consumers scanned: engine/, godot/src/, apps/, and the header's own
directory. `tests/` is deliberately NOT a
consumer: a header included only by its test is exactly the orphan shape.

The pre-existing orphans found at the 2026-08-25 census are listed with their
reason in orphan_header_allowlist.json (the flat C ABI headers that used to sit
there went with the FFI, ADR 0038). Burn that list down; never grow it
without a reason that names the consumer.

Modes:
  (default)   report orphans; exit 0 (soft)
  --enforce   exit 1 on any orphan that is neither allowlisted nor STAGED
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ALLOWLIST_PATH = Path(__file__).resolve().parent / "orphan_header_allowlist.json"

HEADER_ROOTS = ("engine",)
CONSUMER_ROOTS = ("engine", "godot/src", "apps")
SOURCE_SUFFIXES = (".h", ".hpp", ".c", ".cc", ".cpp", ".inl")
HEADER_SUFFIXES = (".h", ".hpp")
SKIP_PARTS = {"third_party", "build", "tools"}
STAGED_MARKER = "STAGED, NOT WIRED"

INCLUDE_LINE = re.compile(r'^\s*#\s*include\s*[<"]([^<>"]+)[>"]')


def lib_root(rel: Path) -> tuple[str, ...]:
    """`engine/<group>/<lib>` for an engine path (the per-lib include dir)."""
    parts = rel.parts
    if len(parts) >= 3 and parts[0] == "engine":
        return parts[:3]
    return parts[:2]


def load_allowlist() -> dict[str, str]:
    try:
        data = json.loads(ALLOWLIST_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return {str(k): str(v) for k, v in data.get("allow", {}).items()}


def _skip(path: Path) -> bool:
    return any(part in SKIP_PARTS or part.startswith("build") for part in path.parts)


def source_files(roots: tuple[str, ...], suffixes: tuple[str, ...]) -> list[Path]:
    files: list[Path] = []
    for root in roots:
        base = REPO / root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix.lower() not in suffixes:
                continue
            rel = path.relative_to(REPO)
            if _skip(rel):
                continue
            files.append(rel)
    return files


def includes_of(rel: Path) -> list[str]:
    try:
        text = (REPO / rel).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    out: list[str] = []
    for line in text.splitlines():
        m = INCLUDE_LINE.match(line)
        if m:
            inc = m.group(1).replace("\\", "/")
            # A relative include (`../wire_cursor.h`) names the header by its
            # tail; the leading dots carry no path information for the join.
            parts = [p for p in inc.split("/") if p not in ("", ".", "..")]
            out.append("/".join(parts))
    return out


def header_is_staged(rel: Path) -> bool:
    try:
        head = (REPO / rel).read_text(encoding="utf-8", errors="replace")[:4000]
    except OSError:
        return False
    return STAGED_MARKER in head


def own_unit(header: Path) -> set[Path]:
    """The header's own translation unit(s): same directory, same stem."""
    units: set[Path] = set()
    for suffix in (".cpp", ".cc", ".c"):
        units.add(header.with_suffix(suffix))
    return units


def find_orphans() -> tuple[list[Path], list[Path]]:
    headers = source_files(HEADER_ROOTS, HEADER_SUFFIXES)
    consumers = source_files(CONSUMER_ROOTS, SOURCE_SUFFIXES)
    # Index every include line once: (consumer, include path components).
    include_index: list[tuple[Path, tuple[str, ...]]] = []
    for consumer in consumers:
        for inc in includes_of(consumer):
            include_index.append((consumer, tuple(inc.split("/"))))
    orphans: list[Path] = []
    staged: list[Path] = []
    for header in headers:
        parts = header.parts
        own = own_unit(header)
        included = False
        for consumer, inc_parts in include_index:
            if consumer == header or consumer in own:
                continue
            n = len(inc_parts)
            if n == 0 or n > len(parts):
                continue
            if parts[-n:] != inc_parts:
                continue
            # A bare "foo.h" include counts from the header's own lib (the
            # per-lib include dir) or its own directory.
            if n == 1 and consumer.parent != header.parent and \
                    lib_root(consumer) != lib_root(header):
                continue
            included = True
            break
        if included:
            continue
        if header_is_staged(header):
            staged.append(header)
        else:
            orphans.append(header)
    return orphans, staged


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--enforce", action="store_true",
                        help="exit 1 on any orphan header that is neither allowlisted nor STAGED")
    args = parser.parse_args()
    orphans, staged = find_orphans()
    allow = load_allowlist()
    for path in staged:
        print(f"[orphan-header] STAGED (allowed): {path.as_posix()}")
    failing: list[Path] = []
    for path in orphans:
        key = path.as_posix()
        if key in allow:
            print(f"[orphan-header] allowlisted: {key} ({allow[key]})")
            continue
        failing.append(path)
        print(f"[orphan-header] ORPHAN: {key} is included by nothing outside tests/ "
              f"and its own unit; wire it or open it with a '{STAGED_MARKER}' paragraph "
              "naming the live owner")
    stale = sorted(set(allow) - {p.as_posix() for p in orphans})
    for key in stale:
        print(f"[orphan-header] allowlist entry no longer an orphan (delete it): {key}")
    print(f"[orphan-header] {len(failing)} orphan(s), {len(staged)} staged, "
          f"{len(orphans) - len(failing)} allowlisted")
    if (failing or stale) and args.enforce:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
