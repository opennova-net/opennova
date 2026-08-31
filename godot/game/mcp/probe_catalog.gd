class_name ProbeCatalog
extends RefCounted

## THE list of runtime probes (docs/mcp.md, ADR 0041): every game_probe entry
## is declared here with its script under res://probes/. The scripts are
## source-only (export_presets.cfg excludes probes/*), so a shipped build
## still lists the catalog but reports each probe unavailable; load_probe
## resolves them with load() at run time for that reason.

const PERF := "res://probes/perf/"
const RENDER := "res://probes/render/"
const STAGE := "res://probes/stage/"
const RUNTIME := "res://probes/runtime/"
const NET := "res://probes/net/"

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
				+ "capture, composite, lighting, channels, clip, matchterrain, "
				+ "glow, calibrate, or compare two grid captures (a, b; headless is fine).",
				RENDER + "render_swatch_probe.gd", {
					"mode": { "type": "string", "enum": ["capture", "composite", "lighting", "channels",
							"clip", "matchterrain", "glow", "compare", "calibrate"] },
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
		ProbeDef.make("loading_screen_render",
				"The menu -> mission loading handoff: from the main menu, start the mission "
				+ "and accept only a loading frame that carries both the loading art and the "
				+ "red progress bar (the mounted root must hold the mission; mnml.bms by default).",
				RENDER + "loading_screen_render_probe.gd", {
					"mission": { "type": "string", "default": "mnml.bms" },
				}, [], true, false, 120_000),
		ProbeDef.make("effects_visual",
				"The environment-particle visual probe: the mounted .ptl set in a bare "
				+ "EffectWorld on a probe stage, six representative effects spawned into a "
				+ "dark scene, frames captured over their lifetimes with live counters.",
				STAGE + "effects_visual_probe.gd", {
					"output_dir": { "type": "string", "default": "" },
				}, [], true, false, 120_000),
		ProbeDef.make("firebarrel_visual",
				"Effect_FireBarrelS through the game's effect layer on a probe stage at 62 Hz "
				+ "ticks, captured at 1..4 s: the barrel flame renders orange again "
				+ "(the case-insensitive curve-table resolve).",
				STAGE + "firebarrel_visual_probe.gd", {
					"output_dir": { "type": "string", "default": "" },
				}, [], true, false, 120_000),
		ProbeDef.make("gore_set_visual",
				"D-PTL-24 pixels: Effect_AmHitBody at three flesh-hit anchors over a clean "
				+ "baseline on a probe stage; passes on five authored emitters and a real "
				+ "changed-pixel count (before/after PNGs).",
				STAGE + "gore_set_visual_probe.gd", {
					"output_dir": { "type": "string", "default": "" },
				}, [], true, false, 120_000),
		ProbeDef.make("bridge_water_shock",
				"D-ITEM-19 evidence: the bridge/water stage before destruction, then the "
				+ "unowned Effect_ShockWaterBrdg family at three DEAD anchors; the best of "
				+ "four tick captures (before/after PNGs).",
				STAGE + "bridge_water_shock_probe.gd", {
					"output_dir": { "type": "string", "default": "" },
				}, [], true, false, 120_000),
		ProbeDef.make("retail_parity_visual",
				"The D-HUD-11 / D-ITEM-16 / D-ITEM-17 annotated stages through the public "
				+ "runtime/presentation seams: attach (the attach-label gate), debris "
				+ "(collision-triangle section debris), glass (glass userpoint shatter).",
				STAGE + "retail_parity_visual_probe.gd", {
					"mode": { "type": "string", "enum": ["attach", "debris", "glass"] },
					"output_dir": { "type": "string", "default": "" },
				}, ["mode"], true, false, 120_000),
		ProbeDef.make("foliage_flicker_regression",
				"The asset-free foliage regression loop on probe stages: the MODEL "
				+ "ONE/ONE blend + depth contract, the detail D3DCMP_GREATER alpha "
				+ "equality, then near/detail and distant MODEL tiers through the real "
				+ "dispatcher: non-black, frame-stable, no exact-black blob.",
				STAGE + "foliage_flicker_regression_probe.gd", {
					"output_dir": { "type": "string", "default": "" },
				}, [], true, false, 300_000),
		ProbeDef.make("loading_screen_stage",
				"The mission loading screen on a probe stage: session_hold (the MP session "
				+ "composite held at a fixed progress, then the SP start-mission splash) or "
				+ "splash_capture (the splash at both blink phases and dismissed).",
				STAGE + "loading_screen_stage_probe.gd", {
					"mode": { "type": "string", "enum": ["session_hold", "splash_capture"],
							"default": "session_hold" },
					"hold_seconds": { "type": "number", "minimum": 0.0, "default": 6.0 },
					"mission": { "type": "string", "default": "" },
					"output_dir": { "type": "string", "default": "" },
				}, [], true, false, 120_000),
		ProbeDef.make("dialog_vs_ambient",
				"The dialog-vs-ambience gate at the MissionAudio seam on the mounted root: "
				+ "set up a mission's audio (ambient marker voices on unless `ambient` is "
				+ "false), play the first `lines` resolvable .DBF dialog ids and assert each "
				+ "spawns, is audible on the Voice bus and advances the queue; reports the "
				+ "ambient/master peaks while dialog plays. Menu launch (no loaded world).",
				RUNTIME + "dialog_vs_ambient_probe.gd", {
					"lines": { "type": "integer", "minimum": 1, "default": 3 },
					"ambient": { "type": "boolean", "default": true },
					"mission": { "type": "string", "default": "00TRa.bms" },
					"sim_ticks": { "type": "integer", "minimum": 0, "default": 0 },
				}, [], false, false, 300_000),
		ProbeDef.make("mission_audio",
				"The mission-audio setup report for one or more missions on the mounted "
				+ "root (every listed .bms when `missions` is empty, cap 12): markers "
				+ "resolved, banks loaded, ambient candidates, dialogs, and the physical "
				+ "pool's loop regions (an empty region is a silent voice).",
				RUNTIME + "mission_audio_probe.gd", {
					"missions": { "type": "array", "items": { "type": "string" }, "default": [] },
				}, [], false, false, 300_000),
		ProbeDef.make("fp_impact",
				"The impact-position probe on the loaded mission: equip `weapon` when given, "
				+ "walk forward, aim down, fire a burst through the real input path, then "
				+ "dump the camera aim ray against every live effect-world group (name, sim "
				+ "position, rendered bounds) and capture the frame; fails on no live group.",
				RUNTIME + "fp_impact_probe.gd", {
					"weapon": { "type": "string", "default": "" },
					"walk_frames": { "type": "integer", "minimum": 0, "default": 120 },
					"look_px": { "type": "number", "default": 300.0 },
					"burst_frames": { "type": "integer", "minimum": 1, "default": 20 },
					"output_dir": { "type": "string", "default": "" },
				}, [], true, true, 300_000),
		ProbeDef.make("hud_killfeed",
				"The message-feed screenshot (D-HUD-23): three real gametext Canned Msg "
				+ "lines through the engine formatters and HudOverlay.push_feed_line with "
				+ "the witnessed colors, captured over the loaded mission.",
				RUNTIME + "hud_killfeed_probe.gd", {
					"output_dir": { "type": "string", "default": "" },
				}, [], true, true, 300_000),
		ProbeDef.make("vm_bone_dump",
				"The first-person viewmodel bone dump: equip `weapon`, write the rig's "
				+ "placement inputs and every bone's camera-relative transform, then the "
				+ "full-clip sweep (every frame of every anim_wpn_idle/anim_wpn_reload "
				+ "variant on both parts) for the retail FP bone-builder comparison.",
				RUNTIME + "vm_bone_dump_probe.gd", {
					"weapon": { "type": "string", "default": "WPN_M16BURST" },
					"output_dir": { "type": "string", "default": "" },
				}, [], true, true, 600_000),
		ProbeDef.make("weapon_round",
				"The weapon-round visual probe: the adjudicated crosshair protocol "
				+ "(D-HUD-9/10), the SIGHTS scope card and unscope-on-move through the real "
				+ "input path (the armory at the spawn tents when in zone); with `fire` the "
				+ "fire-chain diagnostic (FSM view + audio bank per ten frames, the reload "
				+ "variant ring), with `animtrace` the per-frame viewmodel playhead trace.",
				RUNTIME + "weapon_round_probe.gd", {
					"rig_weapon": { "type": "string", "default": "WPN_Barret" },
					"card_weapon": { "type": "string", "default": "WPN_RPG" },
					"fire": { "type": "boolean", "default": false },
					"weapon": { "type": "string", "default": "" },
					"animtrace": { "type": "boolean", "default": false },
					"output_dir": { "type": "string", "default": "" },
				}, [], true, true, 900_000),
		ProbeDef.make("runtime_root_window",
				"The embedded game view (GameRuntimeRoot, the debug windowed startup) on the "
				+ "live process: the game runs inside one always-updating SubViewport, the "
				+ "real ImGui context attached, the tools workspace hides only the direct "
				+ "composite while the shared texture keeps rendering at the Game window's "
				+ "size, a window resize reaches the viewport, and F3 through the Game texture "
				+ "closes the workspace; refuses a direct-runtime fallback.",
				RUNTIME + "runtime_root_window_probe.gd", {}, [], true, false, 120_000),
		ProbeDef.make("entity_pick",
				"The debug pick -> F3 Entities selection path on the live process: the "
				+ "crosshair pick and a synthetic click at the nearest AI row's projected "
				+ "viewport-local point both make DevTools.selected_entity_handle that row; "
				+ "then the per-entity items.def attrib override (nodismember) round-trips "
				+ "through the seam the window's checkbox and game_debug share; captures the "
				+ "workspace.",
				RUNTIME + "entity_pick_probe.gd", {}, [], true, true, 120_000),
		ProbeDef.make("window_fullscreen",
				"The F11 policy (WindowState) on the live process: windowed -> fullscreen -> "
				+ "windowed through the key handler's static, proving each state presents a "
				+ "lit root frame and, in the embedded game view, that the game viewport "
				+ "follows the window size with its own frame lit; a black frame fails.",
				RUNTIME + "window_fullscreen_probe.gd", {
					"mode": { "type": "string", "enum": ["fullscreen", "exclusive", "resize"],
							"default": "fullscreen" },
				}, [], true, false, 120_000),
		ProbeDef.make("parity_joiner_ready",
				"Wait until this --lan-join joiner reaches `readiness_mode` (in_match: a "
				+ "live local player in the InMatch phase, the default deployment pick sent "
				+ "with `auto_deploy`; deploy_hold: the pick pending on a granted handle with "
				+ "the DEATH screen presented) and return the readiness witness the parity "
				+ "runner classifies; published as progress every heartbeat while waiting.",
				NET + "parity_joiner_ready_probe.gd", {
					"readiness_mode": { "type": "string", "enum": ["in_match", "deploy_hold"],
							"default": "in_match" },
					"auto_deploy": { "type": "boolean", "default": false },
					"exercise_motion": { "type": "boolean", "default": false },
					"run_id": { "type": "string", "minLength": 1 },
					"topology": { "type": "string", "enum": ["RO", "OO"] },
				}, ["run_id", "topology"], false, false, 250_000),
		ProbeDef.make("parity_joiner_motion",
				"The joiner's walk/strafe/turn witness inside the runner's steady window: "
				+ "pre-roll, then W 1.8 s / D 1.2 s / A 0.9 s with the look fed in twenty "
				+ "samples per phase; the completed exercise (gate metadata echoed) is the "
				+ "verdict's witness.",
				NET + "parity_joiner_motion_probe.gd", {
					"run_id": { "type": "string", "minLength": 1 },
					"topology": { "type": "string", "enum": ["RO", "OO"] },
					"steady_started_utc": { "type": "string", "minLength": 1 },
					"readiness_mode": { "type": "string", "enum": ["in_match", "deploy_hold"],
							"default": "in_match" },
					"auto_deploy": { "type": "boolean", "default": false },
				}, ["run_id", "topology", "steady_started_utc"], false, true, 60_000),
		ProbeDef.make("parity_joiner_state",
				"This joiner's readiness witness right now (one synchronous read): the "
				+ "runner's initial/final snapshots around a steady window, carrying the "
				+ "deployment pick and motion exercise the process witnessed earlier.",
				NET + "parity_joiner_state_probe.gd", {
					"readiness_mode": { "type": "string", "enum": ["in_match", "deploy_hold"],
							"default": "in_match" },
					"auto_deploy": { "type": "boolean", "default": false },
					"exercise_motion": { "type": "boolean", "default": false },
				}, [], false, false, 30_000),
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
