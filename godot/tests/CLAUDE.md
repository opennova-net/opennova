# godot/tests/ - GUT suite

Use the [GUT skill](../../.claude/skills/gut/SKILL.md) for setup,
collection, silent-drop checks, and isolated reruns. This directory
contains `*_test.gd` files extending `GutTest`; native engine assertions
belong in `tests/`. Runtime `game_probe` tools live in `godot/probes/`
([MCP guide](../../docs/mcp.md)); only their contracts are tested here.

Headless runs have no ImGui context: `DevTools.is_available()` is false,
though its open state and input policy remain testable.

- Tests drive real fixtures (ADR 0043 rule 11): never subclass a production Node or
  world-layer class to override behavior, never poke a `_private`. Boot the real object
  through `support/world_fixture.gd` (`WorldFixture.boot_minimal` — the packaged
  `game_world.tscn` over generated runtime fixtures, `boot_mission_data`, `boot_shell`,
  `stage_minimal_root` + `stage_effects` + `stage_sound_bank`) or `support/hud_fixture.gd`
  (a real `HudOverlay`), and assert through public read seams (`get_debug_group_report`,
  `get_stats`, `recent_fired_soundsets`, ...). Fake only a GDScript INTERFACE class by
  overriding its public verbs (`GameShell`, `WorldView`/`ArmoryWorldView`). `ratchet_counts.py gd_test_production_subclasses` holds the residue.
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
- Every 3DI model the suite loads is synthetic: `fixtures/threedi/synth/*.3di` are
  minted by `tests/fixtures/minimal_3di_gen.cpp` through the engine's parity writer
  (eleven base models plus the one-edit variants: CTRL names, PANM rows, LGHT/material
  fields) and byte-compared by the `minimal_3di_gen` ctest every run. Change the
  generator, run it with `--write`, commit the files. A test that needs an authored
  variant loads one of these and asserts its content with the read-back getters; the
  model and variant tables are in `fixtures/README.md`. No retail model lives in the
  tree; retail-only assertions run as `OPENNOVA_JO_ASSETS` legs.
