---
name: gut
description: Runs the GDScript/GUT test suite under godot/tests the way this repo requires — fresh-worktree setup, silent-drop detection, single-file isolation, and the flaky-failure protocol. Use when running or debugging Godot-side tests, when a GUT failure looks flaky, when adding a *_test.gd, or when the suite is suspiciously green after native changes.
---

# Run GUT tests correctly

GUT exits 0 even when test scripts fail to parse or are silently dropped from
collection. Never trust a green run that wasn't produced by the steps below.
All commands are Git Bash, from the repo root.

## Preconditions (fresh worktree especially)

1. `GODOT_BIN`: in a worktree the wrapper's fallback fails (it only checks the
   worktree's own `.godot-bin/`, which exists only in the main checkout). Set it
   from the main checkout:

       main="$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")"
       export GODOT_BIN="$main/.godot-bin/Godot_v4.6.1-stable_win64_console.exe"

2. Submodules: `git submodule update --init --recursive` — `third_party/` ships
   empty in a fresh worktree. (`scripts/test_godot.sh` self-inits the GUT
   submodule, but building the GDExtension needs godot-cpp too.)
3. Addons installed: `bash scripts/bootstrap_godot.sh` — it copies GUT into
   `godot/addons/gut/` AND runs `scripts/bootstrap_imgui_godot.sh` for the
   imgui-godot addon the `DevTools`/`OnedUi` nodes draw through (ADR 0039).
   `scripts/build.sh` and `scripts/test_godot.sh` call it; `scripts/build_godot.sh`
   does NOT, so a build-only path leaves both addons missing.
4. GDExtension built: fresh worktrees have no DLL in `godot/bin/` →
   `bash scripts/build_godot.sh`. Unregistered engine classes make tests drop
   from collection (see failure signatures below).
5. Imported once: `"$GODOT_BIN" --headless --path godot --import`
   (the wrapper does NOT do this).

## Full suite

    bash scripts/test_godot.sh

This is the trusted entry point: it greps the log for
`SCRIPT ERROR: Parse Error` and `Ignoring script .*does not extend GutTest`
and fails on either — both mean a script was silently skipped, usually a parse
error or an unregistered GDExtension class (stale/missing DLL).

## Single file / single test (isolation runs)

The wrapper takes no arguments; invoke GUT directly:

    "$GODOT_BIN" --headless --path godot -s addons/gut/gut_cmdln.gd \
      -gtest=res://tests/<file>_test.gd -gexit

- One test within the file: add `-gunit_test_name=<substring>`.
- Filename-substring selection needs the wrapper's collection flags:
  `-gdir=res://tests -ginclude_subdirs -gprefix= -gsuffix=_test.gd -gselect=<substring>`
  (GUT's default prefix is `test_`; this repo's files are suffix-named).
  Flag reference: `godot/addons/gut/cli/gut_cli.gd` (installed by bootstrap).
- A direct run bypasses the wrapper's safety greps — after EVERY direct run,
  grep the captured output for `Parse Error` and `does not extend GutTest`
  before believing it.

## Flaky-failure protocol (mandatory)

Full-suite failures can come from shared `user://` state (tests can encounter
the same persisted product config as a local run, including
`user://oned.cfg`). On any reported failure:

1. Re-run that test FILE alone with `-gtest=` as above.
2. Fails alone → real failure; debug it.
3. Passes alone → order/state dependency. Identify the leak (which earlier test
   writes `user://` or leaves autoload/static residue) and fix the leaking test
   or the victim's assumptions. Re-running the suite until it passes is never an
   acceptable resolution.

## Adding tests

`extends GutTest`, named `*_test.gd`, anywhere under `godot/tests/` (subdirs
are collected). Repo fixtures are reached one level above `res://`:
`ProjectSettings.globalize_path("res://").path_join("../fixtures/...")`, with a
skip-and-pass (`pending`/`pass_test`) when the fixture or asset env var is
absent — so a green count does not prove coverage; check for "skipping"/pending
lines when you expected assets present. Retail data comes from the four roots
through `RetailData` (`godot/tests/support/retail_data.gd`), never a bespoke env var.
Runtime probes are not GUT scripts: they live under `godot/probes/` as `game_probe`
tools (`docs/mcp.md`); only their contract tests live here. Never bulk-edit `.gd` files with
PowerShell 5.1 Get/Set-Content (BOM mangling) — do bulk text rewrites with bash
sed/perl, not PowerShell (root CLAUDE.md).

## Done

Wrapper exits 0 with no parse/drop lines, and every failure you saw was either
reproduced in isolation or explained by an identified state leak. The CI
`godot-tests` job runs this same wrapper.
