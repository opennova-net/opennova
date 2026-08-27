class_name ProbeCatalog
extends RefCounted

## THE list of runtime probes (docs/mcp.md, ADR 0041): every game_probe entry
## is declared here with its script under res://probes/. The scripts are
## source-only (export_presets.cfg excludes probes/*), so a shipped build
## still lists the catalog but reports each probe unavailable; load_probe
## resolves them with load() at run time for that reason.

const PERF := "res://probes/perf/"
const RENDER := "res://probes/render/"

# Built once: the runner and the tool look definitions up by identity.
static var _definitions: Array[ProbeDef] = []


static func definitions() -> Array[ProbeDef]:
	if _definitions.is_empty():
		_definitions = _build_definitions()
	return _definitions


static func _build_definitions() -> Array[ProbeDef]:
	return [
		ProbeDef.make("perf_sample",
				"Sample the shell's frame-time counters (world tick legs, the audio "
				+ "leg, the runtime's sim/present/effects spans) for a fixed window "
				+ "after a warm-up, vsync off; avg/p95/max per counter and the frame wall time.",
				PERF + "perf_sample_probe.gd", {
					"warm_ms": { "type": "integer", "minimum": 0, "default": 6000 },
					"sample_ms": { "type": "integer", "minimum": 100, "default": 10000 },
					"counters": { "type": "array", "items": { "type": "string" },
							"default": ["audio", "audio_tick", "present", "sim", "world"] },
				}, [], false, true, 120_000),
		ProbeDef.make("perf_fire",
				"The full-auto performance probe: equip a clip weapon, tap for the "
				+ "first-shot hitch split, measure BASELINE / FIRING x2 / COOLDOWN, then "
				+ "the attribution legs (HUD, world tick, present channels, occlusion, "
				+ "reflection, particle tick, handlers, _process). Fails on a firing regression.",
				PERF + "perf_fire_probe.gd", {
					"pose_json": { "type": "string", "default": "" },
					"look_dy": { "type": "number", "default": 120.0 },
					"fire_seconds": { "type": "number", "minimum": 0.1, "default": 3.0 },
					"baseline_only": { "type": "boolean", "default": false },
					"weapon": { "type": "string", "default": "" },
					"taps": { "type": "boolean", "default": true },
				}, [], true, true, 600_000),
		ProbeDef.make("perf_sweep",
				"The frame-cost decomposition sweep: disable one processing group at a "
				+ "time and measure the saving, then bound the render share (half "
				+ "resolution, cull mask 0); a savings table, largest first.",
				PERF + "perf_sweep_probe.gd", {
					"phase_ms": { "type": "integer", "minimum": 200, "default": 2600 },
				}, [], true, true, 1_200_000),
		ProbeDef.make("perf_mission_rows",
				"The exact Mission Rows benchmark: drain FrameStats once per frame over "
				+ "measurement windows and write the schema-1 JSON record (PRESENT_MISSION, "
				+ "whole-present, world, wall-frame, outside-shell, the engine split).",
				PERF + "perf_mission_rows_probe.gd", {
					"warmup_seconds": { "type": "number", "minimum": 0.0, "default": 6.0 },
					"window_seconds": { "type": "number", "minimum": 0.1, "default": 10.0 },
					"windows": { "type": "integer", "minimum": 1, "default": 5 },
					"label": { "type": "string", "default": "unlabeled" },
					"show_overlay": { "type": "boolean", "default": false },
					"output": { "type": "string", "default": "" },
				}, [], true, true, 1_200_000),
		ProbeDef.make("frame_stats",
				"The F3 Stats acceptance probe: open the dev tools' Stats window, let the "
				+ "capture fill, snapshot every row twice, pass when the load-bearing rows "
				+ "(world, sim, present, occl_apply, hud) carry live numbers.",
				PERF + "frame_stats_probe.gd", {
					"settle_ms": { "type": "integer", "minimum": 0, "default": 3000 },
					"window_ms": { "type": "integer", "minimum": 100, "default": 2000 },
				}, [], true, true, 120_000),
		ProbeDef.make("render_fixture_capture",
				"The exact-pose production-world capture for one docs/render/render-fixtures-v1.json "
				+ "fixture: boots its saved mission, realizes the catalog camera/TOD, captures "
				+ "every variant of the profile and publishes the manifest sibling set "
				+ "transactionally (default output under .scratch/golden/render).",
				RENDER + "render_fixture_capture_probe.gd", {
					"id": { "type": "string", "minLength": 1 },
					"catalog": { "type": "string", "default": "" },
					"minute": { "type": "string", "default": "" },
					"mode": { "type": "string", "default": "" },
					"profile": { "type": "string", "default": "" },
					"suppress_static_bms_ids": { "type": "string", "default": "" },
					"output_dir": { "type": "string", "default": "" },
					"mission_resource_dir": { "type": "string", "default": "" },
					"source_commit": { "type": "string", "minLength": 1 },
					"gdextension_binary": { "type": "string", "minLength": 1 },
				}, ["id", "source_commit", "gdextension_binary"], true, false, 1_800_000),
		ProbeDef.make("render_swatch",
				"The render material swatch A/B driver (ADR 0023) on an off-screen stage: "
				+ "capture, composite, lighting, channels, clip, projshadow, matchterrain, "
				+ "glow, calibrate, or compare two grid captures (a, b; headless is fine).",
				RENDER + "render_swatch_probe.gd", {
					"mode": { "type": "string", "enum": ["capture", "composite", "lighting", "channels",
							"clip", "projshadow", "matchterrain", "glow", "compare", "calibrate"] },
					"output_dir": { "type": "string", "default": "" },
					"prefix": { "type": "string", "default": "" },
					"a": { "type": "string", "default": "" },
					"b": { "type": "string", "default": "" },
				}, ["mode"], false, false, 600_000),
		ProbeDef.make("foliage_spawn_capture",
				"The retail-comparison capture at the real local-player spawn of a saved "
				+ "loose mission (00TRe by default): foliage visible and hidden at the frozen "
				+ "exact spawn, or the fixed-input flicker leg over the spawn tiers.",
				RENDER + "foliage_spawn_capture_probe.gd", {
					"mission": { "type": "string", "default": "00TRe.bms" },
					"mission_path": { "type": "string", "minLength": 1 },
					"expansion": { "type": "string", "default": "revx02" },
					"output_dir": { "type": "string", "default": "" },
					"flicker": { "type": "boolean", "default": false },
					"model_lighting_trace": { "type": "boolean", "default": false },
				}, ["mission_path"], true, false, 900_000),
		ProbeDef.make("environment_cube_capture",
				"The highest-quality environment-cube proof on Forward+ D3D12: six rendered "
				+ "faces, the X/Z axis map, Cubemap sampling orientation and the 0x60 "
				+ "gamma-byte dim, on the probe's own stage.",
				RENDER + "environment_cube_capture_probe.gd", {}, [], true, false, 120_000),
		ProbeDef.make("loading_screen_render",
				"The menu -> mission loading handoff: from the main menu, start the mission "
				+ "and accept only a loading frame that carries both the loading art and the "
				+ "red progress bar (the mounted root must hold the mission; mnml.bms by default).",
				RENDER + "loading_screen_render_probe.gd", {
					"mission": { "type": "string", "default": "mnml.bms" },
				}, [], true, false, 120_000),
	]


static func definition(name: String) -> ProbeDef:
	for def in definitions():
		if def.name == name:
			return def
	return null


## Whether the probe's script is present in this build.
static func is_available(def: ProbeDef) -> bool:
	return def != null and not def.script_path.is_empty() \
			and ResourceLoader.exists(def.script_path)


## A fresh probe instance, or null when the script is absent or is not a
## GameProbe.
static func load_probe(def: ProbeDef) -> GameProbe:
	if not is_available(def):
		return null
	var script := load(def.script_path) as GDScript
	if script == null:
		return null
	var instance: Variant = script.new()
	return instance as GameProbe
