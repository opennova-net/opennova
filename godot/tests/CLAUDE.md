# godot/tests/ — GUT suite

- Run via `scripts/test_godot.sh` (headless; needs `GODOT_BIN` or a binary in the main
  checkout's `.godot-bin/`). On a fresh worktree run
  `"$GODOT_BIN" --headless --path godot --import` first, or engine classes appear missing.
- Collection: files ending `_test.gd` that extend `GutTest`, subdirs included.
  `*_probe.gd` files are manual probes and are not collected.
- Probes: extends-`SceneTree` scripts run
  `"$GODOT_BIN" --headless --path godot -s res://tests/<x>_probe.gd`, asset-gated on
  `NW_SP_MISSION=<bms>` + `NW_RESOURCE_DIR=<retail install>` (never CI). They print a
  graded PASS/FAIL verdict and are judged on the printed verdict, not the exit code —
  Godot 4.6 teardown can exit 139 after a PASS. Windowed/visual probes are `.tscn`
  scenes under `tests/game/` run with `--path godot res://tests/game/<x>.tscn`,
  capturing via viewport self-readback into an env-named dir. For a new probe, copy an
  existing probe's header (`ladder_climb_probe.gd`).
- GUT exits 0 when a script fails to parse — it is silently dropped from collection.
  `scripts/test_godot.sh` greps for parse errors and dropped scripts; prefer it over
  invoking `gut_cmdln.gd` directly, and replicate those greps after any direct run.
- The full suite is flaky (shared `user://` state). Before trusting a failure, re-run the
  failing file alone:
  `"$GODOT_BIN" --headless --path godot -s addons/gut/gut_cmdln.gd -gtest=res://tests/<file> -gexit`.
  (Flag reference: `godot/addons/gut/cli/gut_cli.gd` after bootstrap — `-gselect`
  filename substring, `-gunit_test_name` test-name substring.)
- If a test needs a new `Nova*` class, rebuild the GDExtension first
  (`scripts/build_godot.sh`) — see `godot/src/CLAUDE.md`.
- C++ tests live in `/tests` (ctest). Keep the two suites separate.
- Always `autofree`/`add_child_autofree` harness objects — never leak a
  `GameWorld`-extending harness. A leaked instance segfaults the whole run at
  process exit when the class carries a `Transform3D`-typed member (Godot 4.6
  teardown quirk, bisected 2026-08-09) and GUT still reports green totals, so
  the crash only shows as a nonzero exit code.
