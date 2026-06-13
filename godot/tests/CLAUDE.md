# godot/tests/ — GUT suite

- Run via `scripts/test_godot.sh` (headless; needs `GODOT_BIN` or a binary in the main
  checkout's `.godot-bin/`). On a fresh worktree run
  `"$GODOT_BIN" --headless --path godot --import` first, or engine classes appear missing.
- Collection: files ending `_test.gd` that extend `GutTest`, subdirs included.
  `*_probe.gd` files are manual probes and are not collected.
- GUT exits 0 when a script fails to parse — it is silently dropped from collection.
  `scripts/test_godot.sh` greps for parse errors and dropped scripts; prefer it over
  invoking `gut_cmdln.gd` directly, and replicate those greps after any direct run.
- The full suite is flaky (shared `user://` state). Before trusting a failure, re-run the
  failing file alone:
  `"$GODOT_BIN" --headless --path godot -s addons/gut/gut_cmdln.gd -gtest=res://tests/<file> -gexit`.
  (Flag reference: `godot/addons/gut/cli/gut_cli.gd` after bootstrap — `-gselect`
  filename substring, `-gunit_test_name` test-name substring.)
- If a test needs a new `Nova*` class, rebuild the GDExtension first
  (`scripts/build_godot.sh`) — see `godot/engine/CLAUDE.md`.
- C++ tests live in `/tests` (ctest). Keep the two suites separate.
