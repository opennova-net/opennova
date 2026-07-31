#!/usr/bin/env python3
"""Host-terminology lint: "host" means the game/server host, nothing else.

CONTEXT.md reserves "host" for the authoritative side of an in-match session
(listen server, dedicated host, NovaWorld host registration). Every other
project-owned sense was renamed by the 2026-07 terminology campaign:
UI attach-points are Mounts, presentation owners are Presenters, application
front-ends are Shells, a lib's embedding application is the embedder.

This lint keeps the word from regressing. It scans tracked code files for
"host" tokens (word-unit or CamelCase-boundary; ghost/hostile/hostname and
similar substrings never match) and buckets each hit against
scripts/lint/host_allowlist.json:

  path_globs        files that live entirely in the game-host domain (net
                    runtime, NovaWorld service, wire fixtures) or carry the
                    word for an external reason (the Windows hosts file).
  identifiers       exact game-host API tokens legal anywhere
                    (HostSessionConfig, is_host_listening, host_ip, ...).
  comment_ok_paths  files whose comments legitimately discuss the game host;
                    identifiers on those lines are still checked.
  line_allow        "path-substring|line-substring" escape hatch for
                    player-facing copy ("Host a Game") and similar one-offs.

Modes:
  (default)         summary counts per bucket; exit 0 (CI report mode)
  --report          summary plus every uncovered hit, grouped by file
  --enforce         exit 1 if any hit is uncovered (the post-campaign gate)
  --frozen-audit    diff guard for rename phases (local tool, not CI): fails
                    if the range touches wire/retail-frozen scopes, drops a
                    frozen wire string, or loses an [orig:] citation or
                    D-XXX-n divergence ID from docs. --range as in
                    maturity_lint.py (default origin/master...HEAD).

A newly ADDED doc line using bare "host" outside the net records is reported
as an advisory (never fails) — prose enforcement is fuzzy by design.
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
ALLOWLIST_PATH = Path(__file__).resolve().parent / "host_allowlist.json"

CODE_SUFFIXES = (
    ".gd", ".tscn", ".tres", ".gdshader", ".cpp", ".cc", ".c", ".h", ".hpp",
    ".py", ".ps1", ".sh", ".ts", ".vue", ".cs", ".sql", ".toml", ".cfg",
)
SCAN_PREFIXES = (
    "godot/", "libs/", "apps/", "scripts/", "web/src/", "pyopennova/",
    "tests/", "launcher/", "backend/", "blender/", "opennova_max/",
    "opennova_qt_ui/", "opennova_jobs/", "opennova_blender/", "release/",
)
HARD_EXCLUDES = ("third_party/", "/build/", "scripts/lint/host_lint.py",
                 "scripts/lint/host_allowlist.json")

# Three casings of a host token. Substring traps (ghost, hostile, hostname,
# localhost) are killed by the lookarounds: a lowercase letter before kills
# the snake/CamelCase forms, a lowercase letter after kills all three.
HOST_TOKEN = re.compile(
    r"(?<![A-Za-z])host(?:s|ed|ing)?(?![a-z])"      # host, _host, host_x, hosted
    r"|(?<![A-Z])Host(?:s|ed|ing)?(?![a-z])"        # Host, MenuHost, HostOwner
    r"|(?<![A-Za-z])HOST(?:S|ED|ING)?(?![A-Z]?[a-z])")  # HOST, NW_LAN_HOST

# Standard vocabulary that is not project terminology at all; a line carrying
# one of these is skipped wholesale (the plural-meaning risk on such lines is
# accepted — they are address/config lines, not naming surface).
GENERIC_LINE = re.compile(
    r"hostname|localhost|Write-Host|DOCKER_HOST|ONNET_PUBLIC_HOST"
    r"|connect_to_host|disconnect_from_host|StrictHostKeyChecking"
    r"|resolve_hostname|\[orig:")

IDENT_CHARS = re.compile(r"[A-Za-z0-9_]")

COMMENT_MARKERS = {
    ".gd": "#", ".py": "#", ".sh": "#", ".ps1": "#", ".toml": "#",
    ".cfg": "#", ".cpp": "//", ".cc": "//", ".c": "//", ".h": "//",
    ".hpp": "//", ".cs": "//", ".ts": "//", ".gdshader": "//", ".sql": "--",
}

# Wire/retail-frozen strings: a rename phase must never make one of these
# disappear from the tree. The audit balances removed vs added diff lines —
# a token removed more times than it is re-added means a frozen name changed.
FROZEN_TOKENS = (
    "ClientHostRequest", "ClientHostUpdate", "ClientStopHosting",
    "ClientHostPlayerAdded", "ClientHostPlayerRemoved", "ServerHostResult",
    "HostAcceptEvent", "HostSessionAccept", "\"HostSetup\"", "_var_list(\"Host\"",
    "MULTI_PLAYER_HOST", "HG_SERVEPLAY", "HG_SERVEONLY", "HOST_URL",
    "HOSTKEY", "hostIp", "hostPort", "/api/hosts", "active_hosts",
    "host_players",
)
FROZEN_PATHS = ("fixtures/", "backend/migrations/",
                "apps/novaworld_server/templates/", "libs/napi/",
                "third_party/")

CITATION = re.compile(r"\[orig:[^\]]*\]")
DIVERGENCE_ID = re.compile(r"\bD-[A-Z]+-\d+\b")

# Docs where bare "host" is the domain vocabulary (game host); added lines
# elsewhere in docs get the advisory.
DOC_HOST_OK = re.compile(
    r"^(docs/net/|docs/adr/000[9]-|docs/adr/001[0-3]-|\.agents/|plan/"
    r"|libs/npruntime/)")


def run_git(*args: str) -> str:
    return subprocess.run(
        ["git", *args], cwd=REPO, check=True,
        capture_output=True, text=True, encoding="utf-8").stdout


def resolve_range(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    for candidate in ("origin/master...HEAD", "HEAD~1..HEAD"):
        probe = candidate.split("...")[0].split("..")[0]
        try:
            subprocess.run(["git", "rev-parse", "--verify", "--quiet", probe],
                           cwd=REPO, check=True, capture_output=True)
            return candidate
        except subprocess.CalledProcessError:
            continue
    return None


def tracked_code_files() -> list[str]:
    files = []
    for rel in run_git("ls-files").splitlines():
        if not rel.startswith(SCAN_PREFIXES):
            continue
        if any(marker in rel for marker in HARD_EXCLUDES):
            continue
        if rel.endswith(CODE_SUFFIXES):
            files.append(rel)
    return files


def expand_token(line: str, start: int, end: int) -> str:
    while start > 0 and IDENT_CHARS.match(line[start - 1]):
        start -= 1
    while end < len(line) and IDENT_CHARS.match(line[end]):
        end += 1
    return line[start:end]


class Allowlist:
    def __init__(self, config: dict):
        self.path_globs: list[str] = config.get("path_globs", [])
        self.identifiers: set[str] = set(config.get("identifiers", []))
        self.comment_ok_paths: list[str] = config.get("comment_ok_paths", [])
        self.line_allow: list[str] = config.get("line_allow", [])

    def path_bucket(self, rel: str) -> str | None:
        for glob in self.path_globs:
            if fnmatch.fnmatch(rel, glob):
                return f"path:{glob}"
        return None

    def comment_ok(self, rel: str) -> bool:
        return any(fnmatch.fnmatch(rel, g) for g in self.comment_ok_paths)

    def line_allowed(self, rel: str, line: str) -> bool:
        for entry in self.line_allow:
            path_sub, _, frag = entry.partition("|")
            if path_sub in rel and frag and frag in line:
                return True
        return False


def scan(allow: Allowlist):
    """Yield (bucket, rel, lineno, token, line) for every host token."""
    for rel in tracked_code_files():
        path_bucket = allow.path_bucket(rel)
        comment_ok = allow.comment_ok(rel)
        marker = COMMENT_MARKERS.get(Path(rel).suffix.lower())
        try:
            text = (REPO / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            for m in HOST_TOKEN.finditer(line):
                if path_bucket:
                    yield path_bucket, rel, lineno, m.group(0), line
                    continue
                if GENERIC_LINE.search(line):
                    yield "generic-vocab", rel, lineno, m.group(0), line
                    continue
                comment_at = line.find(marker) if marker else -1
                in_comment = comment_at != -1 and m.start() >= comment_at
                if in_comment and comment_ok:
                    yield "comment-ok", rel, lineno, m.group(0), line
                    continue
                token = expand_token(line, m.start(), m.end())
                if token in allow.identifiers:
                    yield f"ident:{token}", rel, lineno, token, line
                    continue
                if allow.line_allowed(rel, line):
                    yield "line-allow", rel, lineno, token, line
                    continue
                yield "UNCOVERED", rel, lineno, token, line


def docs_advisory(diff_range: str) -> list[str]:
    findings = []
    diff = run_git("diff", "-U0", diff_range, "--", "docs", "*.md")
    path, lineno = "", 0
    for raw in diff.splitlines():
        if raw.startswith("+++ b/"):
            path = raw[6:]
        elif raw.startswith("@@"):
            m = re.search(r"\+(\d+)", raw)
            lineno = int(m.group(1)) if m else 0
        elif raw.startswith("+") and not raw.startswith("+++"):
            text = raw[1:]
            if path.endswith(".md") and not DOC_HOST_OK.match(path) \
                    and HOST_TOKEN.search(text) \
                    and not GENERIC_LINE.search(text):
                findings.append(f"{path}:{lineno}: {text.strip()}")
            lineno += 1
    return findings


def diff_lines(diff_range: str, *pathspecs: str):
    """(sign, path, text) for every +/- line in the range."""
    diff = run_git("diff", "-U0", diff_range, "--", *pathspecs)
    path = ""
    for raw in diff.splitlines():
        if raw.startswith("+++ b/"):
            path = raw[6:]
        elif raw.startswith(("+++", "---", "@@")):
            continue
        elif raw.startswith(("+", "-")):
            yield raw[0], path, raw[1:]


def frozen_audit(diff_range: str) -> int:
    failures = 0

    touched = run_git("diff", "--name-only", diff_range,
                      "--", *FROZEN_PATHS).splitlines()
    for path in touched:
        print(f"[host-lint][frozen] frozen scope touched: {path}")
        failures += 1

    removed: Counter = Counter()
    added: Counter = Counter()
    for sign, _path, text in diff_lines(diff_range):
        bucket = removed if sign == "-" else added
        for token in FROZEN_TOKENS:
            bucket[token] += text.count(token)

    doc_removed: Counter = Counter()
    doc_added: Counter = Counter()
    for sign, path, text in diff_lines(diff_range, "docs"):
        bucket = doc_removed if sign == "-" else doc_added
        for cite in CITATION.findall(text):
            bucket[cite] += 1
        for did in DIVERGENCE_ID.findall(text):
            bucket[did] += 1

    for token in FROZEN_TOKENS:
        if removed[token] > added[token]:
            print(f"[host-lint][frozen] wire-frozen string lost: {token} "
                  f"(-{removed[token]} +{added[token]})")
            failures += 1
    for item, count in doc_removed.items():
        if count > doc_added[item]:
            print(f"[host-lint][frozen] docs citation/ID lost: {item} "
                  f"(-{count} +{doc_added[item]})")
            failures += 1

    if failures:
        print(f"[host-lint] frozen-audit FAIL: {failures} finding(s)")
        return 1
    print("[host-lint] frozen-audit clean")
    return 0


def main() -> int:
    # Windows consoles default to cp1252; doc lines carry arrows/dashes.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", action="store_true",
                        help="list every uncovered hit, grouped by file")
    parser.add_argument("--enforce", action="store_true",
                        help="exit 1 on any uncovered hit")
    parser.add_argument("--frozen-audit", action="store_true",
                        help="diff guard for rename phases (see docstring)")
    parser.add_argument("--range", dest="diff_range", default=None,
                        help="git diff range for --frozen-audit / advisory")
    args = parser.parse_args()

    if args.frozen_audit:
        diff_range = resolve_range(args.diff_range)
        if diff_range is None:
            print("[host-lint] no usable diff base — skipping frozen-audit")
            return 0
        return frozen_audit(diff_range)

    allow = Allowlist(json.loads(ALLOWLIST_PATH.read_text(encoding="utf-8")))
    buckets: Counter = Counter()
    uncovered: dict[str, list[tuple[int, str]]] = {}
    for bucket, rel, lineno, token, line in scan(allow):
        buckets[bucket] += 1
        if bucket == "UNCOVERED":
            uncovered.setdefault(rel, []).append((lineno, line.strip()))

    covered = sum(v for k, v in buckets.items() if k != "UNCOVERED")
    print(f"[host-lint] {covered} covered hit(s), "
          f"{buckets['UNCOVERED']} uncovered hit(s) "
          f"in {len(uncovered)} file(s)")
    for bucket, count in sorted(buckets.items(),
                                key=lambda kv: -kv[1]):
        if bucket != "UNCOVERED":
            print(f"[host-lint]   {count:5d}  {bucket}")

    if args.report:
        for rel in sorted(uncovered):
            hits = uncovered[rel]
            print(f"[host-lint][uncovered] {rel} ({len(hits)})")
            for lineno, line in hits[:6]:
                print(f"[host-lint]   :{lineno}: {line[:110]}")
            if len(hits) > 6:
                print(f"[host-lint]   ... and {len(hits) - 6} more")

    diff_range = resolve_range(args.diff_range)
    if diff_range is not None:
        advisories = docs_advisory(diff_range)
        for f in advisories[:20]:
            print(f"[host-lint][docs-advisory] {f}")
        if len(advisories) > 20:
            print(f"[host-lint][docs-advisory] ... and "
                  f"{len(advisories) - 20} more")

    if buckets["UNCOVERED"] and args.enforce:
        print("[host-lint] FAIL: \"host\" is reserved for the game/server "
              "host (CONTEXT.md). Rename the identifier (Mount / Presenter "
              "/ Shell / embedder), or extend "
              "scripts/lint/host_allowlist.json if this IS the game host.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
