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
  godot_orig_cites      "[orig:" citations across the whole Godot side --
                        godot/src C++ plus the godot/game, godot/modtools and
                        godot/probes GDScript -- as ONE count (ADR 0043 merged
                        the former adapter_cpp_orig_cites / gd_orig_cites
                        pair). A cite moves freely between the two Godot-side
                        languages; the gauge falls only when witnessed code
                        reaches its engine/ home or dies as verified dead
                        code -- bank it with --write-baseline. The one cite
                        marker is `[orig: Name @0xADDR]` (ADR 0042 d7).
  gd_foreign_private_accesses
                        lines in the shipping GDScript (godot/game,
                        godot/modtools) that access an _underscore member of
                        ANOTHER object (self._ excluded) -- a "method annex"
                        reaching into its owner's privates is not a class
                        boundary (ADR 0043). Non-increasing; target zero.
  mcp_boundary_cites    "[orig:" citations in godot/game/mcp GDScript plus
                        the typed debug-control table
                        (godot/game/debug/debug_controls.gd, ADR 0042 d5). An
                        ABSOLUTE zero floor, not baseline-relative (ADR 0042
                        d7): the MCP boundary converts typed records to JSON,
                        and a converter that needs a witness cite is
                        re-deriving. No baseline key exists for it.
  godot_node_meta_sites Object metadata calls (set_meta / get_meta / has_meta /
                        remove_meta) anywhere under godot/ (bindings, game
                        scripts, modtools, probes, tests). An ABSOLUTE zero
                        floor like mcp_boundary_cites: a fact hung on a node
                        by string key is an untyped record nobody can find;
                        it belongs on the node class as a typed property, on
                        a RefCounted record, or in the owner's own table.
  oversize_cpp_headers  .h/.hpp under engine/, apps/, godot/src past the same
                        2500-line limit as oversize_cpp_files (the .cpp glob
                        never saw headers).
  gd_dict_key_sites     Dictionary-keyed reads (`x["key"]`, `.get("key"`) on
                        code lines of the shipping GDScript (godot/game,
                        godot/modtools), excluding godot/game/mcp -- the
                        sanctioned JSON transport edge. Each is a record that
                        crosses untyped (ADR 0017); typed records burn it down.
  godot_src_dictionary_returns
                        Binding methods declared to return Dictionary or
                        TypedArray<Dictionary> in godot/src/**/*.h -- the seams
                        still handing GDScript untyped records instead of a
                        RefCounted row (ADR 0042 d5).

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


def _count_private_pokes(subdirs: tuple[str, ...]) -> int:
    count = 0
    for sub in subdirs:
        for path in (REPO / "godot" / sub).rglob("*.gd"):
            parts = path.relative_to(REPO).parts
            if "addons" in parts or _in_build_dir(parts):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for line in text.splitlines():
                # Code only: a comment naming `GameWorld._runtime` documents a
                # seam, it does not poke one.
                stripped = SELF_POKE.sub("", line.split("#", 1)[0])
                if PRIVATE_POKE.search(stripped):
                    count += 1
    return count


def count_test_private_pokes() -> int:
    return _count_private_pokes(("tests", "probes"))


def count_gd_foreign_private_accesses() -> int:
    """Foreign `._member` accesses in the SHIPPING GDScript (godot/game,
    godot/modtools): the annex pattern -- a RefCounted "method annex" split
    off its owner for size and reaching back through `_owner._field` -- is
    C++ `friend` without the keyword. A class owns its state; an object that
    needs another's privates is a method of that other class (ADR 0043)."""
    return _count_private_pokes(("game", "modtools"))


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


# One cite marker, one count (ADR 0042 d7 + ADR 0043): a witness citation is
# `[orig: Name @0xADDR]` everywhere, and the whole Godot side -- the
# godot/src C++ bindings and the game-level GDScript -- is ONE gauge. A cite
# moving between GDScript and binding C++ leaves the count unchanged; the
# count falls only when witnessed code reaches its engine/ home or dies as
# verified dead code, and that decrease is banked with --write-baseline.
def count_godot_orig_cites() -> int:
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


# The debug-control table (ADR 0042 d5) shares the boundary floor: its rows
# forward into engine functions, and a row needing a witness cite would be
# re-deriving engine behavior instead of calling it.
MCP_BOUNDARY_EXTRA_FILES = (
    Path("godot") / "game" / "debug" / "debug_controls.gd",
)


def count_mcp_boundary_cites() -> int:
    """`[orig:` citations in godot/game/mcp GDScript plus the typed
    debug-control table: an ABSOLUTE zero floor (ADR 0042 d7), not a
    baseline-relative ratchet. The MCP boundary converts typed engine records
    to JSON; a converter that needs a witness cite is re-deriving engine
    facts at the boundary."""
    count = 0
    paths = list((REPO / "godot" / "game" / "mcp").rglob("*.gd"))
    paths += [REPO / extra for extra in MCP_BOUNDARY_EXTRA_FILES]
    for path in paths:
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


GD_DICT_KEY_SITE = re.compile(r'\["[A-Za-z_]\w*"\]|\.get\("')


def count_gd_dict_key_sites() -> int:
    """Dictionary-keyed reads in the shipping GDScript (godot/game and
    godot/modtools; godot/game/mcp excluded as the sanctioned JSON edge):
    `row["key"]` and `.get("key"` on code lines. Each site is a record
    crossing a seam untyped (ADR 0017); a typed RefCounted row removes its
    sites. The floor is the documented transport edges (the dict-contract
    allowlist in this file's baseline)."""
    count = 0
    for sub in ("game", "modtools"):
        for path in (REPO / "godot" / sub).rglob("*.gd"):
            parts = path.relative_to(REPO).parts
            if "addons" in parts or _in_build_dir(parts):
                continue
            if sub == "game" and len(parts) > 2 and parts[2] == "mcp":
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for line in text.splitlines():
                count += len(GD_DICT_KEY_SITE.findall(line.split("#", 1)[0]))
    return count


GODOT_SRC_DICTIONARY_RETURN = re.compile(
    r"^\s*(static\s+)?(virtual\s+)?(TypedArray<Dictionary>|Dictionary)\s+\w+\s*\(")


def count_godot_src_dictionary_returns() -> int:
    """Binding methods declared to return Dictionary or TypedArray<Dictionary>
    in godot/src/**/*.h: the seams that still hand GDScript an untyped record
    where ADR 0042 d5 wants a RefCounted row (EntityRow, FeedRow, ...). A
    to_json_value() converter is not counted (it returns to the MCP edge);
    only method declarations whose return TYPE is the Dictionary."""
    count = 0
    for path in (REPO / "godot" / "src").rglob("*.h"):
        parts = path.relative_to(REPO).parts
        if _in_build_dir(parts):
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line in text.splitlines():
            if GODOT_SRC_DICTIONARY_RETURN.match(line) and "to_json_value" not in line:
                count += 1
    return count


NODE_META_CALL = re.compile(r"\b(?:set_meta|get_meta|has_meta|remove_meta|get_meta_list)\s*\(")


def count_godot_node_meta_sites() -> int:
    """Object metadata calls under godot/ (every .gd/.cpp/.h outside the
    addons and build trees): a fact hung on a node by string key is an
    untyped record with no declared owner. The floor is zero — the placer,
    the wire pass, the audio mixer and the model materials all moved theirs
    onto typed node properties, RefCounted records or owner tables."""
    count = 0
    for path in (REPO / "godot").rglob("*"):
        if path.suffix not in (".gd", ".cpp", ".h"):
            continue
        parts = path.relative_to(REPO).parts
        if "addons" in parts or "third_party" in parts or _in_build_dir(parts):
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line in text.splitlines():
            code = line.split("#", 1)[0] if path.suffix == ".gd" else line.split("//", 1)[0]
            count += len(NODE_META_CALL.findall(code))
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
        "gd_foreign_private_accesses": count_gd_foreign_private_accesses(),
        "engine_uncited_src_files": count_engine_uncited_src_files(allowlist),
        "godot_orig_cites": count_godot_orig_cites(),
        "engine_stdout_prints": count_engine_stdout_prints(),
        "gd_prints_outside_debug": count_gd_prints_outside_debug(),
        "cpp_binding_console_writes": count_cpp_binding_console_writes(),
        "oversize_cpp_files": count_oversize_cpp_files(),
        "oversize_cpp_headers": count_oversize_cpp_headers(),
        "has_method_guards": count_has_method_guards(),
        "gd_dict_key_sites": count_gd_dict_key_sites(),
        "godot_src_dictionary_returns": count_godot_src_dictionary_returns(),
    }

    # Absolute floors, no baseline key: the MCP boundary carries zero witness
    # cites (ADR 0042 d7) and godot/ hangs zero facts on node metadata,
    # forever — neither relaxes via --write-baseline.
    mcp_cites = count_mcp_boundary_cites()
    meta_sites = count_godot_node_meta_sites()

    if args.write_baseline:
        config["counters"] = current
        BASELINE_PATH.write_text(
            json.dumps(config, indent=2, sort_keys=True) + "\n",
            encoding="utf-8", newline="\n")
        print(f"[ratchet] baseline rewritten: {current}")
        if mcp_cites > 0:
            print(f"[ratchet] WARNING: mcp_boundary_cites is {mcp_cites} — the "
                  f"floor is 0 and has no baseline; --enforce will fail.")
        if meta_sites > 0:
            print(f"[ratchet] WARNING: godot_node_meta_sites is {meta_sites} — the "
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
    meta_marker = "OK" if meta_sites == 0 else "ABOVE FLOOR"
    print(f"[ratchet] godot_node_meta_sites: {meta_sites} (absolute floor 0) {meta_marker}")
    if meta_sites > 0:
        print("[ratchet]   a fact hung on a node by string key is an untyped record "
              "nobody can find. Put it on the node class as a typed property, "
              "on a RefCounted record, or in the owner's own table.")
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
