#!/usr/bin/env python3
"""Maturity-program ratchet counters (docs/maturity-program.md, STD-1).

Counts debt classes that must never INCREASE, against the committed baseline
in maturity_baseline.json:

  test_private_pokes    lines in godot/tests/**/*.gd AND godot/probes/**/*.gd
                        that access an _underscore member of ANOTHER object
                        (self._ excluded) -- ADR 0018: each is a missing public
                        seam, on the test side and the probe side alike.
  engine_uncited_src_files  engine/<group>/<lib> source files (post-flatten:
                        no src/ level) with zero "[orig" citations, excluding
                        the allowlisted infra libs (citation is inapplicable
                        there) -- the faithful-port rule's coverage floor.
  adapter_cpp_orig_cites  "[orig:" citations across ALL of godot/src -- the
                        one cite marker (ADR 0042 d7 retired the dual-marker
                        convention; there is no "(retail:" form and no
                        pushdown/device partition any more). Non-increasing:
                        a decrease means witnessed code moved to its engine
                        home or died as verified dead code -- bank it with
                        --write-baseline. The marker-rewrite exit is gone;
                        only code that moves banks the counter.
  mcp_boundary_cites    "[orig:" citations in godot/game/mcp GDScript. An
                        ABSOLUTE zero floor, not baseline-relative (ADR 0042
                        d7): the MCP boundary converts typed records to JSON,
                        and a converter that needs a witness cite is
                        re-deriving. No baseline key exists for it.
  gd_orig_cites         "[orig:" citations in godot/game, godot/modtools and
                        godot/probes GDScript -- witnessed engine behavior
                        still living in the game-level scripts (ADR 0034 d6's
                        C++ rewrite queue, measured; godot/probes joined the
                        scope under ADR 0042 d7). The burn-down class for the
                        push-down campaign; its floor is the device-leg
                        justifications.
  oversize_cpp_headers  .h/.hpp under engine/, apps/, godot/src past the same
                        2500-line limit as oversize_cpp_files (the .cpp glob
                        never saw headers).

Modes:
  (default)         report counts vs baseline; exit 0 regardless (soft mode)
  --enforce         exit 1 if any counter exceeds its baseline (the ratchet)
  --write-baseline  rewrite the baseline to current counts (maintainer
                    action; log the bump in docs/maturity-program.md)

Decreases are reported and SHOULD be committed into the baseline by the
slice that earned them (run --write-baseline in that slice) so the ratchet
only ever tightens.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BASELINE_PATH = Path(__file__).resolve().parent / "maturity_baseline.json"

# Matches an _member access on another object. "self._" hits are stripped
# before this runs; remaining `._name` is a foreign private access.
PRIVATE_POKE = re.compile(r"\._[a-z]")
SELF_POKE = re.compile(r"self\._")


def _in_build_dir(parts) -> bool:
    """Local build output, not source. Generator dirs are named `build`,
    `build-ninja`, `build-vs`, ... and live at the REPO ROOT, as godot's own
    `godot/build*`, or as the GDExtension's `godot/src/build*` (the generated
    godot-cpp tree); only those qualify — a source directory that merely
    starts with "build" elsewhere in the tree (`buildings/`) must stay
    counted."""
    parts = tuple(parts)
    if not parts:
        return False
    if parts[0].startswith("build"):
        return True
    if len(parts) > 1 and parts[0] == "godot" and parts[1].startswith("build"):
        return True
    return len(parts) > 2 and parts[0] == "godot" and parts[1] == "src" and \
        parts[2].startswith("build")


def count_test_private_pokes() -> int:
    count = 0
    for sub in ("tests", "probes"):
        for path in (REPO / "godot" / sub).rglob("*.gd"):
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for line in text.splitlines():
                stripped = SELF_POKE.sub("", line)
                if PRIVATE_POKE.search(stripped):
                    count += 1
    return count


def count_engine_uncited_src_files(allowlist: set[str]) -> int:
    count = 0
    engine = REPO / "engine"
    # engine/<group>/<lib>: the libs are one level below the four group dirs.
    # Post-flatten (2026-08-10) sources sit directly in the lib dir (nested
    # subdirs included); there is no src/ level any more.
    # An allowlist entry is a bare lib-dir name (the whole lib is infrastructure)
    # or an engine-relative path prefix (a subtree or one file with no retail
    # counterpart to cite — the NovaWorld service, a transport shim, a standard
    # codec); docs/maturity-program.md records why each is there.
    prefixes = tuple(entry for entry in allowlist if "/" in entry)
    for lib_dir in sorted(p for p in engine.glob("*/*") if p.is_dir()):
        if lib_dir.name in allowlist:
            continue
        for path in lib_dir.rglob("*"):
            if path.suffix.lower() not in (".c", ".cc", ".cpp"):
                continue
            rel = path.relative_to(engine).as_posix()
            if any(rel == prefix or rel.startswith(prefix.rstrip("/") + "/") for prefix in prefixes):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if "[orig:" not in text:
                count += 1
    return count


# One cite marker, one count (ADR 0042 d7): a witness citation is
# `[orig: Name @0xADDR]` everywhere — the adjudicated `(retail: ...)` note
# form and the pushdown/device partition are retired. The count over all of
# godot/src is non-increasing; a decrease means witnessed code moved to its
# engine home (or died as verified dead code) and is banked with
# --write-baseline. The marker rewrite that let citations leave the gauge is
# no longer an exit: only code that moves banks the counter.
def count_adapter_cpp_orig_cites() -> int:
    count = 0
    adapter = REPO / "godot" / "src"
    for path in adapter.rglob("*"):
        if path.suffix.lower() not in (".c", ".cc", ".cpp", ".h", ".hpp"):
            continue
        rel_parts = path.relative_to(REPO).parts
        if _in_build_dir(rel_parts):  # generated CMake tree (godot-cpp), not source
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        count += text.count("[orig:")
    return count


def count_mcp_boundary_cites() -> int:
    """`[orig:` citations in godot/game/mcp GDScript: an ABSOLUTE zero floor
    (ADR 0042 d7), not a baseline-relative ratchet. The MCP boundary converts
    typed engine records to JSON; a converter that needs a witness cite is
    re-deriving engine facts at the boundary."""
    count = 0
    for path in (REPO / "godot" / "game" / "mcp").rglob("*.gd"):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        count += text.count("[orig:")
    return count


LIBS_PRINT = re.compile(
    r"(?<![\w:])(?:std::)?fprintf\s*\(\s*(?:stderr|stdout)\b"
    r"|(?<![\w:.])(?:std::)?printf\s*\("
    r"|std::cout\b|std::cerr\b"
    r"|(?<![\w:.])puts\s*\(")


def count_engine_stdout_prints() -> int:
    """Console writes inside engine/ (W1-3): libraries route diagnostics through
    the io/log.h sink and stay silent by default — the embedder installs the sink.
    FILE*-parameter writers (fprintf(fp, ...)) are deliberately not matched."""
    count = 0
    engine = REPO / "engine"
    # Post-flatten (2026-08-10): sources/headers sit directly under each
    # engine/<group>/<lib>/ dir (nested subdirs included).
    for path in engine.glob("*/*/**/*"):
        if path.suffix.lower() not in (".c", ".cc", ".cpp", ".h", ".hpp"):
            continue
        if path.name == "log.h" and path.parent.name == "io":
            continue  # the sink's own vsnprintf lives here
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line in text.splitlines():
            code = line.split("//", 1)[0]
            if LIBS_PRINT.search(code):
                count += 1
    return count


GD_PRINT = re.compile(r"(?:^|[^_a-zA-Z\"])(?:print|prints|printerr|print_rich|print_debug)\s*\(")
# The shipping Godot layer routes diagnostics through push_error/push_warning,
# print_verbose, or the dev tools (F3).
GD_PRINT_ALLOWLIST: set[str] = {
    # Hidden release-build CLI: stdout/stderr is its user interface.
    "godot/modtools/pack_game_cli.gd",
}
CPP_CONSOLE = re.compile(
    r"UtilityFunctions::print(?!_verbose)\s*\(|UtilityFunctions::printerr\s*\("
    r"|UtilityFunctions::print_rich\s*\("
    r"|(?<![\w:])print_line\s*\(|\bWARN_PRINT(?:_ONCE|_ED)?\s*\(|\bERR_PRINT(?:_ONCE|_ED)?\s*\(")


def count_gd_prints_outside_debug() -> int:
    """Raw print() family in the shipping godot layer (W1-2): the sanctioned
    channels are push_error/push_warning, print_verbose, and the dev tools (F3).
    godot/tests and the GUT addon are out of scope; the runtime probes under
    godot/probes log through their ProbeContext, so they are in scope.
    godot/src is C++-only (ADR 0034 d6); its .gd leg here is a tripwire."""
    count = 0
    for sub in ("src", "game", "modtools", "probes"):
        for path in (REPO / "godot" / sub).rglob("*.gd"):
            rel = path.relative_to(REPO).as_posix()
            if rel in GD_PRINT_ALLOWLIST:
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for line in text.splitlines():
                code = line.split("#", 1)[0]
                if "print_verbose" in code:
                    code = code.replace("print_verbose", "")
                if GD_PRINT.search(code):
                    count += 1
    return count


HAS_METHOD_GUARD = re.compile(r"(?<!\w)has_method\s*\(")


def count_has_method_guards() -> int:
    """Duck-type guards in the shipping godot layer (W4-2): the floor is the
    documented kept set (harness seams and dynamic dispatch), not zero.
    class_has_method is excluded by the word boundary.
    Covers the native binding layer too (godot/src *.cpp/*.h) - a C++
    has_method() probe is the same duck dispatch, just invisible to GDScript
    greps."""
    count = 0
    for sub in ("src", "game", "modtools", "probes"):
        for path in (REPO / "godot" / sub).rglob("*.gd"):
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for line in text.splitlines():
                count += len(HAS_METHOD_GUARD.findall(line.split("#", 1)[0]))
    for pattern in ("*.cpp", "*.h"):
        for path in (REPO / "godot" / "src").rglob(pattern):
            if _in_build_dir(path.relative_to(REPO).parts):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for line in text.splitlines():
                count += len(HAS_METHOD_GUARD.findall(line.split("//", 1)[0]))
    return count


OVERSIZE_CPP_LINE_LIMIT = 2500


def count_oversize_cpp_files() -> int:
    """Oversized translation units (W3-7, the W3 closer): the god-file splits
    leave two residual offenders; no .cpp under engine/, apps/, or godot/src
    may grow past 2500 lines without splitting first."""
    count = 0
    for root in ("engine", "apps", "godot/src"):
        for path in (REPO / root).rglob("*.cpp"):
            parts = path.relative_to(REPO).parts
            if _in_build_dir(parts):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if len(text.splitlines()) > OVERSIZE_CPP_LINE_LIMIT:
                count += 1
    return count


def count_oversize_cpp_headers() -> int:
    """Oversized headers: the same 2500-line limit as count_oversize_cpp_files,
    applied to .h/.hpp under engine/, apps/, godot/src (a state model the size
    of a translation unit is a split waiting to happen)."""
    count = 0
    for root in ("engine", "apps", "godot/src"):
        for pattern in ("*.h", "*.hpp"):
            for path in (REPO / root).rglob(pattern):
                parts = path.relative_to(REPO).parts
                if _in_build_dir(parts):
                    continue
                try:
                    text = path.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    continue
                if len(text.splitlines()) > OVERSIZE_CPP_LINE_LIMIT:
                    count += 1
    return count


def count_gd_orig_cites() -> int:
    """`[orig:` citations in the game-level GDScript (godot/game,
    godot/modtools, and — since ADR 0042 d7 — godot/probes): witnessed engine
    behavior that ADR 0033/0034 say belongs in engine/. The push-down campaign
    banks this down; the floor is the device-leg justifications (a cite
    explaining WHY a node write happens, not HOW a witnessed value is
    derived)."""
    count = 0
    for sub in ("game", "modtools", "probes"):
        for path in (REPO / "godot" / sub).rglob("*.gd"):
            parts = path.relative_to(REPO).parts
            if "addons" in parts or _in_build_dir(parts):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            count += text.count("[orig:")
    return count


OVERSIZE_GD_LINE_LIMIT = 1200


def count_oversize_gd_files() -> int:
    """Oversized GDScript files (W4-6, the W4 closer): the W4 god-file splits
    leave a ratcheted residual set; no .gd under godot/src, godot/game,
    godot/modtools or godot/probes may grow past 1200 lines without splitting
    first. godot/tests is deliberately out of scope; test-suite file size is a
    separate maintainability concern."""
    count = 0
    for root in ("godot/src", "godot/game", "godot/modtools", "godot/probes"):
        for path in (REPO / root).rglob("*.gd"):
            parts = path.relative_to(REPO).parts
            if "addons" in parts or _in_build_dir(parts):  # vendored addons / build output, not source
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if len(text.splitlines()) > OVERSIZE_GD_LINE_LIMIT:
                count += 1
    return count


def count_cpp_binding_console_writes() -> int:
    """Console writes in the GDExtension bindings (W1-2): error paths use
    push_error/push_warning (the engine's error channel); narration uses
    print_verbose. Raw print/printerr/print_line, the WARN/ERR_PRINT macros,
    AND raw CRT console writes (printf/std::cout/fprintf(stderr) — the same
    family engine_stdout_prints ratchets engine-side) are all at zero."""
    count = 0
    for path in (REPO / "godot" / "src").rglob("*"):
        if path.suffix.lower() not in (".cpp", ".h", ".hpp"):
            continue
        parts = path.relative_to(REPO).parts
        if _in_build_dir(parts):
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line in text.splitlines():
            code = line.split("//", 1)[0]
            if CPP_CONSOLE.search(code) or LIBS_PRINT.search(code):
                count += 1
    return count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--enforce", action="store_true",
                        help="exit 1 when a counter exceeds its baseline")
    parser.add_argument("--write-baseline", action="store_true",
                        help="rewrite the baseline to the current counts")
    args = parser.parse_args()

    config = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    allowlist = set(config.get("citation_allowlist_engine", []))
    baseline = config.get("counters", {})

    current = {
        "test_private_pokes": count_test_private_pokes(),
        "engine_uncited_src_files": count_engine_uncited_src_files(allowlist),
        "adapter_cpp_orig_cites": count_adapter_cpp_orig_cites(),
        "engine_stdout_prints": count_engine_stdout_prints(),
        "gd_prints_outside_debug": count_gd_prints_outside_debug(),
        "cpp_binding_console_writes": count_cpp_binding_console_writes(),
        "oversize_cpp_files": count_oversize_cpp_files(),
        "oversize_cpp_headers": count_oversize_cpp_headers(),
        "oversize_gd_files": count_oversize_gd_files(),
        "has_method_guards": count_has_method_guards(),
        "gd_orig_cites": count_gd_orig_cites(),
    }

    # Absolute floor, no baseline key (ADR 0042 d7): the MCP boundary carries
    # zero witness cites, forever — this never relaxes via --write-baseline.
    mcp_cites = count_mcp_boundary_cites()

    if args.write_baseline:
        config["counters"] = current
        BASELINE_PATH.write_text(
            json.dumps(config, indent=2, sort_keys=True) + "\n",
            encoding="utf-8", newline="\n")
        print(f"[ratchet] baseline rewritten: {current}")
        if mcp_cites > 0:
            print(f"[ratchet] WARNING: mcp_boundary_cites is {mcp_cites} — the "
                  f"floor is 0 and has no baseline; --enforce will fail.")
        return 0

    failed = False
    floor_marker = "OK" if mcp_cites == 0 else "ABOVE FLOOR"
    print(f"[ratchet] mcp_boundary_cites: {mcp_cites} (absolute floor 0) {floor_marker}")
    if mcp_cites > 0:
        print("[ratchet]   godot/game/mcp converts typed records to JSON; a "
              "converter that needs a witness cite is re-deriving (ADR 0042 d7). "
              "Move the witnessed logic to engine/ and delete the cite.")
        failed = True
    for name, value in current.items():
        base = baseline.get(name)
        if base is None:
            print(f"[ratchet] {name}: {value} (NO BASELINE — run --write-baseline)")
            failed = True
            continue
        delta = value - base
        marker = "OK" if delta <= 0 else "INCREASED"
        print(f"[ratchet] {name}: {value} (baseline {base}, {delta:+d}) {marker}")
        if delta > 0:
            failed = True
        elif delta < 0:
            print(f"[ratchet]   improvement — commit it: "
                  f"python scripts/lint/ratchet_counts.py --write-baseline")

    if failed and args.enforce:
        print("[ratchet] FAIL: a counter increased, lacks a baseline, or sits "
              "above an absolute floor. Fix the regression, or (baseline "
              "counters only) a maintainer bumps the baseline and logs it in "
              "docs/maturity-program.md.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
