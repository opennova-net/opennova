#!/usr/bin/env python3
"""Environment-variable lint: the runtime, tools and tests read a closed set.

docs/dev-env-vars.md is the registry. After the 2026-08 cut the whole set is:

  the four machine roots  OPENNOVA_JO_DIR, OPENNOVA_JO_ASSETS,
                          OPENNOVA_MISSION_CORPUS, OPENNOVA_CAPTURES - read ONLY
                          by the three resolvers (tests/common/retail_paths.h,
                          godot/tests/support/retail_data.gd, scripts/net/lib.ps1)
  GODOT_BIN               the scripts' Godot binary
  the deployed service    ONNET_*, DATABASE_PATH, ADMIN_*, ONLAUNCHER_*, ... under
                          apps/novaworld_server, deploy, launcher, web, backend, infra
  OS variables            TEMP/TMP/APPDATA/HOME/... at their known sites

Everything else was a launch flag, an argv option, an MCP tool argument, or
deleted. This lint scans tracked code for environment reads in every
language the tree carries and buckets each hit against
scripts/lint/env_allowlist.json:

  gates        {NAME: [file globs]}  the roots and where they may be read
  files        {glob: [NAME|*]}      per-file allowances (GODOT_BIN in scripts,
                                     the OS variables, the service family '*')
  indirect_ok  [file globs]          files whose helper reads a name passed in
                                     (the resolvers); everywhere else a
                                     non-literal read is a hit

Modes: (default) summary; --report lists every uncovered hit; --enforce exits
1 on any uncovered hit.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ALLOWLIST_PATH = Path(__file__).resolve().parent / "env_allowlist.json"
SCAN_PREFIXES = ("engine/", "godot/", "apps/", "tests/", "scripts/", "web/src/",
                 "launcher/", "backend/", "deploy/", "infra/")
HARD_EXCLUDES = ("third_party/", "godot/addons/", "/build/", "scripts/lint/env_lint.py",
                 "scripts/lint/env_allowlist.json")
CODE_SUFFIXES = (".cpp", ".cc", ".c", ".h", ".hpp", ".gd", ".py", ".ps1", ".sh", ".ts",
                 ".vue", ".js", ".cs", ".psm1")

# (regex, group of the literal name or None when the pattern is a non-literal read)
PATTERNS = {
    "cpp": [
        (re.compile(r"\bgetenv\s*\(\s*\"([A-Za-z0-9_]+)\""), 1),
        (re.compile(r"\bgetenv\s*\(\s*(?!\")"), None),
    ],
    "gd": [
        (re.compile(r"OS\.(?:get|has|set|unset)_environment\(\s*\"([A-Za-z0-9_]+)\""), 1),
        (re.compile(r"OS\.(?:get|has|set|unset)_environment\(\s*(?!\")"), None),
    ],
    "ps1": [
        (re.compile(r"\$env:([A-Za-z0-9_]+)"), 1),
        (re.compile(r"\[Environment\]::(?:Get|Set)EnvironmentVariable\(\s*[\"']([A-Za-z0-9_]+)[\"']"), 1),
        (re.compile(r"\[Environment\]::(?:Get|Set)EnvironmentVariable\(\s*(?![\"'])"), None),
        (re.compile(r"Get-ChildItem\s+Env:"), None),
        (re.compile(r"Env:\$"), None),
    ],
    "sh": [
        (re.compile(r"\$\{([A-Z][A-Z0-9_]*)(?::-|:=|:\?|-|\})"), 1),
        (re.compile(r"\bexport\s+([A-Z][A-Z0-9_]*)="), 1),
    ],
    "py": [
        (re.compile(r"os\.environ(?:\.get)?\s*[\(\[]\s*[\"']([A-Za-z0-9_]+)[\"']"), 1),
        (re.compile(r"os\.getenv\s*\(\s*[\"']([A-Za-z0-9_]+)[\"']"), 1),
        (re.compile(r"os\.environ(?:\.get)?\s*[\(\[]\s*(?![\"'])"), None),
        (re.compile(r"os\.getenv\s*\(\s*(?![\"'])"), None),
    ],
    "js": [
        (re.compile(r"process\.env\.([A-Za-z0-9_]+)"), 1),
        (re.compile(r"import\.meta\.env\.([A-Za-z0-9_]+)"), 1),
    ],
}
LANG_BY_SUFFIX = {".cpp": "cpp", ".cc": "cpp", ".c": "cpp", ".h": "cpp", ".hpp": "cpp",
                  ".gd": "gd", ".py": "py", ".ps1": "ps1", ".psm1": "ps1", ".sh": "sh",
                  ".ts": "js", ".vue": "js", ".js": "js", ".cs": "cpp"}
# Shell variables the sh patterns match but that are the script's own locals
# (assigned in the same file) are not environment reads.
SH_LOCAL = re.compile(r"^\s*(?:local\s+)?([A-Z][A-Z0-9_]*)=", re.M)


def tracked_code_files() -> list[str]:
    out = subprocess.run(["git", "ls-files", "--", *SCAN_PREFIXES], cwd=REPO, check=True,
                         capture_output=True, text=True, encoding="utf-8").stdout
    files = []
    for rel in out.splitlines():
        if not rel or any(x in rel for x in HARD_EXCLUDES):
            continue
        if rel.endswith(CODE_SUFFIXES):
            files.append(rel)
    return files


class Allowlist:
    def __init__(self, config: dict):
        self.gates: dict[str, list[str]] = config.get("gates", {})
        self.files: dict[str, list[str]] = config.get("files", {})
        self.indirect_ok: list[str] = config.get("indirect_ok", [])

    def bucket(self, rel: str, name: str | None) -> str | None:
        if name is None:
            return "indirect" if any(fnmatch.fnmatch(rel, g) for g in self.indirect_ok) else None
        if name in self.gates:
            return f"gate:{name}" if any(fnmatch.fnmatch(rel, g) for g in self.gates[name]) else None
        for glob, names in self.files.items():
            if fnmatch.fnmatch(rel, glob) and ("*" in names or name in names):
                return "service" if "*" in names else f"file:{name}"
        return None


def scan(allow: Allowlist):
    for rel in tracked_code_files():
        lang = LANG_BY_SUFFIX.get(Path(rel).suffix)
        if lang is None:
            continue
        try:
            text = (REPO / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        sh_locals = set(SH_LOCAL.findall(text)) if lang == "sh" else set()
        for lineno, line in enumerate(text.splitlines(), 1):
            stripped = line.lstrip()
            if lang in ("cpp", "js") and stripped.startswith("//"):
                continue
            if lang in ("gd", "py", "sh", "ps1") and stripped.startswith("#"):
                continue
            for pattern, group in PATTERNS[lang]:
                for match in pattern.finditer(line):
                    name = match.group(group) if group else None
                    if lang == "sh" and name in sh_locals:
                        continue
                    bucket = allow.bucket(rel, name)
                    yield (bucket or "UNCOVERED", rel, lineno, name or "<indirect>", line)


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--report", action="store_true", help="list every uncovered hit")
    parser.add_argument("--enforce", action="store_true", help="exit 1 on any uncovered hit")
    args = parser.parse_args()

    allow = Allowlist(json.loads(ALLOWLIST_PATH.read_text(encoding="utf-8")))
    buckets: Counter = Counter()
    names: Counter = Counter()
    uncovered: dict[str, list[tuple[int, str, str]]] = {}
    for bucket, rel, lineno, name, line in scan(allow):
        buckets[bucket] += 1
        if bucket == "UNCOVERED":
            uncovered.setdefault(rel, []).append((lineno, name, line.strip()))
            names[name] += 1
    covered = sum(v for k, v in buckets.items() if k != "UNCOVERED")
    print(f"[env-lint] {covered} covered read(s), {buckets['UNCOVERED']} uncovered read(s) "
          f"in {len(uncovered)} file(s)")
    for bucket, count in sorted(buckets.items(), key=lambda kv: -kv[1]):
        if bucket != "UNCOVERED":
            print(f"[env-lint]   {count:5d}  {bucket}")
    if args.report or (args.enforce and buckets["UNCOVERED"]):
        for rel in sorted(uncovered):
            print(f"[env-lint][uncovered] {rel} ({len(uncovered[rel])})")
            for lineno, name, line in uncovered[rel][:8]:
                print(f"[env-lint]   :{lineno}: {name}: {line[:110]}")
            if len(uncovered[rel]) > 8:
                print(f"[env-lint]   ... and {len(uncovered[rel]) - 8} more")
        if names:
            print("[env-lint] uncovered names: " + ", ".join(f"{n} x{c}" for n, c in names.most_common()))
    if buckets["UNCOVERED"] and args.enforce:
        print("[env-lint] FAIL: only the four machine roots (through their resolvers), GODOT_BIN "
              "(scripts), the deployed service's config and the listed OS variables may be read "
              "from the environment; everything else is a launch flag, an argv option or an MCP "
              "argument (docs/dev-env-vars.md).")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
