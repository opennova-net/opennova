#!/usr/bin/env python3
"""Vocabulary conventions the other gates do not see (CLAUDE.md, ADR 0040).

Three rules, each a tree-wide regex the tree is clean against today, kept so a
regression fails CI instead of waiting for the next hygiene round:

  nova-prefix   no `nova_` / `Nova<Upper>` prefix on a file name or an
                identifier in the code trees (ADR 0040 d1: files, identifiers,
                class_names, header guards). The proper nouns survive:
                NovaWorld*, NovaLogic, opennova/OpenNova, novacrypto, and an
                external project's own path spelled in a comment (onnet's
                onnw/controllers/nova_world/).
  nova-macro    (advisory, never fails) the `NOVA_` macro/constant form of the
                same prefix: the shader constants and the render-fixture
                contract keys still carry it, pinned by validation JSON and
                provenance tests, so the count is reported for the burn-down
                and enforced once it reaches zero.
  em-dash       no U+2014 in public-facing copy: README.md, GOALS.md,
                launcher/README.md, and the user-visible strings of the web
                portal (web/src template text and string literals; comments
                and a bare em-dash placeholder glyph do not count).
  checked-box   no `- [x]` / `* [x]` row in a tracked Markdown file: a
                completed entry is deleted, not ticked (the self-declared
                historical records are exempt: plan/**, docs/maturity-program.md,
                docs/adr/**).

Modes:
  (default)     summary counts; exit 0 (report mode)
  --report      summary plus every hit
  --enforce     exit 1 on any hit
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

CODE_PREFIXES = ("engine/", "godot/", "apps/", "tests/", "scripts/")
CODE_SUFFIXES = (".cpp", ".cc", ".c", ".h", ".hpp", ".gd", ".tscn", ".tres", ".gdshader",
                 ".gdshaderinc", ".py", ".ps1", ".sh", ".cmake", ".txt", ".json", ".cfg")
HARD_EXCLUDES = ("third_party/", "godot/addons/", "/build/", "scripts/lint/conventions_lint.py")

# The prefix forms ADR 0040 names, at an identifier boundary; the macro form
# is counted separately (advisory) until the shader constants are renamed.
NOVA_PREFIX = re.compile(r"(?<![A-Za-z0-9_])(?:nova_[A-Za-z0-9]|Nova[A-Z])")
NOVA_MACRO = re.compile(r"(?<![A-Za-z0-9_])NOVA_[A-Z]")
# Every survivor: the proper nouns, and the external project's path.
NOVA_OK = re.compile(r"NovaWorld|NOVAWORLD|Novaworld|NovaLogic|NOVALOGIC|opennova|OpenNova|OPENNOVA"
                     r"|novacrypto|NovaCrypto|onnw/controllers/nova_world")

PUBLIC_COPY = ("README.md", "GOALS.md", "launcher/README.md")
WEB_PREFIX = "web/src/"
WEB_SUFFIXES = (".vue", ".ts")
EM_DASH = "—"
# A comment line (script, template or style) carries no user-visible text.
COMMENT_LINE = re.compile(r"^\s*(//|/\*|\*|<!--|#)")
# `'—'` / `"—"` alone is a placeholder glyph for an empty cell, not copy.
PLACEHOLDER_GLYPH = re.compile(r"""(['"])—\1""")

CHECKED_BOX = re.compile(r"^\s*[-*]\s+\[[xX]\]")
HISTORICAL_RECORDS = ("plan/", "docs/maturity-program.md", "docs/adr/")


def tracked() -> list[str]:
    out = subprocess.run(["git", "ls-files"], cwd=REPO, check=True, capture_output=True,
                         text=True, encoding="utf-8").stdout
    return [line for line in out.splitlines() if line]


def read(rel: str) -> str:
    try:
        return (REPO / rel).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def check_nova_prefix(files: list[str]) -> tuple[list[str], list[str]]:
    """(enforced identifier/file hits, advisory NOVA_ macro hits)."""
    hits: list[str] = []
    macros: list[str] = []
    for rel in files:
        if not rel.startswith(CODE_PREFIXES) or any(x in rel for x in HARD_EXCLUDES):
            continue
        base = rel.rsplit("/", 1)[-1]
        if base.startswith("nova_") and not base.startswith("novaworld"):
            hits.append(f"{rel}: file name carries the nova_ prefix")
        if not rel.endswith(CODE_SUFFIXES):
            continue
        for lineno, line in enumerate(read(rel).splitlines(), 1):
            for pattern, bucket in ((NOVA_PREFIX, hits), (NOVA_MACRO, macros)):
                for match in pattern.finditer(line):
                    window = line[max(0, match.start() - 24):match.start() + 32]
                    if NOVA_OK.search(window):
                        continue
                    bucket.append(f"{rel}:{lineno}: {line.strip()[:100]}")
                    break
    return hits, macros


def check_em_dash(files: list[str]) -> list[str]:
    hits: list[str] = []
    for rel in files:
        public = rel in PUBLIC_COPY
        web = rel.startswith(WEB_PREFIX) and rel.endswith(WEB_SUFFIXES)
        if not (public or web):
            continue
        in_block = False
        for lineno, line in enumerate(read(rel).splitlines(), 1):
            if web:
                # Block comments (<!-- --> in templates, /* */ in script) span
                # lines; a line inside one is not copy.
                was_in_block = in_block
                if "<!--" in line or "/*" in line:
                    in_block = True
                if "-->" in line or "*/" in line:
                    in_block = False
                if was_in_block or "<!--" in line or "/*" in line:
                    continue
            if EM_DASH not in line:
                continue
            if web and (COMMENT_LINE.match(line) or PLACEHOLDER_GLYPH.sub("", line).find(EM_DASH) < 0):
                continue
            hits.append(f"{rel}:{lineno}: {line.strip()[:100]}")
    return hits


def check_checked_box(files: list[str]) -> list[str]:
    hits: list[str] = []
    for rel in files:
        if not rel.endswith(".md") or rel.startswith(HISTORICAL_RECORDS) or "third_party/" in rel:
            continue
        for lineno, line in enumerate(read(rel).splitlines(), 1):
            if CHECKED_BOX.match(line):
                hits.append(f"{rel}:{lineno}: {line.strip()[:100]}")
    return hits


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--report", action="store_true", help="list every hit")
    parser.add_argument("--enforce", action="store_true", help="exit 1 on any hit")
    args = parser.parse_args()

    files = tracked()
    nova_hits, nova_macros = check_nova_prefix(files)
    results = {
        "nova-prefix": nova_hits,
        "em-dash": check_em_dash(files),
        "checked-box": check_checked_box(files),
    }
    total = sum(len(v) for v in results.values())
    print(f"[conventions] {len(files)} tracked file(s): "
          + ", ".join(f"{len(v)} {k}" for k, v in results.items())
          + f"; {len(nova_macros)} nova-macro (advisory)")
    if args.report or (args.enforce and total):
        for rule, hits in results.items():
            for hit in hits:
                print(f"[conventions][{rule}] {hit}")
    if args.report:
        for hit in nova_macros:
            print(f"[conventions][nova-macro][advisory] {hit}")
    if total and args.enforce:
        print("[conventions] FAIL: no Nova/nova_ prefix (ADR 0040), no em dashes in public copy, "
              "and no checked-off checklist rows (CLAUDE.md).")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
