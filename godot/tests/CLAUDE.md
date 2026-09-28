# godot/tests/ — GUT suite

- Run via `scripts/test_godot.sh` (headless; needs `GODOT_BIN` or a binary in the main
  checkout's `.godot-bin/`). On a fresh worktree run `scripts/bootstrap_godot.sh`
  (installs GUT and the imgui-godot addon the `DevTools` node expects) and then
  `"$GODOT_BIN" --headless --path godot --import`, or engine classes appear missing.
  Headless runs never attach an ImGui context: `DevTools.is_available()` is false
  there while its open state (F3, capture, input policy) still works and is tested.
- Collection: files ending `_test.gd` that extend `GutTest`, subdirs included.
- `--suite core` unsets the retail roots; `--suite retail` requires both roots
  and rejects skipped compatibility tests. The default `--suite all` allows
  explicit missing-data skips. Retail scripts live under `tests/retail/`.
  Scripts whose every test needs a RenderingDevice live under `tests/windowed/`
  (core) and `tests/retail/windowed/` (retail): both are excluded from every
  headless selection and run only with `--windowed` on their suite, where a
  pending test is a failure (a missing RenderingDevice fails; under `all` a
  missing-data skip stays allowed). Headless scripts that mix in RD-gated
  methods keep them `pending` there until they are split into windowed siblings.
  The runner snapshots selected scripts/methods and checks the JUnit report
  for missing coverage. See `docs/asset-gated-tests.md` for migration records.
- No probes live here. Runtime probes are `game_probe` tools under `godot/probes/`
  (`docs/mcp.md`, ADR 0041): registered in `ProbeDef.definitions()`
  (`godot/game/probe/`), driven through the game's MCP endpoint with typed
  arguments, judged by their verdict. Their GUT companions
  (the catalog contract, the render-fixture and foliage-capture contracts, the
  parity-joiner contract) live in `godot/tests/probes/`. An assertion over the
  portable engine is a ctest under `tests/<domain>/`, gated on the retail roots
  when it needs retail data (`docs/asset-gated-tests.md`).
- GUT exits 0 when a script fails to parse — it is silently dropped from collection.
  `scripts/test_godot.sh` greps for parse errors and dropped scripts; prefer it over
  invoking `gut_cmdln.gd` directly, and replicate those greps after any direct run.
- The full suite is flaky (shared `user://` state). Before trusting a failure, re-run the
  failing file alone:
  `"$GODOT_BIN" --headless --path godot -s addons/gut/gut_cmdln.gd -gtest=res://tests/<file> -gexit`.
  (Flag reference: `godot/addons/gut/cli/gut_cli.gd` after bootstrap — `-gselect`
  filename substring, `-gunit_test_name` test-name substring.)
- If a test needs a newly registered engine class, rebuild the GDExtension first
  (`scripts/build_godot.sh`) — see `godot/src/CLAUDE.md`.
- C++ tests live in `/tests` (ctest). Keep the two suites separate.
- Tests drive real fixtures (ADR 0043 rule 11): never subclass a production Node or
  world-layer class to override behavior, never poke a `_private`. Boot the real object
  through `support/world_fixture.gd` (`WorldFixture.boot_minimal` — the packaged
  `game_world.tscn` over generated runtime fixtures, `boot_mission_data`, `boot_shell`,
  `stage_minimal_root` + `stage_effects` + `stage_sound_bank`) or `support/hud_fixture.gd`
  (a real `HudOverlay`), and assert through public read seams (`get_debug_group_report`,
  `get_stats`, `recent_fired_soundsets`, ...). Fake only an interface: the GDScript
  `GameShell` by overriding its public verbs, and the C++ `WorldView`/`ArmoryWorldView`
  (godot/src/world/world_view.h) by overriding their `_` GDVIRTUAL hooks (`_sim`,
  `_resource_root`, `_weapon_database`, ...). `ratchet_counts.py gd_test_production_subclasses` holds the residue.
- Always `autofree`/`add_child_autofree` what you create. A loaded `GameWorld` must be
  unloaded before it is freed (`WorldFixture.make_world` does it on tree exit): a world
  freed loaded, or a leaked instance carrying a `Transform3D`-typed member, segfaults the
  whole run at process exit (Godot 4.6 teardown quirk, bisected 2026-08-09) while GUT
  still reports green totals, so the crash only shows as a nonzero exit code.
- `Simulation` is a RefCounted (ADR 0043 slice G2): never `free()` it, and hold the Ref
  for as long as a consumer reads it. The passes, `Weather`, `AmbientMixer` and `DevTools`
  keep only its ObjectID, so a sim built as a temporary (`_wire_pass(_sim(), ...)`) is gone
  before the first read; keep it on the test (`var _sims: Array[Simulation]`, cleared in
  `after_each`) like `wire_present_pass_test.gd`.
- A new `class_name` (a fixture, a production class) is invisible to a headless GUT run
  until `"$GODOT_BIN" --headless --path godot --import` refreshes the class cache; the
  symptom is "Could not find base class" and a silently dropped script.
- A fixture whose world must run holds a human: a playable load, the listen host's own
  player, or `spawn_local_player` (`simulation_test.gd`'s `_spawn_fixture_human`). Once the
  WAC clock starts, retail's frame gate holds the entity update of a world with no human
  (world-wac-ai-re §38.9), and a `set(ticks,-1)` WAC write opens only the script admission.
  A staged items.def override must lead its file (a type id resolves to its first row), and
  a placed item's brain, part-anim channels and AI card need the items table before the load
  (the brain test is read at spawn).
- Every 3DI model the suite loads is synthetic: `fixtures/threedi/synth/*.3di` are
  minted by `tests/fixtures/minimal_3di_gen.cpp` through the engine's parity writer
  (eleven base models plus the one-edit variants: CTRL names, PANM rows, LGHT/material
  fields) and byte-compared by the `minimal_3di_gen` ctest every run. Change the
  generator, run it with `--write`, commit the files. A test that needs an authored
  variant loads one of these and asserts its content with the read-back getters; the
  model and variant tables are in `fixtures/README.md`. No retail model lives in the
  tree; retail-only assertions run as `OPENNOVA_JO_ASSETS` legs.
