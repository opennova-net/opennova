#!/usr/bin/env python3
"""Formatting and lint checks scoped to CHANGED lines (advisory by default).

Each language's tool runs over the files the diff touches, and only findings
on lines the diff adds or rewrites are reported, so no check asks for a
repository-wide reformat (the tree is not formatted yet; ADR 0043 d14):

  C++      (.h .hpp .cpp .cc .inl)  clang-format --dry-run against .clang-format
  Python   (.py)                    ruff check + ruff format --diff (ruff.toml)
  GDScript (.gd)                    gdlint (gdlintrc) + gdformat --diff

Range: --base wins; else the merge-base with origin/master; else HEAD~1; else
the check skips cleanly. The diff runs from that base to the WORKING TREE, so
a local run covers uncommitted edits too.

Advisory (default): findings print (as ::warning:: annotations with --github),
a missing tool is noted and skipped, and the exit is 0. --enforce exits 1 on
any finding or missing tool.

Local run, with the versions CI pins:

  pip install clang-format==21.1.6 ruff==0.16.9 gdtoolkit==4.5.0
  python scripts/lint/format_lint.py
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

CPP_EXTS = (".h", ".hpp", ".cpp", ".cc", ".inl")
# Vendored and bootstrapped trees are never ours to format (.clang-format-ignore
# says the same for clang-format); docs/ is a submodule, so only its pointer diffs.
EXCLUDED_PREFIXES = ("third_party/", "godot/addons/", "build/", "docs/")
# Windows caps a command line near 32k characters: run the tools in batches.
BATCH = 50

ADDED_HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")
FORMAT_HUNK = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+\d+(?:,\d+)? @@")
CLANG_FORMAT_LINE = re.compile(r"^(.+?):(\d+):\d+: warning: (.+?) \[-Wclang-format-violations\]$")
RUFF_CHECK_LINE = re.compile(r"^(.+?):(\d+):\d+: (.+)$")
GDLINT_LINE = re.compile(r"^(.+?):(\d+): Error: (.+)$")

Finding = tuple[str, int, str, str]  # (path, line, tool, message)


def git(*args: str) -> str:
    return subprocess.run(
        ["git", *args], cwd=REPO, check=True, capture_output=True, text=True, encoding="utf-8"
    ).stdout


def resolve_base(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    for probe in (
        ("merge-base", "origin/master", "HEAD"),
        ("rev-parse", "--verify", "--quiet", "HEAD~1"),
    ):
        try:
            return git(*probe).strip() or None
        except subprocess.CalledProcessError:
            continue
    return None


def changed_lines(base: str) -> dict[str, set[int]]:
    """Every in-scope file the diff touches, with the line numbers it adds."""
    out: dict[str, set[int]] = {}
    path = ""
    for raw in git(
        "diff", "-U0", "--no-color", "--no-ext-diff", "--diff-filter=AMR", base
    ).splitlines():
        if raw.startswith("+++ "):
            path = raw[6:] if raw.startswith("+++ b/") else ""
            if path.startswith(EXCLUDED_PREFIXES):
                path = ""
            continue
        m = ADDED_HUNK.match(raw)
        if m and path:
            start, count = int(m.group(1)), int(m.group(2) or "1")
            out.setdefault(path, set()).update(range(start, start + count))
    return {p: lines for p, lines in out.items() if lines}


def tool(name: str) -> str | None:
    found = shutil.which(name)
    if found:
        return found
    # A venv's console scripts sit beside its interpreter.
    for candidate in (
        Path(sys.executable).parent / name,
        Path(sys.executable).parent / (name + ".exe"),
    ):
        if candidate.exists():
            return str(candidate)
    return None


def run(cmd: list[str]) -> str:
    done = subprocess.run(
        cmd, cwd=REPO, capture_output=True, text=True, encoding="utf-8", errors="replace"
    )
    return done.stdout + done.stderr


def batches(paths: list[str]):
    for i in range(0, len(paths), BATCH):
        yield paths[i : i + BATCH]


def norm(path: str) -> str:
    return path.replace("\\", "/").removeprefix("./")


def per_line(output: str, pattern: re.Pattern[str], tool_name: str) -> list[Finding]:
    found: list[Finding] = []
    for raw in output.splitlines():
        m = pattern.match(raw.strip())
        if m:
            found.append((norm(m.group(1)), int(m.group(2)), tool_name, m.group(3).strip()))
    return found


def format_diff_hunks(output: str, tool_name: str) -> list[Finding]:
    """One finding per formatter hunk, at each original line the hunk rewrites
    (an insertion lands on the line it goes before)."""
    found: list[Finding] = []
    path, orig = "", 0
    for raw in output.splitlines():
        if raw.startswith("--- "):
            path = norm(raw[4:].split("\t")[0].strip())
            continue
        if raw.startswith("+++ "):
            continue
        m = FORMAT_HUNK.match(raw)
        if m:
            orig = int(m.group(1))
            continue
        if not path or not raw:
            continue
        if raw[0] == " ":
            orig += 1
        elif raw[0] == "-":
            found.append((path, orig, tool_name, "would reformat these lines"))
            orig += 1
        elif raw[0] == "+":
            found.append((path, orig, tool_name, "would reformat these lines"))
    return found


def check_cpp(files: list[str], missing: list[str]) -> list[Finding]:
    exe = tool("clang-format")
    if exe is None:
        missing.append("clang-format")
        return []
    found: list[Finding] = []
    for chunk in batches(files):
        found += per_line(
            run([exe, "--dry-run", "--style=file", *chunk]), CLANG_FORMAT_LINE, "clang-format"
        )
    return found


def check_python(files: list[str], missing: list[str]) -> list[Finding]:
    exe = tool("ruff")
    if exe is None:
        missing.append("ruff")
        return []
    found: list[Finding] = []
    for chunk in batches(files):
        found += per_line(
            run([exe, "check", "--no-fix", "--output-format", "concise", *chunk]),
            RUFF_CHECK_LINE,
            "ruff",
        )
        found += format_diff_hunks(run([exe, "format", "--diff", *chunk]), "ruff format")
    return found


def check_gdscript(files: list[str], missing: list[str]) -> list[Finding]:
    found: list[Finding] = []
    lint, fmt = tool("gdlint"), tool("gdformat")
    if lint is None or fmt is None:
        missing.append("gdtoolkit (gdlint, gdformat)")
        return []
    for chunk in batches(files):
        found += per_line(run([lint, *chunk]), GDLINT_LINE, "gdlint")
        found += format_diff_hunks(run([fmt, "--diff", *chunk]), "gdformat")
    return found


def coalesce(findings: list[Finding]) -> list[tuple[str, int, int, str, str]]:
    """Runs of consecutive lines with the same tool and message become one range."""
    runs: list[tuple[str, int, int, str, str]] = []
    for path, line, tool_name, message in sorted(findings, key=lambda f: (f[0], f[2], f[3], f[1])):
        if runs:
            r_path, r_first, r_last, r_tool, r_message = runs[-1]
            if (r_path, r_tool, r_message) == (path, tool_name, message) and line == r_last + 1:
                runs[-1] = (path, r_first, line, tool_name, message)
                continue
        runs.append((path, line, line, tool_name, message))
    return sorted(runs, key=lambda r: (r[0], r[1], r[3]))


def report(findings: list[Finding], annotate: bool) -> int:
    runs = coalesce(findings)
    for path, first, last, tool_name, message in runs:
        where = str(first) if first == last else f"{first}-{last}"
        if annotate:
            print(
                f"::warning file={path},line={first},endLine={last},"
                f"title=format_lint {tool_name}::{message}"
            )
        else:
            print(f"{path}:{where}: [{tool_name}] {message}")
    return len(runs)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", help="diff base (default: merge-base with origin/master)")
    ap.add_argument("--enforce", action="store_true", help="exit 1 on any finding or missing tool")
    ap.add_argument("--github", action="store_true", help="print GitHub ::warning:: annotations")
    args = ap.parse_args()

    base = resolve_base(args.base)
    if base is None:
        print("format_lint: no diff base (shallow clone?); skipping")
        return 0
    touched = changed_lines(base)
    by_lang = {
        "cpp": sorted(p for p in touched if p.endswith(CPP_EXTS)),
        "py": sorted(p for p in touched if p.endswith(".py")),
        "gd": sorted(p for p in touched if p.endswith(".gd")),
    }
    missing: list[str] = []
    findings: list[Finding] = []
    if by_lang["cpp"]:
        findings += check_cpp(by_lang["cpp"], missing)
    if by_lang["py"]:
        findings += check_python(by_lang["py"], missing)
    if by_lang["gd"]:
        findings += check_gdscript(by_lang["gd"], missing)

    # Keep what lands on a changed line, once per (path, line, tool).
    seen: set[tuple[str, int, str]] = set()
    kept: list[Finding] = []
    for f in findings:
        key = (f[0], f[1], f[2])
        if f[1] in touched.get(f[0], ()) and key not in seen:
            seen.add(key)
            kept.append(f)
    runs = report(kept, args.github)

    files = sum(len(v) for v in by_lang.values())
    for name in missing:
        print(f"format_lint: {name} not found; its files were not checked")
    print(
        f"format_lint: {runs} finding(s) over {len(kept)} changed line(s) in {files} file(s)"
        f" since {base[:12]}" + ("" if args.enforce else " (advisory)")
    )
    if args.enforce and (kept or missing):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
