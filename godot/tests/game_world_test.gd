extends GutTest

const WORLD_TEST_ROOT := "game_world_test"
const ArmoryPresenter := preload("res://game/world/armory_presenter.gd")


func after_each() -> void:
	for staged_dir in _staged_dirs:
		TestFs.remove_dir_recursive(staged_dir)
	_staged_dirs.clear()
	TestFs.remove_dir_recursive(OS.get_cache_dir().path_join(WORLD_TEST_ROOT))


# (The Node runtime/sim doubles that used to live here — TransportRuntimeStub,
# ProfilingRuntimeStub, FxRuntimeStub, ItemPoseRuntimeStub, the anchor/blink/
# occlusion/joiner stubs — are gone: GameWorld._runtime is typed MissionRoot
# and MissionRoot._sim is typed Simulation, so every runtime-consuming
# test now boots the REAL stack through the public load path. The presentation
# doubles followed (ADR 0043 rule 11): the viewmodel/prewarm placer stubs and
# their GameWorld harnesses, the FxWorldStub/ImpactAudioStub recording sinks,
# the ItemFxDirectorProbe/ItemFxGameWorldHarness pair and the warm-pass stubs
# are replaced by the packaged world booted over staged roots (WorldFixture)
# and read through EffectWorld.get_debug_group_report,
# MissionAudio.recent_fired_soundsets and the world's typed local-player seams.)

# The WorldFixture roots this file staged (removed in after_each).
var _staged_dirs: Array[String] = []


# Register a staged WorldFixture root for after_each cleanup.
func _staged(root_dir: String) -> String:
	_staged_dirs.append(root_dir)
	return root_dir


# The committed minimal pack (the default root of every bare mission load).
func _minimal_assets_dir() -> String:
	return ProjectSettings.globalize_path(WorldFixture.MINIMAL_ASSETS_DIR)


func _append_to_file(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.READ_WRITE)
	assert_not_null(file, "the staged root carries %s to append to" % path.get_file())
	if file == null:
		return
	file.seek_end(0)
	file.store_string(text)
	file.close()


# --- Real item-fx fixtures (ADR 0034 / ADR 0043 rule 11): the item database is
# the staged root's items.def parsed by the REAL placer, the models are the
# committed synthetic .3di set staged under the graphics the rows name, and the
# per-item effects are attached by the world's OWN ItemEffectDirector during the
# mission load (reattach from on_effect_world_started, replayed by the warm
# pass). mount.3di authors the MFlash01 user point but carries a live PANM
# track, so it always places as an individual ObjectModel (the node-attached
# leg); gun.3di authors MFlash01 and batches (the static matched-anchor leg);
# shed.3di authors no such point and batches (the static origin-fallback leg).
# The first-16 mask RULE itself is native and pinned by the threedi
# user-point-mask ctest.
static var _fx_data_cache: Dictionary = {}

const FX_MOUNT_GRAPHIC := "FxMount"  # mount.3di: MFlash01 + live PANM -> individual node
const FX_GUN_GRAPHIC := "FxGun"  # gun.3di: MFlash01, inert -> static population
const FX_SHED_GRAPHIC := "FxShed"  # shed.3di: no MFlash01, inert -> static population
# A long-lived fixture effect (60 s emit) for the persistent item attaches and
# a short one for the origin-fallback rows, so each row identifies itself by
# name in the effect world's report.
const FX_PERSISTENT_EFFECT := "Buildup"
const FX_FALLBACK_EFFECT := "synth_dust"


func _fx_object_data(fixture_dir: String, model_file: String) -> ObjectData:
	var key := fixture_dir + "/" + model_file
	if _fx_data_cache.has(key):
		return _fx_data_cache[key]
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(fixture_dir)), OK)
	var data := ObjectData.new()
	assert_eq(data.open_from_resource_root(root, model_file), OK,
			"%s loads for the item-fx anchor fixtures" % model_file)
	_fx_data_cache[key] = data
	return data


func _fx_anchor_data() -> ObjectData:
	return _fx_object_data("res://../fixtures/threedi/synth", "mount.3di")


func _fx_gun_data() -> ObjectData:
	return _fx_object_data("res://../fixtures/threedi/synth", "gun.3di")


# The MFlash01 point's index on a real model (mount / gun).
func _fx_anchor_index_on(data: ObjectData) -> int:
	var mask := int(data.get_user_point_bone_mask("MFlash01"))
	assert_gt(mask, 0, "the model authors the MFlash01 user point in the first 16")
	for i in range(16):
		if (mask & (1 << i)) != 0:
			return i
	return -1


# The anchor point's index/info on the real model (MFlash01 on mount).
func _fx_anchor_index() -> int:
	return _fx_anchor_index_on(_fx_anchor_data())


func _fx_anchor_info() -> ModelUserPoint:
	return _fx_anchor_data().get_user_point_info(_fx_anchor_index())


func _fx_gun_anchor_info() -> ModelUserPoint:
	return _fx_gun_data().get_user_point_info(_fx_anchor_index_on(_fx_gun_data()))


# One items.def row for the staged item-fx root: {id, type (default object),
# graphic (default FxMount), anim_def (optional), attribs (optional token
# string), effect + userpoint (optional)}. CRLF like the shipped file: retail's
# .def parser stops at a bare LF.
func _fx_item_row(row: Dictionary) -> String:
	var id := int(row.get("id", 0))
	var text := "\r\nbegin \"fx item %d\"\r\n  id %d\r\n  type %s\r\n  graphic %s\r\n" % [
			id, id, String(row.get("type", "object")),
			String(row.get("graphic", FX_MOUNT_GRAPHIC))]
	var anim_def := String(row.get("anim_def", ""))
	if not anim_def.is_empty():
		text += "  anim_def %s\r\n" % anim_def
	var attribs := String(row.get("attribs", ""))
	if not attribs.is_empty():
		text += "  attrib: %s\r\n" % attribs
	var effect := String(row.get("effect", ""))
	if not effect.is_empty():
		text += "  particlefx %s %s\r\n" % [effect, String(row.get("userpoint", ""))]
	return text + "end\r\n"


# Stage the minimal pack plus the .ptl catalog, the three item-fx models under
# their graphic names, and the authored item rows appended to items.def.
func _stage_item_fx_fixture(name: String, rows: Array) -> String:
	var root_dir := _staged(WorldFixture.stage_minimal_root(name))
	WorldFixture.stage_effects(root_dir)
	for pair in [
		["mount.3di", FX_MOUNT_GRAPHIC],
		["gun.3di", FX_GUN_GRAPHIC],
		["shed.3di", FX_SHED_GRAPHIC],
	]:
		assert_eq(DirAccess.copy_absolute(
				ProjectSettings.globalize_path("res://../fixtures/threedi/synth/" + pair[0]),
				root_dir.path_join(pair[1] + ".3di")), OK)
	var text := ""
	for row_v in rows:
		text += _fx_item_row(row_v as Dictionary)
	_append_to_file(root_dir.path_join("items.def"), text)
	return root_dir


# The effect world's report rows attached to `node` (owner keys
# "itemfx:<instance id>:<point index | origin>"), live ones only unless
# `include_detached`; hidden groups only with `include_hidden`.
func _fx_rows_for_node(world: GameWorld, node: Node, include_detached := false,
		include_hidden := false) -> Array:
	var out: Array = []
	var prefix := "itemfx:%d:" % node.get_instance_id()
	for row_v in world.get_effect_world().get_debug_group_report(include_hidden):
		var row: EffectGroupReport = row_v
		var owner: Variant = row.owner_key
		if owner is String and String(owner).begins_with(prefix) \
				and (include_detached or not row.detached):
			out.append(row)
	return out


# The world-bound (unowned) report rows, optionally only those named `effect`
# (the static item effects and the impact transients spawn unowned).
func _fx_unowned_rows(world: GameWorld, effect := "", include_hidden := false) -> Array:
	var out: Array = []
	for row_v in world.get_effect_world().get_debug_group_report(include_hidden):
		var row: EffectGroupReport = row_v
		if row.owner_key != null:
			continue
		if effect.is_empty() or row.name == effect:
			out.append(row)
	return out


# The placed ObjectModel a mission record resolves to (the runtime's index).
func _placed_node(world: GameWorld, placed: Dictionary) -> ObjectModel:
	var bms_id := int(placed.get("bms_id", 0))
	assert_gt(bms_id, 0, "the authored entity carries a BMS id")
	var node := world.get_runtime().get_entity_index().resolve_single(bms_id) as ObjectModel
	assert_not_null(node, "the authored entity placed a real ObjectModel")
	return node


# One vehicle_control_* lifecycle edge as Simulation.drain_effects emits it,
# addressed at a placed record (net id, BMS id, packed spawn origin).
func _control_effect(kind: String, net_id: int, placed: Dictionary) -> MissionEffect:
	return MissionEffect.make(kind, net_id, int(placed.get("bms_id", 0)),
			int(Simulation.spawn_origin_pack(
					int(placed.get("kind", MissionData.KIND_ITEM)),
					int(placed.get("index", -1)))))


# A wire-spawned presentation node the way the present pass's wire callback
# hands one to the world's director (on_wire_node_spawned): a real ObjectModel
# over the mount.3di anchor fixture, parented under the world FIRST
# (set_object_data builds render children against the live global transform),
# carrying the wire row's EntityRef.
func _fx_wire_node(world: GameWorld, ref: EntityRef) -> ObjectModel:
	var model := ObjectModel.new()
	world.add_child(model)
	model.set_object_data(_fx_anchor_data())
	model.entity_ref = ref
	return model


# The staged ammo.def for the impact-routing tests: the minimal fixture rows
# plus authored effects_table blocks, so a real terrain hit resolves a real
# per-surface impact row through the witnessed bake (world/ammo_table.h). Every
# plausible minimal-terrain surface tag carries the same pair so the assert is
# surface-agnostic; the effect column names a synthetic .ptl effect the staged
# catalog carries (synth_dirt_hit) so the REAL effect world interns a definition
# for it; AM_SOUNDONLY authors 'none' effects (sound-only rows).
const IMPACT_EFFECT := "synth_dirt_hit"
const IMPACT_SOUND := "imp_bullet_dirt"
const SOUND_ONLY_SOUND := "imp_gren_dirt"
const IMPACT_AMMO_DEF := """
ammo AT_NULL
	velocity            0
	max_age             0
	drag                1
	min_damage          0
	max_damage          0
end

ammo AM_556MM
	velocity            3000
	max_age             300
	drag                1
	weight_in_grains    62
	min_damage          25
	max_damage          40
	penetration_impact  100
	light_move          6.0 128 120 80
	light_impact        10.0 255 192 96 0.2
	effects_table
		dirt          synth_dirt_hit    imp_bullet_dirt   15
		grass         synth_dirt_hit    imp_bullet_dirt   15
		snow          synth_dirt_hit    imp_bullet_dirt   15
		cement        synth_dirt_hit    imp_bullet_dirt   15
		sand          synth_dirt_hit    imp_bullet_dirt   15
		packeddirt    synth_dirt_hit    imp_bullet_dirt   15
		stone         synth_dirt_hit    imp_bullet_dirt   15
		mud           synth_dirt_hit    imp_bullet_dirt   15
		water         synth_dirt_hit    imp_bullet_dirt   15
		uwaterdeep    synth_dirt_hit    imp_bullet_dirt   15
		uwatershallow synth_dirt_hit    imp_bullet_dirt   15
		uwatersurface synth_dirt_hit    imp_bullet_dirt   15
	end
end

ammo AM_SOUNDONLY
	velocity            3000
	max_age             300
	drag                1
	min_damage          1
	max_damage          1
	effects_table
		dirt          none    imp_gren_dirt   15
		grass         none    imp_gren_dirt   15
		snow          none    imp_gren_dirt   15
		cement        none    imp_gren_dirt   15
		sand          none    imp_gren_dirt   15
		packeddirt    none    imp_gren_dirt   15
		stone         none    imp_gren_dirt   15
		mud           none    imp_gren_dirt   15
		water         none    imp_gren_dirt   15
		uwaterdeep    none    imp_gren_dirt   15
		uwatershallow none    imp_gren_dirt   15
		uwatersurface none    imp_gren_dirt   15
	end
end
"""

# A render-frame delta that always banks at least one 62.5 Hz logic tick
# (TICK_DT itself can round to zero ticks in the native accumulator).
const ONE_TICK_DELTA := 0.02


# Stage the minimal fixture over the synthetic Tmap terrain plus the staged
# impact ammo, the .ptl catalog its effects intern from, and a sound bank
# carrying the two impact sets. The minimal mnml map is flat, so rounds would
# never ground on it; Tmap carries the relief the round flight can actually hit.
func _stage_impact_fixture(name: String) -> String:
	var root_dir := _staged(WorldFixture.stage_minimal_root(name, true,
			{"ammo.def": IMPACT_AMMO_DEF}))
	WorldFixture.stage_effects(root_dir)
	WorldFixture.stage_sound_bank(root_dir,
			PackedStringArray([IMPACT_SOUND, SOUND_ONLY_SOUND]))
	return root_dir


# The staged harness item for KIND_BUILDING tests. The shipped assets/items.def
# carries only what the minimal mission places or the engine spawns by fixed id,
# so the staged root appends its own catalogue entry for the fixture model the
# stagers copy in as GuardTwr1.3di. CRLF: retail's .def parser stops at a bare LF.
const BUILDING_ITEM_DEF := "\r\nbegin \"Guard Tower\"\r\n  id 102001\r\n  type building\r\n  graphic GuardTwr1\r\n  sid guardtwr1\r\n  anim_def GuardTwr1\r\n  husk GuardTwr1X\r\n  hp 5000\r\nend\r\n"


func _append_building_item(root_dir: String) -> void:
	_append_to_file(root_dir.path_join("items.def"), BUILDING_ITEM_DEF)


# Stage the minimal fixture plus the house.3di collision fixture as item
# 102001's GuardTwr1 graphic, so authored KIND_BUILDING entities place a REAL
# ObjectModel and enter the sim's real collision/occlusion world.
func _stage_building_fixture(name: String) -> String:
	var root_dir := _staged(WorldFixture.stage_minimal_root(name))
	assert_eq(DirAccess.copy_absolute(
			ProjectSettings.globalize_path("res://../fixtures/threedi/synth/house.3di"),
			root_dir.path_join("GuardTwr1.3di")), OK)
	_append_building_item(root_dir)
	return root_dir


func _stage_lit_building_fixture(name: String) -> String:
	var root_dir := _staged(WorldFixture.stage_minimal_root(name))
	assert_eq(DirAccess.copy_absolute(
			ProjectSettings.globalize_path("res://../fixtures/threedi/synth/shed.3di"),
			root_dir.path_join("GuardTwr1.3di")), OK)
	_append_building_item(root_dir)
	return root_dir


# Tmap supplies the terrain-normal table inputs (its ramp cell) and house.3di keeps
# the SSN owner on the real placed-object path used by the routing assertion.
# [orig: WacScript_SpawnEffectAtSsnEntity @0x4F23A0 resolves the SSN entity,
# then reads the terrain normal for its grid cell before creating the emitter.]
func _stage_building_terrain_fixture(name: String) -> String:
	var root_dir := _stage_impact_fixture(name)
	assert_eq(DirAccess.copy_absolute(
			ProjectSettings.globalize_path("res://../fixtures/threedi/synth/house.3di"),
			root_dir.path_join("GuardTwr1.3di")), OK)
	_append_building_item(root_dir)
	return root_dir


# The missing-terrain/env reason is join-aware: a wire-header join (no local
# .bms — the host streamed the identity) names the stream and the mounted/
# installed expansions so a live retail-server punt reads as an install gap.
func test_wire_header_missing_asset_reason_names_the_install() -> void:
	var world := WorldFixture.make_world(self)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../assets")), OK)
	assert_eq(world.missing_mission_asset_reason("dvi.trn", "x.bms", root, false),
			"dvi.trn (from x.bms) not found in %s" % root.get_root_dir(),
			"a local-file load keeps the historical reason text")
	var wire_reason := String(
			world.missing_mission_asset_reason("dvi.trn", "HOSTMAP.BMS", root, true))
	assert_string_contains(wire_reason, "host's streamed mission HOSTMAP.BMS",
			"a wire-header join names the host's stream, not a bad local file")
	assert_string_contains(wire_reason, "mounted: base game",
			"the reason names the mounted data set")
	assert_string_contains(wire_reason, "installed:",
			"the reason names the installed expansion set")


# The env presenters hand their clocks to GameFramePipeline at _ready: their
# idle callbacks stay off under a live world, or their cost leaves the
# measured WORLD_ENV_NODES/WORLD_WATER legs for the unmeasured
# process-callbacks window and the advance races the camera placement
# (game_world.gd's _env_presenters_world_driven handoff; D-RORD-8's ordering).
func test_env_presenters_are_pipeline_clocked_not_self_clocked() -> void:
	var world := WorldFixture.make_world(self)
	for presenter_name in ["Weather", "SkyDome", "Celestial", "Water", "SunShadow"]:
		var presenter := world.get_node_or_null(NodePath(presenter_name)) as Node
		assert_not_null(presenter, "%s exists under the world" % presenter_name)
		if presenter != null:
			assert_false(presenter.is_processing(),
					"%s must be pipeline-clocked, never self-clocked" % presenter_name)
			assert_false(presenter.is_physics_processing(),
					"%s must not self-clock on the physics tick either" % presenter_name)


func test_manual_perf_probe_routes_through_the_public_runtime_gate() -> void:
	var world := WorldFixture.boot_minimal(self)
	var sim := world.get_sim()
	assert_not_null(sim)
	assert_false(bool(sim.get_runtime_perf_counters().get("runtime_profiling_enabled", true)),
			"a fresh mission runtime starts with the native probe timer off")

	world.set_perf_probe_enabled(true)
	assert_true(bool(sim.get_runtime_perf_counters().get("runtime_profiling_enabled", false)),
			"GameWorld forwards consumer intent through MissionRoot's public seam")
	world.set_perf_probe_enabled(false)
	assert_false(bool(sim.get_runtime_perf_counters().get("runtime_profiling_enabled", true)),
			"disabling the probe releases the native timer through the same seam")


func test_perf_counters_estimate_the_retained_instance_uniform_geometry() -> void:
	var world := WorldFixture.make_world(self)
	var idle: Dictionary = world.get_runtime_perf_counters().get(
			"instance_uniform_geometry_estimate", {})
	assert_eq(int(idle.get("budget", 0)),
			int(ProjectSettings.get_setting(
					"rendering/limits/global_shader_variables/buffer_size", 0)) / 16,
			"the budget is the project's buffer_size in 16-value geometry slots")
	assert_eq(int(idle.get("total", -1)),
			int(idle.get("foliage_pool", 0)) + int(idle.get("static_populations", 0))
			+ int(idle.get("object_geometry", 0)),
			"the total is the sum of the three shell-owned terms")

	# An authored building places a real ObjectModel (house.3di as GuardTwr1):
	# its retained surface instances are the object term of the estimate.
	var root_dir := _stage_building_fixture("instance_uniform_estimate")
	var before_load := int(ObjectModel.get_live_geometry_instance_count())
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				mission.add_entity(
						MissionData.KIND_BUILDING, 102001, Vector3(16, 24, 4), Vector3.ZERO)), OK)
	var loaded: Dictionary = world.get_runtime_perf_counters().get(
			"instance_uniform_geometry_estimate", {})
	var placement: Dictionary = world.get_runtime_perf_counters().get(
			"mission_placement", {})
	assert_eq(int(loaded.get("static_populations", -1)),
			int(placement.get("batches", 0)) + int(placement.get("static_shadow_batches", 0)),
			"static populations count the placer's visible batches and shadow twins")
	assert_gt(int(loaded.get("object_geometry", 0)), before_load,
			"the placed building's ObjectModel retains instance-uniform geometry")
	assert_eq(int(loaded.get("object_geometry", -1)),
			int(ObjectModel.get_live_geometry_instance_count()),
			"the object term is the live ObjectModel surface-instance count")
	assert_lte(int(loaded.get("total", 0)), int(loaded.get("budget", 0)),
			"the minimal mission stays inside the instance-uniform budget")
	world.unload()
	await get_tree().process_frame
	assert_eq(int(ObjectModel.get_live_geometry_instance_count()), before_load,
			"unloading the mission retires every surface instance it counted")


func test_tick_gates_the_runtime_on_its_transport() -> void:
	# The game shell's tick must respect inmatch::Session state - the debug
	# overlay's Pause/Step work on a live mission BECAUSE this gate exists
	# (before it, play()/pause() were inert in the game).
	var world := WorldFixture.boot_minimal(self)
	var runtime: MissionRoot = world.get_runtime()
	var sim := world.get_sim()
	assert_not_null(sim)

	runtime.pause()
	var baseline := int(sim.get_logic_tick())
	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	assert_eq(int(sim.get_logic_tick()), baseline, "a paused runtime never ticks")
	runtime.play()
	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	assert_eq(int(sim.get_logic_tick()), baseline + 1,
			"a playing runtime ticks once per render frame")
	runtime.pause()
	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	assert_eq(int(sim.get_logic_tick()), baseline + 1, "pausing stops it again")


# Drive a REAL debug-spawned round through the playing world until its impact
# row lands in the real presentation sinks (a live effect-world group or a
# fired soundset). Returns the number of world frames run.
func _tick_until_impact(world: GameWorld) -> int:
	for frame in range(30):
		world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
		if not world.get_mission_audio().recent_fired_soundsets().is_empty() \
				or world.get_effect_world().live_group_count() > 0:
			return frame + 1
	return 30


func test_round_light_move_rows_reach_world_selected_output() -> void:
	var root_dir := _stage_impact_fixture("round_light_move")
	var world := WorldFixture.make_world(self)
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				assert_true(mission.set_header_string("terrain", "Tmap"))), OK)
	var camera := Camera3D.new()
	camera.position = Vector3(16, 300, -16)
	world.add_child(camera)
	camera.make_current()
	assert_gte(int(world.get_sim().debug_spawn_round(
			camera.position, Vector3.RIGHT, "AM_556MM")), 0)
	world.tick(camera.position, camera.global_transform, ONE_TICK_DELTA)
	world.device_frame().render_light_frame()
	var report := world.get_effect_light_report()
	var report_contract: Variant = report
	assert_true(report_contract is EffectLightReport,
			"GameWorld preserves the typed effect-light report contract")
	assert_eq(report.live, 1,
			"Simulation.get_round_glow_rows creates the in-flight light")
	assert_eq(report.selected, 1,
			"the terrain-disabled round glow remains eligible for object output")
	world.unload()


func test_round_impacts_route_generic_transient_and_audio_legs() -> void:
	var root_dir := _stage_impact_fixture("impact_generic")
	var world := WorldFixture.make_world(self)
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				assert_true(mission.set_header_string("terrain", "Tmap"))), OK)
	var effects := world.get_effect_world()
	var audio := world.get_mission_audio()
	assert_eq(effects.live_group_count(), 0, "the load-time warm leaves no live group behind")

	var sim := world.get_sim()
	assert_gte(int(sim.debug_spawn_round(
			Vector3(16, 60, -16), Vector3.DOWN, "AM_556MM")), 0,
			"the real flight sim accepts the staged rifle round")
	_tick_until_impact(world)
	# The fixed ticks the presenting frame banked (a 0.02 s frame banks one,
	# every fourth frame two): the same-frame particle advance ran once per tick.
	var ticks_in_frame := int(world.get_runtime().get_perf_counters().ticks)

	var rows := effects.get_debug_group_report()
	assert_eq(rows.size(), 1,
			"one real terrain hit presents exactly one impact transient")
	var first_id := 0
	if rows.size() == 1:
		var spawn: EffectGroupReport = rows[0]
		first_id = int(spawn.id)
		var transform: Transform3D = spawn.transform
		assert_eq(spawn.name, IMPACT_EFFECT,
				"the surface row's authored .ptl effect reaches the effect world")
		assert_lt(transform.basis.z.distance_to(Vector3.DOWN), 0.05,
				"the transient carries the real incoming flight direction")
		# The engine stamps imp.tick DURING the producing tick and bumps
		# logic_tick before the shell's fixed-tick drain runs, so a same-frame
		# impact reads age 1 (one counter bump), never real catch-up aging: the
		# fresh emitters have aged that one initial tick plus the presenting
		# frame's particle advances, and nothing more.
		for emitter_v in spawn.emitters:
			var age := (emitter_v as EffectEmitterReport).age
			assert_lte(age, float(ticks_in_frame + 1) * Simulation.tick_dt() + 0.0001,
					"an impact presented in its production frame carries no catch-up aging")
			assert_gte(age, Simulation.tick_dt() - 0.0001,
					"the presenting tick's particle advance aged the fresh transient")
		assert_eq(spawn.render_domain,
				EffectScene.RENDER_DOMAIN_WORLD)
		assert_gt(int(spawn.source_tick), 0,
				"the row carries its production tick for catch-up chronology")
	var fires := audio.recent_fired_soundsets()
	assert_eq(fires.size(), 1)
	if fires.size() == 1 and rows.size() == 1:
		var fired: FiredSoundset = fires[0]
		assert_eq(fired.set_name, IMPACT_SOUND,
				"impact audio fires the same surface row's soundset")
		assert_true(fired.played, "the staged bank carries the set, so the one-shot plays")
		var impact_transform: Transform3D = (rows[0] as EffectGroupReport).transform
		assert_eq(fired.position, impact_transform.origin,
				"impact audio shares collision presentation with the visual transient")
	assert_eq(world.get_effect_light_report().live, 1,
			"the real light_impact dictionary reaches the EffectLightDirector")
	var camera := Camera3D.new()
	camera.position = Vector3(16, 60, -16)
	world.add_child(camera)
	camera.make_current()
	world.device_frame().render_light_frame()
	assert_eq(world.get_effect_light_report().selected, 1,
			"the impact flash reaches camera-global object output")

	# Drained rows cannot accumulate: later frames re-drain an empty queue.
	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	var later := effects.get_debug_group_report()
	assert_lte(later.size(), 1,
			"resolved impact rows cannot accumulate between presentation frames")
	for row_v in later:
		assert_eq(int((row_v as EffectGroupReport).id), first_id,
				"no second transient appears once the queue has drained")
	assert_eq(audio.recent_fired_soundsets().size(), 1)
	world.unload()


func test_round_impacts_route_sound_only_without_a_particle() -> void:
	var root_dir := _stage_impact_fixture("impact_sound_only")
	var world := WorldFixture.make_world(self)
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				assert_true(mission.set_header_string("terrain", "Tmap"))), OK)

	assert_gte(int(world.get_sim().debug_spawn_round(
			Vector3(16, 60, -16), Vector3.DOWN, "AM_SOUNDONLY")), 0)
	_tick_until_impact(world)

	assert_true(world.get_effect_world().get_debug_group_report().is_empty(),
			"a 'none' effect column must not spawn an impact particle")
	var fires := world.get_mission_audio().recent_fired_soundsets()
	assert_eq(fires.size(), 1)
	if fires.size() == 1:
		assert_eq((fires[0] as FiredSoundset).set_name, SOUND_ONLY_SOUND,
				"the sound-only surface row retains its authored soundset")


func test_fixed_tick_orders_weapon_and_impact_before_particle_advance() -> void:
	# The strict call ledger ["weapon", "impact", "advance"] the recording
	# EffectWorld double kept has no public twin. The same contract is pinned
	# through its observable consequences on the real objects: the fixed-tick
	# drain hands EVERY tick's weapon batch to the world's local view
	# presenter FIRST (its fixed_weapon_batches_consumed read seam advances
	# once per logic tick), no impact group exists before the presenting
	# frame's drains ran, the group the drain then presents rides that drain's
	# source tick, and its emitters carry exactly the particle advances that
	# ran AFTER the spawn (initial tick + the presenting frame's advances), so
	# the source tick was presented chronologically before its particle pass.
	var root_dir := _stage_impact_fixture("impact_order")
	var world := WorldFixture.make_world(self)
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				assert_true(mission.set_header_string("terrain", "Tmap"))), OK)
	var effects := world.get_effect_world()
	var sim := world.get_sim()
	# A REAL presenter (no camera: the drain seam is what is under test) binds
	# itself as the world's local view presenter from setup to teardown.
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, null)
	assert_eq(world.local_view_presenter(), presenter,
			"setup binds the presenter as the world's local view presenter")
	var tick_at_attach := int(sim.get_logic_tick())

	assert_gte(int(sim.debug_spawn_round(
			Vector3(16, 60, -16), Vector3.DOWN, "AM_556MM")), 0)
	# Frame until the hit presents, remembering the drain count and the live
	# groups seen at the start of the presenting frame.
	var consumed_before_frame := presenter.fixed_weapon_batches_consumed()
	var groups_before_frame := effects.live_group_count()
	var presented := false
	for _frame in range(30):
		consumed_before_frame = presenter.fixed_weapon_batches_consumed()
		groups_before_frame = effects.live_group_count()
		world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
		if effects.live_group_count() > 0:
			presented = true
			break
	assert_true(presented, "the terrain hit presented its transient")

	var rows := effects.get_debug_group_report()
	assert_eq(rows.size(), 1, "the terrain hit presented its transient")
	var consumed := presenter.fixed_weapon_batches_consumed()
	assert_gt(consumed, 0, "the weapon consumer runs from the fixed-tick drain")
	if rows.size() != 1 or not presented:
		presenter.teardown()
		return
	var spawn: EffectGroupReport = rows[0]
	assert_eq(consumed, int(sim.get_logic_tick()) - tick_at_attach,
			"every fixed tick's drain hands the presenter one weapon batch")
	assert_gt(consumed, consumed_before_frame,
			"the weapon consumer ran on the fixed tick that presented the impact")
	assert_eq(groups_before_frame, 0,
			"the weapon events of the presenting tick are consumed before its impact spawns")
	# The counter has already bumped once past the row's production tick when
	# the drain runs (see the generic routing test above): the drain that
	# presented the hit is the one that consumed weapon events at tick + 1.
	var presenting_tick := int(spawn.source_tick) + 1
	# Every fixed tick from the presenting one through the end of that frame
	# ran one particle advance after its drain.
	var advances_after_spawn := int(sim.get_logic_tick()) - presenting_tick + 1
	assert_gte(advances_after_spawn, 1,
			"the presenting tick lies inside the frame that presented the hit")
	for emitter_v in spawn.emitters:
		var age := (emitter_v as EffectEmitterReport).age
		assert_gte(age, float(advances_after_spawn) * Simulation.tick_dt() - 0.0001,
				"the presenting tick's particle advance ran after the impact spawned")
		# Same-frame drain: at most the one counter bump of initial age on top.
		assert_lte(age, float(advances_after_spawn + 1) * Simulation.tick_dt() + 0.0001,
				"a physical collision is visible in its production frame without catch-up aging")
	# teardown releases the seam: later drains reach no presenter.
	presenter.teardown()
	assert_null(world.local_view_presenter(),
			"teardown releases the world's local view presenter")


func test_fx2ssn_routes_position_owner_and_terrain_orientation() -> void:
	var root_dir := _stage_building_terrain_fixture("fx2ssn")
	var world := WorldFixture.make_world(self)
	var placed := {}  # mutated (merge), never reassigned: lambda captures copy locals
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				assert_true(mission.set_header_string("terrain", "Tmap"))
				placed.merge(mission.add_entity(
						MissionData.KIND_BUILDING, 102001, Vector3(6, 4, 5), Vector3.ZERO))), OK)
	var ssn := int(placed.get("bms_id", 0))
	assert_gt(ssn, 0, "the authored building carries a WAC/BMS-addressable SSN")
	var effects := world.get_effect_world()
	assert_true(effects.get_debug_group_report().is_empty(),
			"the loaded fixture starts with no live group")

	world.route_mission_effects([MissionEffect.make("fx2ssn", 0, ssn, 0, FX_FALLBACK_EFFECT)])
	var rows := effects.get_debug_group_report()
	assert_eq(rows.size(), 1)
	if rows.is_empty():
		return
	var spawn: EffectGroupReport = rows[0]
	assert_eq(spawn.owner_key, ssn,
			"the emitter handle is owned per SSN entity (a scripted re-trigger replaces it)")
	assert_eq(spawn.name, FX_FALLBACK_EFFECT)
	var transform: Transform3D = spawn.transform
	var spawn_pos := transform.origin
	assert_almost_eq(spawn_pos.x, 6.0, 0.01,
			"the SSN resolves to the real registry entity's Godot-space position")
	assert_almost_eq(spawn_pos.z, -4.0, 0.01,
			"mission (x, north, up) maps through the canonical frame")
	var terrain := world.get_terrain_data()
	assert_not_null(terrain)
	# This independently spells out the recovered centered raw-height
	# difference at the synthetic map's ramp cell (source 518,508).
	# [orig: Terrain_GenerateNormalMap @0x603210, diff scale @0x7C6950;
	# WacScript_SpawnEffectAtSsnEntity consumes that cell normal @0x4F23A0.]
	var expected := Vector3(
			terrain.get_height_world(spawn_pos + Vector3(-1, 0, 0))
					- terrain.get_height_world(spawn_pos + Vector3(1, 0, 0)),
			1.0,
			terrain.get_height_world(spawn_pos + Vector3(0, 0, -1))
					- terrain.get_height_world(spawn_pos + Vector3(0, 0, 1))).normalized()
	# The effect pose puts the authored forward along the group's Z axis.
	var orientation := transform.basis.z
	assert_lt(orientation.distance_to(expected), 0.00001,
			"fx2ssn receives the terrain cell's recovered surface normal")
	assert_gt(orientation.distance_to(Vector3.UP), 0.01,
			"the ramp witness cannot regress to the old UP placeholder")


func test_round_outcome_effects_pass_through_to_hud_consumers() -> void:
	# The round-outcome trio (win/lose/round_end) is produced sim-side [orig: the WAC
	# win/lose handlers @0x4ed4a0/@0x4ed3f0 + Server_ProcessRoundEnd @0x5164f0] and is
	# host presentation on this side: the router must pass every row through to
	# mission_effects (the HUD banner + the shell's end-of-mission flow consume there).
	var world := WorldFixture.boot_minimal(self)
	watch_signals(world)
	var rows := [
		MissionEffect.make("lose", 0, 0, 0, "STRMISC_KILLEDGREEN"),
		MissionEffect.make("win", 1),
		MissionEffect.make("round_end", 2),
	]
	# The runtime's drained batch is the router's only input: raise it through
	# the same signal MissionRoot emits once per tick.
	world.get_runtime().effects_drained.emit(rows)
	assert_signal_emitted_with_parameters(world, "mission_effects", [rows])


func test_load_world_requires_hardcoded_environment_in_global_root() -> void:
	var root := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join("missing_env_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root)
	# Pass the runtime archive gate so this fixture reaches the missing-environment contract.
	_write_pff(root.path_join("resource.pff"), [])
	_write_fixture_file(root.path_join("Tmap.trn"), "terrain_name \"Tmap\"\n")

	var world := WorldFixture.make_world(self)
	await get_tree().process_frame

	assert_eq(world.load_world(root), ERR_FILE_NOT_FOUND, "Runtime global root must contain full_00.env next to Tmap.trn.")


func test_packaged_scene_instantiates_with_intact_wiring() -> void:
	# game_world.tscn is the game shell's embeddable world. Pin the extraction:
	# every engine node is present and the intra-scene NodePaths survived the
	# move out of main_game.tscn.
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	assert_not_null(packed, "the packaged world scene loads")
	var world := packed.instantiate()
	add_child_autofree(world)
	assert_true(world is GameWorld, "the root carries the GameWorld script")
	for child_name in ["Terrain", "MissionEnvironment", "SkyDome", "Weather", "Water", "Celestial",
			"SunShadow", "SlotShadow"]:
		assert_not_null(world.get_node_or_null(child_name), "%s is in the packaged scene" % child_name)
	assert_not_null(world.get_node_or_null("Terrain/FoliageDispatcher"))
	assert_null(world.get_node_or_null("Terrain/TileOverlay"),
		"the removed editor overlay is not part of the runtime world")
	var terrain: Terrain = world.get_node("Terrain")
	assert_eq(terrain.environment_path, NodePath("../MissionEnvironment"), "terrain env path survived extraction")
	assert_eq(terrain.weather_path, NodePath("../Weather"), "terrain weather path survived extraction")
	var water: Water = world.get_node("Water")
	assert_eq(water.water_height, 0.0,
		"the retained scene must not invent water before ENV/TRN/BMS author it")
	assert_false(water.is_water_active())
	assert_eq(water.get_reflection_viewport().render_target_update_mode,
		SubViewport.UPDATE_DISABLED)
	water.water_height = 10.0
	assert_true(water.is_water_active(), "the authored height remains retained")
	assert_false(water.is_water_render_active(),
		"the packaged shell keeps retained water out of rendering before a load")
	assert_false(world.is_water_render_active(),
		"frame clear and occlusion use the lifecycle-aware water predicate")
	water.set_world_rendering_enabled(true)
	assert_true(world.is_water_render_active())
	water.set_world_rendering_enabled(false)


func test_unload_synchronously_retires_effect_light_state() -> void:
	var root_dir := _stage_lit_building_fixture("effect_light_unload")
	var world := WorldFixture.make_world(self)
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				mission.add_entity(MissionData.KIND_BUILDING, 102001,
						Vector3(6, 4, 5), Vector3.ZERO)), OK)
	assert_eq(world.get_effect_light_report().live, 1,
			"the loaded authored model hosts its point light")
	world.unload()
	var report := world.get_effect_light_report()
	assert_eq(report.live, 0,
			"unload retires the prior mission pool before queued node deletion")
	assert_eq(report.selected, 0,
			"unload synchronously clears point-light shader output")


func test_explicit_bms_zero_water_beats_nonzero_terrain() -> void:
	var root_dir := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join(
		"zero_water_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root_dir)
	for source_dir in [
		ProjectSettings.globalize_path("res://../fixtures/terrain/tmap"),
		ProjectSettings.globalize_path("res://../assets"),
	]:
		for file_name in DirAccess.get_files_at(source_dir):
			assert_eq(DirAccess.copy_absolute(
				source_dir.path_join(file_name), root_dir.path_join(file_name)), OK)

	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK)
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	world.set_resource_root(root)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, "mnml.bms"), OK)
	assert_true(mission.set_header_string("terrain", "Tmap"))
	assert_true(mission.set_header_string("environment", "mnml"))
	assert_true(mission.set_header_int("water_override", 0))
	assert_true(mission.set_header_flag(0x1, true))
	assert_true(mission.get_environment_overrides().has_water_height)

	assert_eq(world.load_mission_data(mission, "mnml.bms"), OK)
	var water := world.get_node("Water") as Water
	assert_eq(world.get_terrain_data().get_water_height(), 21,
		"fixture proves the lower-priority TRN has nonzero water")
	assert_eq(water.water_height, 0.0,
		"flagged BMS zero disables water and still beats TRN")
	assert_false(water.is_water_active())
	assert_eq(water.get_reflection_viewport().render_target_update_mode,
		SubViewport.UPDATE_DISABLED)


func test_wire_header_mission_uses_host_metadata_without_a_local_bms_body() -> void:
	var fixture_path := ProjectSettings.globalize_path(
			"res://../assets/mnml.bms")
	var full_bytes := FileAccess.get_file_as_bytes(fixture_path)
	assert_gt(full_bytes.size(), 616)
	var full_mission := MissionData.new()
	assert_eq(full_mission.open_file(fixture_path), OK)

	var wire_mission := MissionData.new()
	assert_eq(wire_mission.open_wire_header(full_bytes.slice(0, 616)), OK)
	assert_true(wire_mission.is_loaded())
	assert_true(wire_mission.is_wire_header_only())
	assert_eq(wire_mission.get_mission_name(), full_mission.get_mission_name())
	assert_eq(wire_mission.get_terrain_ref(), full_mission.get_terrain_ref())
	assert_eq(wire_mission.get_environment_ref(), full_mission.get_environment_ref())
	for kind in range(4):
		assert_eq(wire_mission.get_entity_count(kind), 0,
				"S2C 0x0B supplies metadata, never locally-authored body records")

	assert_ne(wire_mission.open_wire_header(full_bytes.slice(0, 615)), OK)
	assert_false(wire_mission.is_loaded(),
			"an inexact wire header cannot leave the previous host metadata live")
	assert_false(wire_mission.is_wire_header_only())

func test_armory_can_reuse_game_world_weapon_database_on_first_open() -> void:
	Strings.clear()
	# The retail weapon.mnu, weapon.def and string tables come from the
	# reference fixture set (docs/asset-gated-tests.md).
	var staged := {
		"mnu/jo_weapon.mnu": "weapon.mnu",
		"def/weapon.def": "weapon.def",
		"rtxt/menutxt.bin": "menutxt.BIN",
		"rtxt/gametext.bin": "gametext.bin",
	}
	for rel in staged:
		if RetailData.fixture(rel).is_empty():
			pending(RetailData.fixture_pending_text(rel))
			return
	var root_dir := _staged(WorldFixture.stage_minimal_root("first_armory_open"))
	for rel in staged:
		var target := root_dir.path_join(staged[rel])
		if FileAccess.file_exists(target):
			assert_eq(DirAccess.remove_absolute(target), OK)
		assert_eq(DirAccess.copy_absolute(RetailData.fixture(rel), target), OK)

	var world := WorldFixture.make_world(self)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK)
	world.set_resource_root(root)
	var loadout := PlayerSpawnLoadout.new()
	loadout.primary = "WPN_M4AUTO"
	loadout.accessory = "WPN_SATCHEL_CHARGE"
	loadout.player_class = 8
	world.set_local_player_spawn_loadout(loadout)
	assert_eq(world.load_mission("mnml.bms"), OK)

	var weapons: WeaponDatabase = world.get_weapon_database()
	assert_not_null(weapons, "first armory open lazily resolves weapon.def")
	if weapons == null:
		return
	assert_true(weapons.is_loaded())
	assert_gte(weapons.find_weapon("WPN_M4AUTO"), 0)
	assert_gte(weapons.find_weapon("WPN_SATCHEL_CHARGE"), 0)
	var sim := world.get_sim()
	var expected_names := ["WPN_M4AUTO", "WPN_SATCHEL_CHARGE"]
	var before_names: Array[String] = []
	for value in sim.get_local_player_loadout():
		before_names.append((value as WeaponKitEntry).name)
	assert_eq(before_names, expected_names,
		"the production world promoted the PLAYER_INFO-style canonical profile")

	var overlay := Control.new()
	add_child_autofree(overlay)
	overlay.size = Vector2(800, 600)
	var presenter := ArmoryPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world.armory_view(), null, overlay)
	assert_true(presenter.open(), "the production world catalog reaches first armory open")
	assert_not_null(overlay.get_node_or_null("ArmoryMenu"))
	var driver: MenuDriver = presenter.get_menu_driver()
	assert_not_null(driver)
	assert_gt(driver.selected_row(driver.widget_id("PRIMARY")), 0,
		"the current primary is not NONE on first visit")
	assert_gt(driver.selected_row(driver.widget_id("ACCESSORY")), 0,
		"the current satchel is not NONE on first visit")

	driver.widget_activated.emit(driver.widget_id("ACCEPT"), "ACCEPT")
	var after_names: Array[String] = []
	for value in sim.get_local_player_loadout():
		after_names.append((value as WeaponKitEntry).name)
	assert_eq(after_names, expected_names,
		"accepting the untouched first-open rows preserves the exact canonical kit")
	world.unload()
	Strings.clear()


func test_clear_color_environment_renders_the_witnessed_frame_clear() -> void:
	# _update_frame_clear_color() writes the witnessed frame clear into the
	# ClearColor Environment's background_color every frame - but the scene
	# resource decides whether that color ever renders. The Wave-1 scene shipped
	# background_mode = 2 (BG_SKY) with no Sky resource, which renders BLACK and
	# silently swallows the env-#21 clear consumer: a 1px black dome-rim seam in
	# ground views, a black band in aerial views. Pin the mode so it can't drift.
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	assert_not_null(packed, "the packaged world scene loads")
	var world := packed.instantiate()
	add_child_autofree(world)
	var clear := world.get_node_or_null("ClearColor") as WorldEnvironment
	assert_not_null(clear, "the ClearColor WorldEnvironment is in the packaged scene")
	if clear == null:
		return
	assert_not_null(clear.environment, "ClearColor carries an Environment resource")
	if clear.environment == null:
		return
	assert_eq(clear.environment.background_mode, Environment.BG_COLOR,
		"BG_COLOR renders background_color; BG_SKY with a null sky renders BLACK and silently swallows the witnessed frame clear [orig: Render_ProcessMainSceneFrame @ 0x5ca776..0x5ca792]")
	assert_eq(clear.environment.ambient_light_source, Environment.AMBIENT_SOURCE_DISABLED,
		"Godot ambient must never inject into the witnessed lighting model - all OpenNova materials light themselves; AMBIENT_SOURCE_BG would derive ambient from the clear color")


func test_hidden_world_suppresses_retained_terrain_and_restores_idle_frame_clear() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	var clear := world.get_node("ClearColor") as WorldEnvironment
	clear.environment = clear.environment.duplicate()
	var idle_clear := Color(0.01, 0.02, 0.03)
	clear.environment.background_color = idle_clear

	var camera := Camera3D.new()
	camera.position = Vector3(0, 71, 0)
	camera.current = true
	world.add_child(camera)
	add_child_autofree(world)
	world.set_playable(false)
	assert_eq(world.get_current_frame_clear_color(), idle_clear,
		"the scene-authored clear is the menu/loading baseline before a mission presents")

	var root_dir := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join(
		"presentation_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root_dir)
	for source_dir in [
		ProjectSettings.globalize_path("res://../fixtures/terrain/tmap"),
		ProjectSettings.globalize_path("res://../assets"),
	]:
		for file_name in DirAccess.get_files_at(source_dir):
			assert_eq(DirAccess.copy_absolute(
				source_dir.path_join(file_name), root_dir.path_join(file_name)), OK)

	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK)
	world.set_resource_root(root)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, "mnml.bms"), OK)
	assert_true(mission.set_header_string("terrain", "Tmap"))
	assert_true(mission.set_header_string("environment", "mnml"))
	assert_eq(world.load_mission_data(mission, "mnml.bms"), OK)
	await get_tree().process_frame
	await get_tree().process_frame
	# Since ADR 0033 R2 terrain presents from its frame leg, not a self-driven
	# _process — drive one world frame (the paused/legacy sequence runs the
	# terrain leg for a non-playing world).
	world.tick(camera.global_position, camera.get_global_transform())

	var terrain := world.get_node("Terrain") as Terrain
	var env := world.get_node("MissionEnvironment") as MissionEnvironment
	assert_gt(terrain.get_visible_patch_count(), 0,
		"the loaded fixture presents native RenderingServer terrain patches")
	var mission_clear := world.get_current_frame_clear_color()
	assert_ne(mission_clear, idle_clear,
			"the loaded mission replaces the scene-authored frame clear")
	var terrain_material: ShaderMaterial = terrain.get_terrain_material()
	var dry_terrain_fog_color: Vector3 = terrain_material.get_shader_parameter("u_fog_color")
	var dry_terrain_fog_end := float(terrain_material.get_shader_parameter("u_fog_end"))
	var dry_terrain_fog_type := int(terrain_material.get_shader_parameter("u_fog_type"))
	var dry_object_values: EnvLightValues = env.get_light_state().get_values()
	var dry_object_fog_color := dry_object_values.get_fog_color()
	var dry_object_fog_end := dry_object_values.get_fog_end()
	var dry_object_fog_type := dry_object_values.get_fog_type()
	var particle_renderer := world.get_effect_world().get_node(
			"ParticleRenderer") as ParticleRenderer
	assert_not_null(particle_renderer,
			"the runtime particle compositor is wired to MissionEnvironment")
	var dry_particle_fog: Dictionary = particle_renderer.get_debug_draw_list_report().get(
			"environment_fog", {})

	# Camera offsets move the rendered eye independently of global_position.
	# Cross the waterline with v_offset alone and pin the clear-color branch to
	# the same adjusted eye used by Water strip classification.
	var water := world.get_node("Water") as Water
	water.set_mission_water_height_override(10.0)
	camera.position.y = 10.25
	camera.v_offset = -0.25
	world.tick(camera.global_position, camera.get_global_transform())
	assert_false(env.is_underwater_view(),
			"an eye exactly on the plane stays dry: retail's side test is strict <")
	assert_true(env.is_underwater_overlay_view(),
			"the later retail murk scissor includes exact waterline equality")
	assert_eq(world.get_current_frame_clear_color(), mission_clear,
			"the frame clear shares the strict waterline equality policy")
	camera.v_offset = -1.0
	assert_lt(camera.get_camera_transform().origin.y, water.water_height)
	world.tick(camera.global_position, camera.get_global_transform())
	assert_true(env.is_underwater_overlay_view())
	var combined := EnvFile.combine_terrain_light(
			Color(env.get_sun_light().x, env.get_sun_light().y, env.get_sun_light().z),
			Color(env.get_sky_ambient().x, env.get_sky_ambient().y, env.get_sky_ambient().z))
	var lit := EnvFile.lit_water_color(
			Color(env.get_water_color().x, env.get_water_color().y, env.get_water_color().z),
			combined)
	assert_eq(world.get_current_frame_clear_color(), Color(lit.r, lit.g, lit.b),
			"v_offset below water selects the underwater clear even when the node origin is above")
	# Environment_ApplyFogAndAmbient selects one pass payload from the same
	# render-eye classification as the clear: underwater the lit water color,
	# murk-derived visibility end, and linear fog must reach both the terrain
	# shader and the shared ObjectModel record (the FP weapon consumes that
	# record in its dedicated pass). Pin the actual consumers, not merely the
	# already-correct clear color.
	var underwater_color := Vector3(lit.r, lit.g, lit.b)
	var underwater_end := env.get_environment_data().get_fog_end_underwater()
	assert_true(Vector3(terrain_material.get_shader_parameter("u_fog_color"))
			.is_equal_approx(underwater_color),
			"below-water terrain fogs toward Env_WaterColorLit")
	assert_almost_eq(float(terrain_material.get_shader_parameter("u_fog_end")),
			underwater_end, 0.001,
			"below-water terrain visibility follows the water-murk curve")
	assert_eq(int(terrain_material.get_shader_parameter("u_fog_type")), 1,
			"below-water terrain uses the witnessed linear fog mode")
	var object_values: EnvLightValues = env.get_light_state().get_values()
	assert_true(object_values.get_fog_color().is_equal_approx(underwater_color),
			"below-water ObjectModel/viewmodel fogs toward Env_WaterColorLit")
	assert_almost_eq(object_values.get_fog_end(), underwater_end, 0.001,
			"below-water ObjectModel/viewmodel visibility follows water murk")
	assert_eq(object_values.get_fog_type(), 1,
			"below-water ObjectModel/viewmodel uses the witnessed linear fog mode")
	var particle_fog: Dictionary = particle_renderer.get_debug_draw_list_report().get(
			"environment_fog", {})
	assert_true(Vector3(particle_fog.get("color", Vector3.ZERO))
			.is_equal_approx(underwater_color),
			"below-water particle submissions fog toward Env_WaterColorLit")
	assert_almost_eq(float(particle_fog.get("end", 0.0)), underwater_end, 0.001,
			"below-water particle visibility follows water murk")
	assert_eq(int(particle_fog.get("type", -1)), 1,
			"below-water particle submissions use linear fog")
	# Weather writes shader globals from its GameFramePipeline leg (the env
	# nodes advance after the scene-environment classify, before terrain) in a
	# live frame. Drive that leg explicitly in this paused harness and prove it
	# cannot replace the selected shared payload with dry fog. The portable
	# environment_state_test pins the exact pass-aware global block because
	# RenderingServer global readback is nil under the headless renderer.
	var weather := world.get_weather_node() as Weather
	weather.advance_frame(0.0)
	object_values = env.get_light_state().get_values()
	assert_true(object_values.get_fog_color().is_equal_approx(underwater_color),
			"Weather preserves the underwater shared fog color")
	assert_almost_eq(object_values.get_fog_end(), underwater_end, 0.001,
			"Weather preserves the underwater shared murk end")
	assert_eq(object_values.get_fog_type(), 1,
			"Weather preserves the underwater shared fog type")

	# A hidden loaded world is the menu/loading shell: do not leak its pass fog
	# through process-global shader parameters or the shared light record. A
	# synchronous show reclassifies the still-below render eye before drawing.
	world.visible = false
	await get_tree().process_frame
	assert_false(env.is_underwater_view(),
			"hiding a loaded underwater world retires its scene-pass state")
	assert_false(env.is_underwater_overlay_view(),
			"hiding a loaded world retires its pre-HUD murk overlay")
	object_values = env.get_light_state().get_values()
	assert_eq(object_values.get_fog_color(), dry_object_fog_color,
			"hidden/menu state restores the dry shared fog payload")
	world.visible = true
	await get_tree().process_frame
	assert_true(env.is_underwater_view(),
			"showing the world reclassifies its still-below render eye synchronously")
	assert_true(env.is_underwater_overlay_view())
	object_values = env.get_light_state().get_values()
	assert_true(object_values.get_fog_color().is_equal_approx(underwater_color),
			"showing the underwater world restores the pass payload before drawing")
	world.tick(camera.global_position, camera.get_global_transform())
	camera.v_offset = 0.0
	camera.position.y = 71.0
	water.set_mission_water_height_override(NAN)
	world.tick(camera.global_position, camera.get_global_transform())
	assert_eq(world.get_current_frame_clear_color(), mission_clear)
	assert_false(env.is_underwater_overlay_view(),
			"surfacing retires the independently gated murk overlay")
	assert_eq(Vector3(terrain_material.get_shader_parameter("u_fog_color")),
			dry_terrain_fog_color,
			"terrain restores the dry pass fog color after surfacing")
	assert_almost_eq(float(terrain_material.get_shader_parameter("u_fog_end")),
			dry_terrain_fog_end, 0.001,
			"terrain restores the dry pass fog end after surfacing")
	assert_eq(int(terrain_material.get_shader_parameter("u_fog_type")),
			dry_terrain_fog_type,
			"terrain restores the dry pass fog type after surfacing")
	object_values = env.get_light_state().get_values()
	assert_eq(object_values.get_fog_color(), dry_object_fog_color,
			"ObjectModel/viewmodel restores the dry pass fog color after surfacing")
	assert_almost_eq(object_values.get_fog_end(), dry_object_fog_end, 0.001,
			"ObjectModel/viewmodel restores the dry pass fog end after surfacing")
	assert_eq(object_values.get_fog_type(), dry_object_fog_type,
			"ObjectModel/viewmodel restores the dry pass fog type after surfacing")
	particle_fog = particle_renderer.get_debug_draw_list_report().get(
			"environment_fog", {})
	assert_eq(Vector3(particle_fog.get("color", Vector3.ZERO)),
			dry_particle_fog.get("color", Vector3.ZERO),
			"particle submissions restore the dry pass fog color after surfacing")
	assert_almost_eq(float(particle_fog.get("start", 0.0)),
			float(dry_particle_fog.get("start", 0.0)), 0.001,
			"particle submissions restore the dry pass fog start after surfacing")
	assert_almost_eq(float(particle_fog.get("end", 0.0)),
			float(dry_particle_fog.get("end", 0.0)), 0.001,
			"particle submissions restore the dry pass fog end after surfacing")
	assert_eq(int(particle_fog.get("type", -1)),
			int(dry_particle_fog.get("type", -1)),
			"particle submissions restore the dry pass fog type after surfacing")

	world.visible = false
	await get_tree().process_frame
	assert_eq(terrain.get_visible_patch_count(), 0,
		"a hidden GameWorld must hide native terrain RIDs that bypass Node3D visibility")
	assert_eq(world.get_current_frame_clear_color(), idle_clear,
		"a hidden GameWorld must restore the menu/loading frame clear")

	world.visible = true
	await get_tree().process_frame
	world.tick(camera.global_position, camera.get_global_transform())
	assert_gt(terrain.get_visible_patch_count(), 0,
		"showing the retained world lets terrain traversal present patches again")
	assert_eq(world.get_current_frame_clear_color(), mission_clear,
		"showing the loaded world restores its mission frame clear")

	world.unload()
	await get_tree().process_frame
	assert_eq(world.get_current_frame_clear_color(), idle_clear,
		"an unloaded GameWorld restores the scene-authored frame clear")


func test_water_mirror_camera_filters_entity_waves_and_never_draws_the_body() -> void:
	# The reflection layer contract (env #30): above water the mirror draws
	# exactly the flag-0x400 population — vehicles by item type plus records
	# whose BMS attribute authors Reflective — and it never draws the water
	# surface, FP overlay, or any person including the local body (there is
	# no player-render leg) [orig: Terrain_CollectVisibleEntitiesForReflection
	# @ 0x5c90a0 filterMask 0x400; Entity_InitFromModel @ 0x40e20a;
	# Entity_SpawnFromBMSRecord @ 0x40ed1d..0x40ed2b;
	# Water_ReflectionPrerender @ 0x5c2780 -> render_main_scene @ 0x5c1240;
	# Player_RenderFirstPersonViewModel @ 0x4ded60].
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	assert_not_null(packed, "the packaged world scene loads")
	var world := packed.instantiate()
	add_child_autofree(world)
	var water: Water = world.get_node_or_null("Water")
	assert_not_null(water, "the packaged scene ships the water node")
	if water == null:
		return
	var mirror: Camera3D = water.get_reflection_camera()
	assert_not_null(mirror, "the water builds its mirror camera on ready")
	if mirror == null:
		return
	assert_eq(mirror.cull_mask & Water.VISUAL_LAYER_WATER, 0,
		"the mirrored scene never draws the water surface itself")
	assert_eq(mirror.cull_mask & Water.VISUAL_LAYER_VIEWMODEL, 0,
		"the FP arms/weapon overlay never enters the mirrored scene")
	assert_eq(mirror.cull_mask & Water.VISUAL_LAYER_SHADOW_CASTER_MASK, 0,
		"caster-only helper instances never enter the color reflection")
	assert_eq(mirror.cull_mask & Water.VISUAL_LAYER_FP_BODY_SHADOW_ONLY, 0,
		"no person enters the mirror — the FP body layer stays out")
	assert_eq(mirror.cull_mask & Water.VISUAL_LAYER_WORLD_NO_MIRROR, 0,
		"plain (unflagged) world entities stay out of the above-water mirror")
	assert_ne(mirror.cull_mask & Water.VISUAL_LAYER_WORLD, 0,
		"the mirrored scene renders vehicles and authored-Reflective records")
	assert_eq(water.get_mesh_instance().layers, Water.VISUAL_LAYER_WATER,
		"the water strip rides the water-only layer the mirror excludes")


func test_exact_pose_refresh_retargets_a_frozen_water_mirror_without_advancing_tod() -> void:
	var world := WorldFixture.make_world(self)
	var camera := Camera3D.new()
	world.add_child(camera)
	camera.make_current()
	world.set_playable(false)
	assert_eq(WorldFixture.load_mission(world, _minimal_assets_dir()), OK)

	var runtime := world.get_runtime()
	assert_not_null(runtime)
	if runtime == null:
		return
	runtime.pause()
	assert_eq(world.debug_set_mission_minute_of_day(720.0), OK)
	var environment := world.get_environment_node()
	var fixed24_before: int = environment.get_mission_time_fixed24()
	var water := world.get_water_node()
	assert_not_null(water)
	if water == null:
		return
	water.set_mission_water_height_override(10.0)
	camera.global_position = Vector3(5.0, 30.0, 7.0)
	water.advance_frame(0.0)

	world.process_mode = Node.PROCESS_MODE_DISABLED
	camera.global_position = Vector3(100.0, 30.0, 200.0)
	camera.make_current()
	var expected_mirror_position := Vector3(100.0, -10.0, 200.0)
	assert_false(water.get_reflection_camera().global_position.is_equal_approx(
			expected_mirror_position),
			"moving a frozen capture camera leaves the reflection pose stale")

	assert_eq(world.debug_refresh_render_pose(camera), OK)
	assert_true(water.get_reflection_camera().global_position.is_equal_approx(
			expected_mirror_position),
			"the explicit evidence seam refreshes the production mirror camera")
	assert_eq(environment.get_mission_minute_of_day(), 720.0)
	assert_eq(environment.get_mission_time_fixed24(), fixed24_before,
			"render-pose refresh may not tick the mission clock")


func test_exact_pose_refresh_rebuilds_the_frozen_particle_draw_list() -> void:
	var world := WorldFixture.make_world(self)
	var camera := Camera3D.new()
	world.add_child(camera)
	camera.make_current()
	world.set_playable(false)
	assert_eq(WorldFixture.load_mission(world, _minimal_assets_dir()), OK)
	var runtime := world.get_runtime()
	assert_not_null(runtime)
	if runtime == null:
		return
	runtime.pause()

	# One synthetic renderable effect (the effect_world_test pattern): a live
	# world-bound emitter whose quads must survive the capture freeze.
	var effect_world: EffectWorld = world.get_effect_world()
	assert_not_null(effect_world)
	if effect_world == null:
		return
	var def := ParticleDef.new()
	def.id = "puff dots"
	def.emit_dur = 0.5
	def.emit_rate = 50.0
	def.emit_burst = 4
	def.age = 2.0
	def.alpha = 1.0
	def.scale_value = 1.0
	var graphics: Array = def.graphics
	var layer := graphics[0] as ParticleGraphicLayer
	layer.present = true
	layer.texture = "bink.tga"
	layer.alpha = 1.0
	layer.scale_value = 1.0
	def.graphics = graphics
	var effect := ParticleEffect.new()
	effect.id = "puff"
	effect.pdefs = PackedStringArray(["puff dots"])
	var file := ParticleFile.new()
	var particles: Array = file.particles
	particles.append(def)
	file.particles = particles
	var effects: Array = file.effects
	effects.append(effect)
	file.effects = effects
	file.source_path = ProjectSettings.globalize_path(
			"res://../fixtures/cbin/renderable_effect_fixture.ptl")
	effect_world.load_particle_file(file)
	# The world's facade wired its texture provider to the minimal fixture root,
	# which has no bink.tga; the procedural fallback keeps the layer renderable
	# without adding image fixtures to the shared minimal root.
	var renderer := effect_world.get_node("ParticleRenderer") as ParticleRenderer
	assert_not_null(renderer)
	if renderer != null:
		renderer.procedural_fallback_enabled = true
	var options := EffectSpawnOptions.new()
	options.admission = EffectScene.ADMISSION_ALWAYS
	options.binding = EffectScene.BINDING_WORLD
	options.render_domain = EffectScene.RENDER_DOMAIN_WORLD
	var receipt := effect_world.spawn_effect_request(
			"puff", Transform3D(Basis.IDENTITY, Vector3(2.0, 1.0, 3.0)), options)
	assert_true(receipt.spawned, "the probe effect spawns")
	for _i in range(3):
		effect_world.advance_fixed_tick(0.016)
	camera.global_position = Vector3(2.0, 1.5, 12.0)
	camera.look_at(Vector3(2.0, 1.0, 3.0))
	world.device_frame().render_particle_frame()
	var before: Dictionary = effect_world.get_debug_draw_list_report().get(
			"world_camera_side", {})
	assert_gt(int(before.get("rendered_quad_count", 0)), 0,
			"the live emitter renders quads before the freeze")

	world.process_mode = Node.PROCESS_MODE_DISABLED
	camera.global_position = Vector3(6.0, 3.0, 14.0)
	camera.look_at(Vector3(2.0, 1.0, 3.0))
	camera.make_current()
	var stale: Dictionary = effect_world.get_debug_draw_list_report().get(
			"world_camera_side", {})
	assert_eq(stale.get("compile_index"), before.get("compile_index"),
			"a frozen world leaves the particle draw list stale at the old pose")

	assert_eq(world.debug_refresh_render_pose(camera), OK)
	var after: Dictionary = effect_world.get_debug_draw_list_report().get(
			"world_camera_side", {})
	assert_ne(after.get("compile_index"), before.get("compile_index"),
			"the evidence seam rebuilds the particle draw list for the capture camera")
	assert_gt(int(after.get("rendered_quad_count", 0)), 0,
			"frozen-phase particles stay visible after the camera retarget")


func test_game_world_is_playable_by_default_without_env_flag() -> void:
	var world := WorldFixture.make_world(self)
	assert_true(world.is_playable(),
			"normal and F6 standalone launches spawn a player by default")
	world.set_playable(false)
	assert_false(world.is_playable(),
			"diagnostic fixtures can explicitly opt out of local-player setup")


func test_load_mission_data_rejects_an_empty_document() -> void:
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	var failures: Array = []
	world.load_failed.connect(func(reason): failures.append(reason))
	assert_eq(world.load_mission_data(null, "x.bms"), ERR_INVALID_PARAMETER)
	assert_eq(world.load_mission_data(MissionData.new(), "x.bms"), ERR_INVALID_PARAMETER,
		"an unloaded document is rejected before any root resolution")
	assert_eq(failures.size(), 2, "both rejections explain themselves via load_failed")


func test_loaded_mission_drives_the_shared_time_of_day_clock() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)

	var root := ResourceRoot.new()
	var fixture_dir := ProjectSettings.globalize_path("res://../assets")
	assert_eq(root.set_root_dir(fixture_dir), OK)
	world.set_resource_root(root)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, "mnml.bms"), OK)
	mission.set_header_int("start_time", 0x0540)  # unsigned Q8.8 = 05:15
	mission.set_header_int("minutes_per_day", 60)

	assert_eq(world.load_mission_data(mission, "mnml.bms"), OK)
	var audio := world.get_mission_audio()
	assert_not_null(audio)
	if audio == null:
		return
	var env := world.get_node("MissionEnvironment") as MissionEnvironment
	var expected_clock := MissionEnvironment.new()
	expected_clock.configure_mission_clock(0x0540, 60)
	expected_clock.advance_mission_clock(Weather.MISSION_START_PREWARM_TICKS)
	assert_almost_eq(env.time_of_day, expected_clock.time_of_day, 0.000001,
		"the BMS start time plus retail's 255-tick prewarm initializes the shared clock")

	# 0.128 seconds advances eight 62.5 Hz simulation ticks and the weather
	# clock rides every one of them (retail Environment_UpdateWeatherTick runs
	# once per drained quantum, Game_ProcessMainFrame @ 0x52674b).
	world.tick(Vector3.ZERO, Transform3D(), 0.128)
	var advanced := env.time_of_day
	expected_clock.advance_mission_clock(8)
	assert_almost_eq(advanced, expected_clock.time_of_day, 0.000001,
			"the mission clock advances once per simulation tick")
	assert_eq(int(world.get_runtime().get_perf_counters().ticks), 8,
			"mission simulation retains its 62.5 Hz cadence")
	expected_clock.free()

	# F3/MCP scrubs are a host-level operation: pause guarantees no later
	# weather tick can hide a missing resync behind normal advancement.
	world.get_runtime().pause()
	assert_eq(world.debug_set_mission_minute_of_day(22.0 * 60.0 + 7.0), OK)
	assert_almost_eq(env.time_of_day, 2207.0, 0.001)
	var weather := world.get_weather_node() as Weather
	assert_true(weather.get_smooth_fill().is_equal_approx(
			env.get_fill_light_target()))
	assert_true(weather.get_smooth_sun().is_equal_approx(
			env.get_sun_light_target()))
	assert_true(weather.get_smooth_fog().is_equal_approx(
			env.get_fog_color_target()))
	world.tick(Vector3.ZERO, Transform3D(), 0.25)
	assert_almost_eq(env.time_of_day, 2207.0, 0.001,
			"a paused host keeps the scrubbed clock and rendered weather")
	world.unload()


func _world_driven_weather_state_after(deltas: Array) -> Array:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)

	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../assets")), OK)
	world.set_resource_root(root)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, "mnml.bms"), OK)
	mission.set_header_int("start_time", 0x0540)
	mission.set_header_int("minutes_per_day", 60)
	assert_eq(world.load_mission_data(mission, "mnml.bms"), OK)

	var sim_ticks := 0
	for delta in deltas:
		world.tick(Vector3.ZERO, Transform3D(), float(delta))
		sim_ticks += int(world.get_runtime().get_perf_counters().ticks)
	var env := world.get_node("MissionEnvironment") as MissionEnvironment
	var weather := world.get_node("Weather") as Weather
	var state := [
		sim_ticks,
		env.time_of_day,
		weather.get_smooth_fill(),
		weather.get_smooth_sun(),
		weather.get_smooth_fog(),
		weather.get_smooth_sky(),
		weather.get_smooth_skyfog(),
		weather.get_smooth_sky_base(),
		weather.get_smooth_sky_bright(),
		weather.get_smooth_sky_highlight(),
		weather.get_smooth_cloud_base(),
		weather.get_smooth_cloud_highlight(),
		weather.get_smooth_cloud_edge(),
		weather.get_sway_amount(),
		weather.get_sway_phase(),
	]
	world.unload()
	return state


func test_world_driven_weather_is_invariant_to_render_batching() -> void:
	var slow: Array = await _world_driven_weather_state_after([0.128])
	var split: Array = await _world_driven_weather_state_after([
		0.016, 0.016, 0.016, 0.016, 0.016, 0.016, 0.016, 0.016,
	])
	assert_eq(slow, split,
			"each simulation tick refreshes TOD targets before one weather tick")
	assert_eq(int(slow[0]), 8, "0.128 seconds still contains eight simulation ticks")

	# ONE clock: the weather rides the 62.5 Hz simulation tick, so 0.128 s is
	# exactly eight weather/TOD ticks (retail Game_ProcessMainFrame @ 0x526774).
	var expected_eight := MissionEnvironment.new()
	expected_eight.configure_mission_clock(0x0540, 60)
	expected_eight.advance_mission_clock(
			Weather.MISSION_START_PREWARM_TICKS + 8)
	assert_almost_eq(float(slow[1]), expected_eight.time_of_day, 0.000001,
			"0.128 seconds contains eight weather/TOD ticks")

	var boundary: Array = await _world_driven_weather_state_after([0.128, 0.000001])
	assert_eq(int(boundary[0]), 8,
			"a sub-tick residual delta runs no extra weather quantum")
	assert_almost_eq(float(boundary[1]), expected_eight.time_of_day, 0.000001,
			"no separate weather credit exists to consume a ninth TOD tick")
	expected_eight.free()


func test_injected_root_bypasses_settings_mount() -> void:
	# The editor injects its own mounted root; the load must resolve through it
	# (and report ITS directory in errors) instead of mounting from settings.
	var root_dir := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join("injected_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root_dir)
	var injected := ResourceRoot.new()
	assert_eq(injected.set_root_dir(root_dir), OK)

	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_resource_root(injected)

	var failures: Array = []
	world.load_failed.connect(func(reason): failures.append(String(reason)))
	assert_eq(world.load_mission("missing.bms"), ERR_FILE_NOT_FOUND,
		"the missing file resolves against the injected root")
	assert_eq(failures.size(), 1)
	assert_string_contains(failures[0], root_dir.get_file(),
		"the error names the injected root's directory, proving no settings mount ran")


# Typed net-session request builders (the records GameWorld's host/joiner entries
# consume; expansion/game_type ride the record defaults: "" + GAME_TYPE_COOP).
func _lan_host_config(mission: String, bind_port: int) -> HostSessionConfig:
	var config := HostSessionConfig.new()
	config.mission = mission
	config.bind_port = bind_port
	return config


func _join_target(host_ip: String, port: int, mission := "", player_name := "Joiner") -> JoinTarget:
	var target := JoinTarget.new()
	target.host_ip = host_ip
	target.port = port
	target.mission = mission
	target.player_name = player_name
	return target


func test_failed_host_load_does_not_arm_the_next_mission_as_a_lan_host() -> void:
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_playable(false)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../assets")), OK)
	world.set_resource_root(root)

	assert_eq(world.load_mission_as_host(_lan_host_config("missing.bms", 0)),
		ERR_FILE_NOT_FOUND)
	assert_eq(world.load_mission("mnml.bms"), OK)
	var sim: Simulation = world.get_sim()
	assert_not_null(sim)
	if sim != null:
		assert_false(sim.is_host_listening(),
			"a rejected host request cannot turn a later ordinary mission into a LAN host")
	world.unload()


func test_lan_host_threads_truthful_base_metadata_into_the_native_session() -> void:
	# GameConfig's old capture-shaped defaults include jox01; the production
	# GameWorld handoff must explicitly replace them with the menu/root values,
	# including the meaningful empty string for a base-game mount.
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_playable(false)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../assets")), OK)
	world.set_resource_root(root)

	assert_eq(world.load_mission_as_host(_lan_host_config("mnml.bms", 0)), OK)
	var sim: Simulation = world.get_sim()
	assert_not_null(sim)
	if sim != null:
		var config := sim.get_host_session_config()
		assert_eq(config.game_type, 0x30020)
		assert_eq(config.expansion, "",
			"base JO stays empty instead of falling back to captured jox01")
	world.unload()


func test_lan_host_bind_failure_is_reported_instead_of_falling_back_socketless() -> void:
	# Reserve an OS-chosen endpoint, then request that exact port through the
	# production GameWorld host path. The old behavior silently started an SP
	# listen session and still emitted world_loaded, leaving joiners no socket.
	var blocker := UdpPump.new()
	assert_eq(blocker.bind_listen(0), OK)
	var occupied_port := blocker.local_port()
	assert_gt(occupied_port, 0)

	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_playable(false)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../assets")), OK)
	world.set_resource_root(root)
	watch_signals(world)
	var failures: Array[String] = []
	world.load_failed.connect(func(reason: String): failures.append(reason))

	var result := world.load_mission_as_host(_lan_host_config("mnml.bms", occupied_port))
	assert_eq(result, ERR_CANT_CREATE)
	assert_false(world.is_loaded())
	assert_null(world.get_sim(), "a failed UDP host bind creates no socketless fallback sim")
	assert_signal_not_emitted(world, "world_loaded")
	assert_eq(failures.size(), 1)
	if failures.size() == 1:
		assert_string_contains(failures[0], str(occupied_port),
			"the launch error identifies the exact requested port")
	blocker.close()


func test_lan_host_bind_failure_survives_synchronous_teardown_handler() -> void:
	# The game shell returns to the menu from INSIDE load_failed — its teardown
	# calls unload(), which frees the failed runtime. The bind-failure leg must
	# free/null its runtime before emitting; emitting first made the handler's
	# reentry turn the follow-up free into a null-instance error.
	var blocker := UdpPump.new()
	assert_eq(blocker.bind_listen(0), OK)
	var occupied_port := blocker.local_port()
	assert_gt(occupied_port, 0)

	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_playable(false)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../assets")), OK)
	world.set_resource_root(root)
	var failures: Array[String] = []
	world.load_failed.connect(func(reason: String):
		failures.append(reason)
		world.unload())

	var result := world.load_mission_as_host(_lan_host_config("mnml.bms", occupied_port))
	assert_eq(result, ERR_CANT_CREATE)
	assert_eq(failures.size(), 1)
	assert_false(world.is_loaded())
	assert_null(world.get_sim())
	blocker.close()


func test_escape_aborts_the_joiner_preload_wait() -> void:
	# ESC during a load: the joiner's pre-load connect/session wait is the one
	# interruptible leg — the reachable analog of the original per-asset abort
	# poll [orig: Client_CheckDisconnectOrEscDuringLoad @ 0x520270]
	# (docs/interface/loading-screen-re.md D-LOADSCR-7).
	var blocker := UdpPump.new()  # a bound but silent "host": never replies
	assert_eq(blocker.bind_listen(0), OK)
	var silent_port := blocker.local_port()
	assert_gt(silent_port, 0)

	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_playable(false)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../assets")), OK)
	world.set_resource_root(root)
	var failures: Array[String] = []
	world.load_failed.connect(func(reason: String): failures.append(reason))

	assert_false(world.cancel_join_preload(), "no preload in flight is a no-op")
	assert_eq(world.load_mission_as_joiner(
			_join_target("127.0.0.1", silent_port, "", "EscTester")), OK)
	await get_tree().process_frame  # the preload process step starts
	assert_true(world.cancel_join_preload(), "an in-flight preload aborts")
	assert_eq(failures.size(), 1)
	if failures.size() == 1:
		assert_string_contains(failures[0], "aborted")
	assert_false(world.is_loaded())
	await get_tree().process_frame  # the disabled process step stays quiet
	assert_eq(failures.size(), 1, "the canceled driver does not double-report")
	blocker.close()


func test_freeing_world_during_joiner_preload_leaves_no_suspended_owner_method() -> void:
	var blocker := UdpPump.new()  # bound but silent: keeps the preload pending
	assert_eq(blocker.bind_listen(0), OK)
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_playable(false)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../assets")), OK)
	world.set_resource_root(root)

	assert_eq(world.load_mission_as_joiner(
			_join_target("127.0.0.1", blocker.local_port(), "", "FreeTester")), OK)
	await get_tree().process_frame  # the preload process step starts
	world.unload()
	world.free()
	world = null
	blocker.close()
	await get_tree().process_frame
	assert_engine_error_count(0,
			"tearing down a pending preload leaves no suspended owner method")


func test_join_wire_asset_failure_is_edge_gated_per_session() -> void:
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	var failures: Array[String] = []
	world.load_failed.connect(func(reason: String): failures.append(reason))

	world.report_join_wire_asset_failure("first failure")
	world.report_join_wire_asset_failure("duplicate observer failure")
	assert_eq(failures, ["first failure"],
			"the frame observer and admission watchdog share one failure edge")

	world.unload()
	world.report_join_wire_asset_failure("next session failure")
	assert_eq(failures, ["first failure", "next session failure"],
			"unload rearms the latch for the next join")


func test_escape_aborts_the_joiner_admission_wait() -> void:
	# ESC during the SECOND interruptible joiner wait: a real host drives the
	# wire-header preload through local world load, then this test stops pumping it
	# before admission so cancel_join_admission owns the remaining wait.
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_playable(false)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../assets")), OK)
	world.set_resource_root(root)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, "mnml.bms"), OK)
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.server_name = "Abort Admission Host"
	host_options.mission_name = mission.get_mission_name()
	host_options.mission_file = "mnml.bms"
	host_options.expansion = ""
	host_options.game_type = 0x30020
	host_options.max_players = 4
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	var failures: Array[String] = []
	world.load_failed.connect(func(reason: String): failures.append(reason))
	var observed := {"local_load": false}
	world.world_loaded.connect(
			func(): observed["local_load"] = true, CONNECT_ONE_SHOT)

	assert_false(world.cancel_join_admission(), "no armed admission wait is a no-op")
	assert_eq(world.load_mission_as_joiner(
			_join_target("127.0.0.1", host.get_host_listen_port(), "mnml")), OK)
	for _frame in range(600):
		host.step()
		await get_tree().process_frame
		if bool(observed["local_load"]):
			break
	assert_true(bool(observed["local_load"]),
			"the real wire-header preload reached the admission wait")
	assert_true(world.cancel_join_admission(), "an armed admission wait accepts the abort")
	# GameWorld is externally ticked by MainGame in production. This bare-world
	# fixture drives the same frame observer once so it consumes the abort.
	world.tick(Vector3.ZERO)
	await get_tree().process_frame
	assert_eq(failures.size(), 1)
	if failures.size() == 1:
		assert_string_contains(failures[0], "aborted")
	await get_tree().process_frame
	assert_eq(failures.size(), 1, "the aborted watchdog does not double-report")
	assert_false(world.cancel_join_admission(), "the wait is disarmed after the abort")
	world.unload()


func test_failed_join_load_does_not_make_the_next_mission_wire_only() -> void:
	var blocker := UdpPump.new()
	assert_eq(blocker.bind_listen(0), OK)
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_playable(false)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../assets")), OK)
	world.set_resource_root(root)

	assert_eq(world.load_mission_as_joiner(_join_target(
			"127.0.0.1", blocker.local_port(), "missing.bms")), OK,
			"a browse-time mission hint is never opened locally")
	await get_tree().process_frame
	assert_true(world.cancel_join_preload())
	await get_tree().process_frame
	assert_eq(world.load_mission("mnml.bms"), OK)
	assert_gt(world.get_mission_stats().markers, 0,
		"a rejected join request cannot make a later ordinary mission wire-only")
	world.unload()
	blocker.close()


func test_environment_load_failure_finishes_its_perf_timeline() -> void:
	var root_dir := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join(
		"timeline_env_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(root_dir), OK)
	var bms_name := "timeline_env_fail_%d.bms" % Time.get_ticks_usec()
	assert_eq(DirAccess.copy_absolute(
		ProjectSettings.globalize_path("res://../assets/mnml.bms"),
		root_dir.path_join(bms_name)), OK)
	_write_fixture_file(root_dir.path_join("mnml.env"), "")
	_write_fixture_file(root_dir.path_join("mnml.trn"), "terrain_name \"mnml\"\n")
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK)

	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	world.set_resource_root(root)
	assert_eq(world.load_mission(bms_name), ERR_CANT_OPEN)

	var timeline: PerfTimeline = world.last_load_timeline()
	assert_not_null(timeline, "a failed environment stage still retains its timeline")
	if timeline == null:
		return
	assert_eq(timeline.label, "Mission load %s" % bms_name)
	var spans := timeline.spans()
	assert_eq(spans.size(), 1)
	if spans.size() == 1:
		assert_eq(spans[0].name, "environment")
		assert_gt(spans[0].end_us, 0,
			"finish closes the environment span left open by the early return")


func test_terrain_load_failure_finishes_its_perf_timeline() -> void:
	var root_dir := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join(
		"timeline_terrain_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(root_dir), OK)
	var bms_name := "timeline_terrain_fail_%d.bms" % Time.get_ticks_usec()
	assert_eq(DirAccess.copy_absolute(
		ProjectSettings.globalize_path("res://../assets/mnml.bms"),
		root_dir.path_join(bms_name)), OK)
	# A REAL environment node parses the env stage now, so this root needs the
	# valid fixture env; the empty terrain still fails the terrain stage.
	assert_eq(DirAccess.copy_absolute(
		ProjectSettings.globalize_path("res://../assets/mnml.env"),
		root_dir.path_join("mnml.env")), OK)
	_write_fixture_file(root_dir.path_join("mnml.trn"), "")
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK)

	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_playable(false)
	world.set_resource_root(root)
	assert_eq(world.load_mission(bms_name), ERR_CANT_OPEN)

	var timeline: PerfTimeline = world.last_load_timeline()
	assert_not_null(timeline, "a failed terrain stage still retains its timeline")
	if timeline == null:
		return
	assert_eq(timeline.label, "Mission load %s" % bms_name)
	var spans := timeline.spans()
	assert_eq(spans.size(), 2)
	if spans.size() == 2:
		assert_eq(spans[0].name, "environment")
		assert_eq(spans[1].name, "terrain")
		assert_gt(spans[1].end_us, 0,
			"finish closes the terrain span left open by the early return")


func test_successful_mission_load_exposes_the_loaded_file_until_unload() -> void:
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame

	var root := ResourceRoot.new()
	var fixture_dir := ProjectSettings.globalize_path("res://../assets")
	assert_eq(root.set_root_dir(fixture_dir), OK)
	world.set_resource_root(root)
	world.mission_file = "boot-option.bms"

	assert_eq(world.load_mission("mnml.bms"), OK)
	assert_eq(world.get_loaded_mission_file(), "mnml.bms",
		"the successful load argument, not the exported boot option, is the active mission")
	assert_eq(world.mission_file, "boot-option.bms",
		"loading does not repurpose the exported boot option as mutable runtime state")

	world.unload()
	assert_eq(world.get_loaded_mission_file(), "",
		"an unloaded world no longer reports a stale active mission")


func test_runtime_dev_mount_still_loads_bms_from_archive() -> void:
	var root_dir := _staged(WorldFixture.stage_minimal_root("archive_only_bms"))
	var archived_bms := FileAccess.get_file_as_bytes(root_dir.path_join("mnml.bms"))
	_write_pff(root_dir.path_join("resource.pff"), [{
		"name": "mnml.bms",
		"bytes": archived_bms,
	}])
	_write_bytes(root_dir.path_join("mnml.bms"), "not a mission".to_utf8_buffer())

	var resource_root := ResourceRoot.new()
	assert_eq(resource_root.mount_runtime(root_dir, "", true), OK,
		"the runtime fixture mounts with /d loose overrides enabled")
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_playable(false)
	world.set_resource_root(resource_root)

	assert_eq(world.load_mission("mnml.bms"), OK,
		"the witnessed BMS caller bypasses the corrupt loose override")
	assert_eq(world.get_loaded_mission_file(), "mnml.bms")
	world.unload()


func test_editor_run_loads_the_exact_saved_loose_bms() -> void:
	var root_dir := _staged(WorldFixture.stage_minimal_root("exact_loose_bms"))
	_write_pff(root_dir.path_join("resource.pff"), [{
		"name": "mnml.bms",
		"bytes": "not a mission".to_utf8_buffer(),
	}])

	var resource_root := ResourceRoot.new()
	assert_eq(resource_root.mount_runtime(root_dir, "", true), OK)
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_playable(false)
	world.set_resource_root(resource_root)

	assert_eq(world.load_loose_mission("mnml.bms"), OK,
		"F6 opens the valid disk BMS even when the archive row is corrupt")
	assert_eq(world.get_loaded_mission_file(), "mnml.bms")
	world.unload()


func test_editor_run_rejects_non_top_level_or_non_bms_paths() -> void:
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	assert_eq(world.load_loose_mission("nested/mnml.bms"), ERR_INVALID_PARAMETER)
	assert_eq(world.load_loose_mission("../mnml.bms"), ERR_INVALID_PARAMETER)
	assert_eq(world.load_loose_mission("mnml.mis"), ERR_INVALID_PARAMETER)


func test_runtime_mission_til_forces_loose_first_in_packed_mode() -> void:
	var root_dir := _make_fixture_root("loose_first_til")
	var source_dir := ProjectSettings.globalize_path("res://../assets")
	var archive_entries: Array = []
	for file_name in DirAccess.get_files_at(source_dir):
		archive_entries.append({
			"name": file_name,
			"bytes": FileAccess.get_file_as_bytes(source_dir.path_join(file_name)),
		})
	archive_entries.append({
		"name": "mnml.til",
		"bytes": TilFixture.bytes_for_cell(0),
	})
	_write_pff(root_dir.path_join("resource.pff"), archive_entries)
	_write_bytes(root_dir.path_join("mnml.til"), TilFixture.bytes_for_cell(4))

	var resource_root := ResourceRoot.new()
	assert_eq(resource_root.mount_runtime(root_dir), OK,
		"packed-default mode makes the archive win unless the caller forces loose-first")
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_playable(false)
	world.set_resource_root(resource_root)
	assert_eq(world.load_mission("mnml.bms"), OK)

	var terrain := world.get_node("Terrain") as Terrain
	var tile_info := terrain.tile_info_override as TerrainTileInfo
	assert_not_null(tile_info)
	if tile_info != null:
		assert_true(tile_info.blocks_foliage(72.0, 8.0, 2.0),
			"the loose TIL blocker at cell 4 reaches terrain composition")
		assert_false(tile_info.blocks_foliage(8.0, 8.0, 2.0),
			"the conflicting archived TIL at cell 0 does not win")
	world.unload()


func test_mission_til_is_shared_by_terrain_foliage_and_cleared_without_file() -> void:
	var root_dir := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join(
		"mission_til_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(root_dir), OK)
	var source_dir := ProjectSettings.globalize_path("res://../assets")
	for file_name in DirAccess.get_files_at(source_dir):
		assert_eq(DirAccess.copy_absolute(
			source_dir.path_join(file_name), root_dir.path_join(file_name)), OK)

	var til_bytes := PackedByteArray()
	til_bytes.resize(28)
	til_bytes.encode_u32(0, 0x74696c30)
	til_bytes.encode_u32(4, 1)
	# One entry at world [0,16] x [0,16].
	til_bytes.encode_u32(16, 0)
	til_bytes.encode_u32(20, 0)
	til_bytes[24] = 1
	var til_file := FileAccess.open(root_dir.path_join("mnml.til"), FileAccess.WRITE)
	assert_not_null(til_file)
	if til_file == null:
		return
	til_file.store_buffer(til_bytes)
	til_file.close()

	var resource_root := ResourceRoot.new()
	assert_eq(resource_root.set_root_dir(root_dir), OK)
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	world.set_resource_root(resource_root)
	assert_eq(world.load_mission("mnml.bms"), OK)

	var terrain := world.get_node("Terrain") as Terrain
	var dispatcher := world.get_node("Terrain/FoliageDispatcher") as FoliageDispatcher
	var tile_info := terrain.tile_info_override as TerrainTileInfo
	assert_not_null(tile_info)
	if tile_info != null:
		assert_eq(tile_info.get_entry_count(), 1)
		assert_true(tile_info.blocks_foliage(8.0, 8.0, 2.0))
		assert_same(dispatcher.tile_info, tile_info,
			"Terrain composition and foliage exclusion must share the parsed mission resource.")

	world.unload()
	await get_tree().process_frame
	assert_null(terrain.tile_info_override)
	assert_null(dispatcher.tile_info)

	assert_eq(DirAccess.remove_absolute(root_dir.path_join("mnml.til")), OK)
	var no_til_root := ResourceRoot.new()
	assert_eq(no_til_root.set_root_dir(root_dir), OK)
	world.set_resource_root(no_til_root)
	assert_eq(world.load_mission("mnml.bms"), OK)
	assert_null(terrain.tile_info_override,
		"A subsequent mission without a co-named TIL cannot inherit stale blockers.")
	assert_null(dispatcher.tile_info)
	world.unload()
	await get_tree().process_frame


func test_unload_forgets_the_viewmodel_def_memo() -> void:
	var world := WorldFixture.make_world(self)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../assets")), OK)
	world.set_resource_root(root)
	assert_eq(world.load_mission("mnml.bms"), OK)
	# The debug viewmodel rig (the `set_viewmodel_weapon` control's seam).
	assert_true(world.set_local_player_weapon_by_name("WPN_AK47AUTO"))
	assert_not_null(world.local_player_viewmodel_def())
	assert_eq(world.local_player_weapon_name(), "WPN_AK47AUTO",
			"the first decode installs the weapon dict")
	world.unload()
	# The same resolved name in the next mission must re-decode from that
	# mission's weapon.def instead of returning the memo over an empty dict
	# (unload drops the previous entity's selection, so it is re-rigged).
	assert_eq(world.load_mission("mnml.bms"), OK)
	assert_true(world.set_local_player_weapon_by_name("WPN_AK47AUTO"))
	var again: PlayerViewmodelDef = world.local_player_viewmodel_def()
	assert_not_null(again)
	assert_eq(world.local_player_weapon_name(), "WPN_AK47AUTO",
			"a reload with the same weapon name repopulates the weapon dict")
	world.unload()


func test_unload_drops_the_previous_entitys_armory_viewmodel_state() -> void:
	var world := WorldFixture.make_world(self)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../assets")), OK)
	world.set_resource_root(root)
	assert_eq(world.load_mission("mnml.bms"), OK)

	world.clear_local_player_weapon()
	assert_null(world.local_player_viewmodel_def(), "the authored NONE row has no viewmodel")
	world.unload()

	# The root stays mounted across unload, so the next entity's rig resolves
	# against its weapon.def even before the next mission loads.
	assert_true(world.set_local_player_weapon_by_name("WPN_AK47AUTO"))
	var restored: PlayerViewmodelDef = world.local_player_viewmodel_def()
	assert_not_null(restored, "a new mission is not stuck with the previous entity's NONE state")
	if restored != null:
		assert_eq(restored.weapon_name, "WPN_AK47AUTO",
			"the next mission can resolve a weapon after the previous entity selected NONE")


# --- The real first-person viewmodel over a staged root -----------------------
# The staged weapon.def rows, appended to the minimal pack's file (the same row
# shape as its WPN_AK47AUTO: the magazine/reserve keys the parser reads plus
# the nine action rows the FSM bakes, so the sim's retained table accepts the
# mount). WPN_TEST names a synthetic FP gun; WPN_AVENGER is a valid
# retail-shaped emplaced definition with NO fpModel [orig: the Flags 0x80
# emplaced test @ 0x4dedc7; a resolved def with no gfx1 submits no gun,
# Player_RenderFirstPersonViewModel @ 0x4ded60]. The FP arms are the
# character's, never a weapon.def field (retail parses-and-discards gfx1a/gfx1b
# [orig: WeaponDefs_ParseLineCallback @0x5448d0/@0x5448e6]).
const VIEWMODEL_WEAPON_ROW := """
weapon "%s"
	category 1
	rank     0
	statid   %d
	clipsize    30
	maxclips    7
	startrounds 210
	ammoclass   CLASS_556MM 1
	round_type  AM_556MM
	flags       %s
%s	pos		10.0		0.0		-201.0			0.0		0.0		1.0
	TPOS	-28.046		21.531		-187.857		0.0		0.0		0.0

	ACTION	"IDLE"
	DELAYEND	auto
	ANIM		ANIM_WPN_IDLE
	FUNCTION	WPN_STD_IDLE
	END

	ACTION	"EMPTYIDLE"
	DELAYEND	auto
	ANIM		ANIM_WPN_IDLE
	FUNCTION	WPN_STD_IDLE
	END

	ACTION	"FIRE"
	DELAYEND	5
	ANIM		ANIM_WPN_FIRE
	FUNCTION	WPN_STD_FIRE
	END

	ACTION	"RECOIL"
	DELAYEND	0
	ANIM		ANIM_WPN_RECOIL
	FUNCTION	WPN_STD_RECOIL
	END

	ACTION	"RELOAD"
	DELAYSTART	196
	DELAYEND	auto
	ANIM		ANIM_WPN_RELOAD
	FUNCTION	WPN_STD_RELOAD
	END

	ACTION	"EMPTY"
	DELAYSTART	0
	DELAYEND	auto
	ANIM		ANIM_WPN_EMPTY
	FUNCTION	WPN_STD_EMPTY
	END

	ACTION	"SWITCHTO"
	DELAYSTART	1
	DELAYEND	1
	ANIM		ANIM_WPN_SWITCHRANK
	FUNCTION	WPN_STD_SWITCHTO
	END

	ACTION	"SWITCHFROM"
	DELAYSTART	1
	DELAYEND	1
	ANIM		ANIM_WPN_SWITCHFROM
	FUNCTION	WPN_STD_SWITCHFROM
	END

	ACTION	"SWITCHRANK"
	DELAYSTART	1
	DELAYEND	1
	ANIM		ANIM_WPN_SWITCHRANK
	FUNCTION	WPN_STD_SWITCHRANK
	END
end
"""
const VIEWMODEL_GUN_GRAPHIC := "TestGun"  # gun.3di under the WPN_TEST gfx1 name
# person.3di under the arms name Avatars.def carries (with its extension, as
# the registry names its parts).
const VIEWMODEL_ARMS_GRAPHIC := "TestArms.3di"
const VIEWMODEL_ARMS_CAMO := Vector3i(17, 34, 51)
# The player's character registry: retail's ONLY first-person arms source is
# the selected combo's arms part, so one good-side combo binds the synthetic
# person head/body + the TestArms arms with an authored raw camo triplet.
const VIEWMODEL_AVATARS_DEF := """define head STAGED_HEAD
{
	graphic person.3di
	camo 0 0 0
	voice 1
	sex m
}
define body STAGED_BODY
{
	graphic person.3di
	camo 0 0 0
}
define arms STAGED_ARMS
{
	graphic TestArms.3di
	camo 17 34 51
}
nationality 0 STAGED_NAT
{
	alignment good
	division 0 STAGED_DIV
	{
		combo 1 STAGED_HEAD STAGED_BODY STAGED_ARMS
	}
}
"""


# Stage the minimal pack plus the two viewmodel weapon rows and the synthetic
# gun/arms/person models; `with_character` adds the Avatars.def combo the local
# player's arms resolve through (without it no character resolves: gun alone).
func _stage_viewmodel_fixture(name: String, with_character: bool) -> String:
	var root_dir := _staged(WorldFixture.stage_minimal_root(name))
	var rows := VIEWMODEL_WEAPON_ROW % [
			"WPN_TEST", 102, "auto", "\tGFX1\t%s\n" % VIEWMODEL_GUN_GRAPHIC] \
			+ VIEWMODEL_WEAPON_ROW % ["WPN_AVENGER", 103, "emplaced", ""]
	_append_to_file(root_dir.path_join("weapon.def"), rows.replace("\n", "\r\n"))
	for pair in [
		["gun.3di", VIEWMODEL_GUN_GRAPHIC + ".3di"],
		["person.3di", VIEWMODEL_ARMS_GRAPHIC],
		["person.3di", "person.3di"],
	]:
		assert_eq(DirAccess.copy_absolute(
				ProjectSettings.globalize_path("res://../fixtures/threedi/synth/" + pair[0]),
				root_dir.path_join(pair[1])), OK)
	if with_character:
		WorldFixture.write_file(root_dir.path_join("Avatars.def"),
				VIEWMODEL_AVATARS_DEF.replace("\n", "\r\n"))
	return root_dir


func test_valid_emplaced_def_without_gfx1_builds_no_fallback_gun() -> void:
	# AVENGER has a valid retail weapon definition but no fpModel. That means an
	# intentionally empty FP pass, not the bring-up AK fallback used when no
	# definition resolves at all.
	var root_dir := _stage_viewmodel_fixture("emplaced_no_gfx1", false)
	var world := WorldFixture.boot_minimal(self, root_dir)
	assert_true(world.set_local_player_weapon_by_name("WPN_AVENGER"),
			"the staged weapon.def carries the emplaced definition")
	var def := world.local_player_viewmodel_def()
	assert_not_null(def, "a valid definition resolves for the equipped weapon")
	if def == null:
		return
	assert_eq(def.weapon_name, "WPN_AVENGER")
	assert_eq(def.gfx1, "", "the valid definition authors no fpModel")
	var viewmodel := world.build_local_player_viewmodel()
	assert_not_null(viewmodel,
			"a valid no-model definition is a stable empty FP presentation epoch")
	assert_true(world.local_player_viewmodel_parts().is_empty(),
			"a missing authored gfx1 must not substitute the AK first-person gun")
	assert_false(world.local_player_first_person_model_available(),
			"the render gate observes that no first-person gun model resolved")


func test_first_person_uses_selected_arms_and_raw_part_local_camo() -> void:
	var root_dir := _stage_viewmodel_fixture("selected_arms", true)
	var world := WorldFixture.boot_minimal(self, root_dir)
	assert_true(world.set_local_player_weapon_by_name("WPN_TEST"))
	var container := world.build_local_player_viewmodel()
	assert_not_null(container)
	var parts := world.local_player_viewmodel_parts()
	assert_eq(parts.size(), 2)
	if parts.size() != 2:
		return
	var arms: ObjectModel = parts[0]
	var gun: ObjectModel = parts[1]
	assert_eq(arms.graphic_name, VIEWMODEL_ARMS_GRAPHIC,
			"the first-person arms are the selected character's combo arms")
	assert_eq(gun.graphic_name, VIEWMODEL_GUN_GRAPHIC,
			"the equipped definition's fpModel is submitted after the arms")
	# The arms carry their authored raw triplet for the rig's per-submit FP
	# writer (Avatar_SetArmsCamoCtrl runs before each arms submit, never at
	# load); the gun part is never a camo target.
	assert_eq(arms.avatar_part, ObjectModel.AVATAR_PART_ARMS)
	assert_eq(arms.avatar_camo, VIEWMODEL_ARMS_CAMO,
			"first-person arms carry the authored raw CTRL bytes")
	assert_eq(gun.avatar_part, ObjectModel.AVATAR_PART_NONE,
			"the arms' per-draw TEX_CAMO state does not leak into the gun")
	assert_true(arms.get_ctrl_values().is_empty(),
			"no FP CTRL writer runs at build time")
	var witness: FirstPersonArmsWitness = \
			world.local_player_first_person_arms_witness()
	assert_true(witness.is_valid())
	assert_ne(world.local_player_character_id(), 0,
			"the authority stamped the selected combo on the local player")
	assert_eq(witness.character_id, world.local_player_character_id())
	assert_eq(witness.arms_graphic, VIEWMODEL_ARMS_GRAPHIC)
	assert_eq(Array(witness.arms_camo), [17, 34, 51])
	arms.visible = false
	assert_false(world.local_player_first_person_arms_witness().is_valid(),
			"a hidden arms submit cannot witness a visible comparison frame")
	arms.visible = true
	container.visible = false
	assert_false(world.local_player_first_person_arms_witness().is_valid(),
			"arms hidden by their viewmodel ancestor cannot pass the witness")


func test_first_person_without_character_arms_submits_the_gun_alone() -> void:
	var root_dir := _stage_viewmodel_fixture("gun_alone", false)
	var world := WorldFixture.boot_minimal(self, root_dir)
	assert_true(world.set_local_player_weapon_by_name("WPN_TEST"))
	var container := world.build_local_player_viewmodel()
	assert_not_null(container)
	var parts := world.local_player_viewmodel_parts()
	assert_eq(parts.size(), 1)
	if parts.size() == 1:
		assert_eq((parts[0] as ObjectModel).graphic_name, VIEWMODEL_GUN_GRAPHIC,
				"no resolved character arms means no arms submit -- gfx1a is not a fallback "
				+ "[orig: Player_RenderFirstPersonViewModel @0x4df064/@0x4df06b]")
		assert_eq((parts[0] as ObjectModel).avatar_part, ObjectModel.AVATAR_PART_NONE)
	assert_true(world.local_player_first_person_model_available(),
			"the gun alone satisfies the first-person model gate")


func test_joiner_challenge_prewarm_loads_player_and_current_viewmodels_before_freeze() -> void:
	var root_dir := _stage_viewmodel_fixture("challenge_prewarm", true)
	var world := WorldFixture.boot_minimal(self, root_dir)
	assert_true(world.set_local_player_weapon_by_name("WPN_TEST"))
	var body := world.get_item_db().get_graphic(MissionObjectPlacer.PLAYER_VISUAL_ITEM_ID)
	assert_false(body.is_empty(), "the player body graphic resolves from the staged catalog")

	var resolved := world.prewarm_challenge_models()

	# The present snapshot's rows warm first (the minimal mission's only row is
	# the local player's own type -> the player body), then the explicit
	# player-body / current-gun / character-arms legs, in that order.
	assert_eq(Array(resolved), [body, body, VIEWMODEL_GUN_GRAPHIC, VIEWMODEL_ARMS_GRAPHIC],
		"the frozen 0x3D source includes every .3DI the first player frame would load "
		+ "(the character's arms, not a weapon.def field)")


func test_joiner_accepts_novaworld_advertised_mission_basename() -> void:
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	var root := ResourceRoot.new()
	var fixture_dir := ProjectSettings.globalize_path("res://../assets")
	assert_eq(root.set_root_dir(fixture_dir), OK)
	world.set_resource_root(root)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, "mnml.bms"), OK)
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.server_name = "Basename Host"
	host_options.mission_name = mission.get_mission_name()
	host_options.mission_file = "mnml"
	host_options.expansion = ""
	host_options.game_type = 0x30020
	host_options.max_players = 4
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))

	var err := world.load_mission_as_joiner(_join_target(
			"127.0.0.1", host.get_host_listen_port(), "stale_browse_hint"))
	assert_eq(err, OK)
	for _frame in range(600):
		host.step()
		await get_tree().process_frame
		if world.is_loaded():
			break
	assert_true(world.is_loaded(),
			"the host's S2C 0x7B basename reached the wire-driven load")
	assert_eq(world.get_loaded_mission_file(), "mnml.bms",
		"the authoritative basename is normalized for mission/text-table naming")
	world.unload()
	# The admission watchdog used to be a coroutine owned by NetSessionDrive.
	# Freeing its sole GameWorld owner while it awaited process_frame made Godot
	# resume a method whose class instance was already gone on the next frame.
	world.free()
	world = null
	await get_tree().process_frame
	assert_engine_error_count(0,
			"tearing down a successfully wire-loaded world leaves no suspended owner method")


func test_hide_foliage_toggles_dispatcher_visibility() -> void:
	# The F3 overlay's "Hide foliage" toggle routes here: it hides/shows the foliage
	# dispatcher node (whose ArrayMesh batches render the scattered vegetation).
	var world := GameWorld.new()
	var terrain := Terrain.new()
	terrain.name = "Terrain"
	var disp := FoliageDispatcher.new()
	disp.name = "FoliageDispatcher"
	terrain.add_child(disp)
	world.add_child(terrain)
	add_child_autofree(world)
	await get_tree().process_frame  # _ready wires _dispatcher from the named child

	assert_false(world.is_foliage_hidden(), "foliage is shown by default")
	assert_true(disp.visible, "...with the dispatcher visible")

	world.set_foliage_hidden(true)
	assert_true(world.is_foliage_hidden())
	assert_false(disp.visible, "hiding foliage hides the dispatcher (and its MultiMesh slots)")

	world.set_foliage_hidden(false)
	assert_false(world.is_foliage_hidden())
	assert_true(disp.visible, "showing foliage restores the dispatcher")


func test_tick_feeds_dispatcher_silhouette_anchors_from_the_sim() -> void:
	# The hide-in-grass anchor feed: every host tick routes the sim's
	# crouched/prone infantry positions into the foliage dispatcher's silhouette
	# tier [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dc2/0x5c7ded
	# (MoveOrder & 0x300), groundEntity gate @ 0x5c7dd5..0x5c7df7]. Driven by
	# the REAL local player's stance latches [orig: Player_PackInputStateToEntity
	# @ 0x4df6a7..0x4df6cd].
	var world := WorldFixture.boot_minimal(self)
	var disp := world.get_node("Terrain/FoliageDispatcher") as FoliageDispatcher
	var sim := world.get_sim()
	assert_true(bool(sim.has_local_player()), "the playable mission spawned its player")

	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	assert_eq(disp.silhouette_anchors, PackedVector3Array(),
		"a STANDING infantry entity never anchors the silhouette tier")

	assert_true(sim.request_local_player_stance(1))  # crouch (SELECT 169)
	# The SELECT latch crosses the input pump one frame after the request, so
	# the world loop needs two frames where the bare sim.step() needed one.
	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	assert_eq(disp.silhouette_anchors.size(), 1,
		"tick feeds the sim's anchor positions into the dispatcher's silhouette tier")
	if disp.silhouette_anchors.size() == 1:
		var player := sim.get_local_player_position()
		assert_lt(Vector2(disp.silhouette_anchors[0].x, disp.silhouette_anchors[0].z)
				.distance_to(Vector2(player.x, player.z)), 0.1,
			"the anchor is the crouched player's own Godot-space ground position")

	assert_true(sim.request_local_player_stance(0))  # stand (SELECT 172)
	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	assert_eq(disp.silhouette_anchors, PackedVector3Array(),
		"standing back up empties the anchor feed on the next tick")


func test_tick_clears_stale_silhouette_anchors_when_no_sim_anchors_remain() -> void:
	# The feed assigns unconditionally: a sim reporting no anchors must wipe
	# anchors left by an earlier frame, never leave grass clumps orbiting a
	# despawned player. (The old no-get_sim/null-sim runtime doubles are gone —
	# the typed _runtime seam always reaches a real Simulation; the
	# unconditional-assignment contract is observed on the real stack.)
	var world := WorldFixture.boot_minimal(self)
	var disp := world.get_node("Terrain/FoliageDispatcher") as FoliageDispatcher

	disp.silhouette_anchors = PackedVector3Array([Vector3(1.0, 2.0, 3.0)])  # stale
	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	assert_eq(disp.silhouette_anchors, PackedVector3Array(),
		"a standing-player sim clears stale anchors on the next tick")


func test_set_foliage_hidden_is_safe_without_a_dispatcher() -> void:
	# Before a world loads (or with no foliage), there is no dispatcher; the toggle must
	# just record intent and never crash. Deliberately a bare code-built world:
	# the packaged scene always ships a dispatcher, and _dispatcher stays a
	# get_node_or_null lookup.
	var world := GameWorld.new()
	var terrain := Terrain.new()
	terrain.name = "Terrain"
	world.add_child(terrain)
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_foliage_hidden(true)
	assert_true(world.is_foliage_hidden(), "the flag holds even with no dispatcher to act on")


func test_effect_warm_temporarily_lifts_and_restores_the_particle_switch() -> void:
	# The recording EffectWorld/director doubles pinned the warm pass as a call
	# ledger (warm -> advance -> render -> reset under a lifted switch, then the
	# reattach under the restored one). The real load runs that pass; its
	# observable consequences on the real objects pin the same contract: a
	# hidden EffectWorld's spawn facade interns nothing, so a catalog that IS
	# interned after a load that started hidden proves the warm spawned under a
	# LIFTED switch; no live group survives it (the runtime reset ran after the
	# warm snapshot); the switch is hidden again afterwards; and the persistent
	# item effect the reattach met under the RESTORED switch was deferred, not
	# lost -- it attaches exactly once when the preference lifts.
	var root_dir := _stage_item_fx_fixture("warm_switch", [
		{"id": 108002, "graphic": FX_GUN_GRAPHIC,
				"effect": FX_PERSISTENT_EFFECT, "userpoint": "MFlash01"},
	])
	var world := WorldFixture.make_world(self)
	world.set_particles_hidden(true)
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				mission.add_entity(
						MissionData.KIND_ITEM, 108002, Vector3(10, 20, 30), Vector3.ZERO)), OK)
	var effects := world.get_effect_world()
	assert_true(world.is_particles_hidden(), "the persistent preference survives the load")
	assert_true(effects.are_particles_hidden(),
			"the user's particle preference is restored before the load returns")
	assert_gt(effects.effect_count(), 0, "the staged catalog loaded")
	assert_gt(effects.interned_count(), 0,
			"the persistent gameplay preference cannot suppress load warming: "
			+ "the warm spawned (and interned) the catalog under a lifted switch")
	assert_eq(effects.live_group_count(), 0,
			"the warm snapshot's runtime values reset before the load returns")
	assert_true(effects.get_debug_group_report(true).is_empty(),
			"persistent item effects reattach under the restored preference: deferred, not spawned")
	var stats := world.get_item_effect_director().get_stats()
	assert_eq(stats.pending_static, 1,
			"the reattach saw the restored switch: the static source is deferred, not lost")
	assert_eq(stats.registered_static, 0)
	world.set_particles_hidden(false)
	assert_eq(_fx_unowned_rows(world, FX_PERSISTENT_EFFECT).size(), 1,
			"lifting the preference attaches the deferred persistent effect exactly once")
	stats = world.get_item_effect_director().get_stats()
	assert_eq(stats.registered_static, 1)
	assert_eq(stats.pending_static, 0)


func test_item_effect_attach_uses_the_original_pool_specific_gates() -> void:
	# Mission kinds preserve the original pool mapping. Pool 0 is not walked;
	# pool 1 skips attrib 0x42; pools 2/3 skip only powerup bit 0x2.
	# [orig: resolve_item_materials_and_spawn_bone_trails @ 0x522ee0,
	#  gates @ 0x523233 / @ 0x523272 / @ 0x5232af]
	# Every case is a mission record the REAL placer presents as an individual
	# ObjectModel (mount.3di: MFlash01 + a live PANM track) and the world's own
	# director walks at load. The pool-3 marker leg has no real twin (a placed
	# marker never materializes a node); its shared powerup-only gate is pinned
	# by the pool-2 building cases.
	var root_dir := _stage_item_fx_fixture("pool_gates", [
		{"id": 108001, "type": "person", "effect": FX_PERSISTENT_EFFECT, "userpoint": "MFlash01"},
		{"id": 108002, "effect": FX_PERSISTENT_EFFECT, "userpoint": "MFlash01"},
		{"id": 108003, "attribs": "PlayerControl",
				"effect": FX_PERSISTENT_EFFECT, "userpoint": "MFlash01"},
		{"id": 108004, "type": "building", "attribs": "Powerup",
				"effect": FX_PERSISTENT_EFFECT, "userpoint": "MFlash01"},
		{"id": 108005, "type": "building", "attribs": "PlayerControl",
				"effect": FX_PERSISTENT_EFFECT, "userpoint": "MFlash01"},
	])
	var cases := [
		[MissionData.KIND_ORGANIC, 108001, 0],
		[MissionData.KIND_ITEM, 108002, 1],
		[MissionData.KIND_ITEM, 108003, 0],
		[MissionData.KIND_BUILDING, 108004, 0],
		[MissionData.KIND_BUILDING, 108005, 1],
	]
	var world := WorldFixture.make_world(self)
	var placed: Array = []  # mutated (append), never reassigned: lambda captures copy locals
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				for case_index in range(cases.size()):
					var case: Array = cases[case_index]
					placed.append(mission.add_entity(int(case[0]), int(case[1]),
							Vector3(10 + 10 * case_index, 20, 0), Vector3.ZERO))), OK)
	var db := world.get_item_db()
	var nodes: Array[ObjectModel] = []
	for case_index in range(cases.size()):
		var case: Array = cases[case_index]
		var node := _placed_node(world, placed[case_index])
		nodes.append(node)
		if node == null:
			continue
		assert_eq(_fx_rows_for_node(world, node).size(), int(case[2]),
				"kind %d attrib 0x%x" % [int(case[0]), db.get_attrib(int(case[1]))])

	assert_eq(world.get_effect_world().get_debug_group_report().size(), 2,
			"only normal pool-1 plus allowed pool-2/3 entities attach")
	var anchor_info := _fx_anchor_info()
	var item_rows: Array = _fx_rows_for_node(world, nodes[1]) if nodes[1] != null else []
	if item_rows.size() == 1:
		var row: EffectGroupReport = item_rows[0]
		assert_eq(String(row.owner_key),
				"itemfx:%d:%d" % [nodes[1].get_instance_id(), _fx_anchor_index()],
				"the emitter is keyed to the model's matched MFlash01 point")
		assert_true(row.transform.origin
				.is_equal_approx(nodes[1].global_transform * anchor_info.position),
				"the effect anchors at the real model's MFlash01 point")
		assert_eq(row.binding, EffectScene.BINDING_FOLLOW_OWNER,
				"an entity-attached emitter follows its owner")
	# A registered attachment is never duplicated by a re-run of the attach
	# entry: neither the retry the particle switch triggers on re-enable nor a
	# replayed wire-node callback (the same guarded entry) touches a registered
	# node.
	world.set_particles_hidden(true)
	world.set_particles_hidden(false)
	assert_eq(world.get_effect_world().get_debug_group_report().size(), 2,
			"a replayed attach cannot duplicate an existing attach")
	if nodes[1] != null:
		world.get_item_effect_director().on_wire_node_spawned(
				nodes[1], MissionData.KIND_ITEM, 108002)
		assert_eq(_fx_rows_for_node(world, nodes[1]).size(), 1,
				"a replayed wire-node callback cannot duplicate an existing attach")

	# A persistent item first materialized while the retail master switch is off
	# must attach once when particles are re-enabled; a mission loaded hidden
	# otherwise loses that effect permanently.
	world.set_particles_hidden(true)
	world.unload()
	placed.clear()
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				placed.append(mission.add_entity(
						MissionData.KIND_ITEM, 108002, Vector3(10, 20, 0), Vector3.ZERO))
				placed.append(mission.add_entity(
						MissionData.KIND_ITEM, 108002, Vector3(20, 20, 0), Vector3.ZERO))), OK)
	assert_true(world.get_effect_world().get_debug_group_report(true).is_empty(),
			"a hidden load defers every persistent attachment")
	# A wire node may despawn while particles are disabled: the deferred entry
	# of a freed node is pruned instead of retried.
	var despawned := _placed_node(world, placed[1])
	if despawned != null:
		despawned.free()
	world.set_particles_hidden(false)
	assert_eq(world.get_effect_world().get_debug_group_report().size(), 1,
			"re-enable retries the hidden-at-load persistent attachment once; "
			+ "a node freed while hidden is pruned instead of retried")
	world.set_particles_hidden(false)
	assert_eq(world.get_effect_world().get_debug_group_report().size(), 1,
			"steady enabled state cannot duplicate it")
	assert_eq(world.get_item_effect_director().get_stats().pending_nodes, 0,
			"the retried and pruned entries leave nothing pending")


func test_dbuggy_fx00_follows_controller_lifecycle_with_pre_node_race() -> void:
	# Shipped DBuggy1.3di: ITEMS.DEF item 101291 is PlayerControl (0x40) and
	# authors Effect_whiteExhaust at model userpoint FX00. It stays dormant in
	# the mission-start pool walk, then follows controller occupancy events.
	# (The staged twin keeps the id and the PlayerControl attrib over the
	# synthetic mount.3di / MFlash01 / Buildup fixtures; the node reaches the
	# world's director the way the present pass's wire callback hands it over.)
	const DBUGGY_ITEM := 101291
	var root_dir := _stage_item_fx_fixture("dbuggy_lifecycle", [
		{"id": DBUGGY_ITEM, "attribs": "PlayerControl",
				"effect": FX_PERSISTENT_EFFECT, "userpoint": "MFlash01"},
	])
	var world := WorldFixture.boot_minimal(self, root_dir)
	var director := world.get_item_effect_director()
	watch_signals(world)

	var spawn_origin := int(Simulation.spawn_origin_pack(MissionData.KIND_ITEM, 3))
	var started := MissionEffect.make("vehicle_control_started", 71, 9001, spawn_origin)
	var stopped := MissionEffect.make("vehicle_control_stopped", 71, 9001, spawn_origin)

	# The simulation event can precede the present pass's wire/model callback.
	world.get_runtime().effects_drained.emit([started])
	assert_eq(director.get_stats().control_active, 3,
			"the start records every identity alias (net id, BMS id, spawn origin)")
	assert_signal_not_emitted(world, "mission_effects",
			"render-internal lifecycle events never leak to HUD consumers")
	var node := _fx_wire_node(world,
			EntityRef.make(MissionData.KIND_ITEM, 3, 9001, DBUGGY_ITEM))
	director.on_wire_node_spawned(node, MissionData.KIND_ITEM, DBUGGY_ITEM)
	assert_eq(director.get_stats().control_nodes, 1,
			"the PlayerControl node registers for its controller lifecycle")
	var rows := _fx_rows_for_node(world, node)
	assert_eq(rows.size(), 1,
			"a node presented after the start event attaches on arrival")
	var first_group := 0
	if rows.size() == 1:
		var row: EffectGroupReport = rows[0]
		first_group = int(row.id)
		assert_eq(row.name, FX_PERSISTENT_EFFECT)
		var anchor_info := _fx_anchor_info()
		assert_eq(String(row.owner_key),
				"itemfx:%d:%d" % [node.get_instance_id(), _fx_anchor_index()])
		assert_true(row.transform.origin
				.is_equal_approx(node.global_transform * anchor_info.position),
				"the effect anchors at the real model's MFlash01 point")

	# Replayed starts are idempotent; a single transition stop detaches the
	# exact native group the receipt-bearing facade created.
	world.get_runtime().effects_drained.emit([started])
	assert_eq(_fx_rows_for_node(world, node).size(), 1)
	world.get_runtime().effects_drained.emit([stopped])
	assert_true(_fx_rows_for_node(world, node).is_empty(),
			"the stop detaches the controller group")
	var detached := _fx_rows_for_node(world, node, true)
	assert_eq(detached.size(), 1)
	if detached.size() == 1:
		assert_eq(int((detached[0] as EffectGroupReport).id), first_group,
				"the stop detaches exactly the group the start created")
	assert_eq(director.get_stats().control_active, 0,
			"the stop retires every identity alias")

	# A later control transition can create a fresh group, and a mixed drain
	# exposes only the public effect after consuming the lifecycle row.
	world.get_runtime().effects_drained.emit([started])
	rows = _fx_rows_for_node(world, node)
	assert_eq(rows.size(), 1)
	if rows.size() == 1:
		assert_ne(int((rows[0] as EffectGroupReport).id), first_group,
				"a later control transition creates a fresh group")
	var public_effect := MissionEffect.make("text", 0, 0, 0, "still public")
	world.get_runtime().effects_drained.emit([stopped, public_effect])
	assert_true(_fx_rows_for_node(world, node).is_empty(),
			"the mixed drain's lifecycle row still detaches the group")
	assert_signal_emitted_with_parameters(
			world, "mission_effects", [[public_effect]])


func test_dbuggy_hidden_pending_is_cancelled_when_control_stops() -> void:
	const DBUGGY_ITEM := 101291
	var root_dir := _stage_item_fx_fixture("dbuggy_hidden_pending", [
		{"id": DBUGGY_ITEM, "attribs": "PlayerControl",
				"effect": FX_PERSISTENT_EFFECT, "userpoint": "MFlash01"},
	])
	var world := WorldFixture.make_world(self)
	world.set_particles_hidden(true)
	var placed := {}  # mutated (merge), never reassigned: lambda captures copy locals
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				placed.merge(mission.add_entity(
						MissionData.KIND_ITEM, DBUGGY_ITEM, Vector3(10, 20, 0), Vector3.ZERO))), OK)
	var node := _placed_node(world, placed)
	if node == null:
		return
	var director := world.get_item_effect_director()
	assert_true(world.get_effect_world().get_debug_group_report(true).is_empty(),
			"the unchanged mission-start 0x42 gate keeps PlayerControl dormant")
	var started := _control_effect("vehicle_control_started", 81, placed)
	var stopped := _control_effect("vehicle_control_stopped", 81, placed)
	# A start under the hidden switch defers the activation: the director's
	# pending count reads the deferral, and the group that appears -- or not --
	# when the switch lifts is its consequence.
	world.get_runtime().effects_drained.emit([started])
	assert_true(world.get_effect_world().get_debug_group_report(true).is_empty(),
			"a hidden activation spawns nothing yet")
	assert_eq(director.get_stats().pending_nodes, 1,
			"a hidden activation is deferred, not lost")
	world.get_runtime().effects_drained.emit([stopped])
	assert_eq(director.get_stats().pending_nodes, 0,
			"stop cancels a hidden activation before particles are re-enabled")
	world.set_particles_hidden(false)
	assert_true(_fx_rows_for_node(world, node, true).is_empty(),
			"stop cancels a hidden activation before particles are re-enabled: "
			+ "re-enable cannot resurrect a stopped controller attachment")
	# The control leg: a hidden activation still live when the switch lifts
	# attaches once, so the cancel above is a real cancel.
	world.set_particles_hidden(true)
	world.get_runtime().effects_drained.emit([started])
	assert_true(world.get_effect_world().get_debug_group_report(true).is_empty())
	assert_eq(director.get_stats().pending_nodes, 1)
	world.set_particles_hidden(false)
	assert_eq(_fx_rows_for_node(world, node).size(), 1,
			"a live hidden activation attaches once when particles are re-enabled")
	assert_eq(director.get_stats().pending_nodes, 0)


func test_controller_net_id_does_not_alias_a_wire_handle() -> void:
	const DBUGGY_ITEM := 101291
	var root_dir := _stage_item_fx_fixture("net_id_wire_alias", [
		{"id": DBUGGY_ITEM, "attribs": "PlayerControl",
				"effect": FX_PERSISTENT_EFFECT, "userpoint": "MFlash01"},
	])
	var world := WorldFixture.boot_minimal(self, root_dir)
	var director := world.get_item_effect_director()

	world.get_runtime().effects_drained.emit(
			[MissionEffect.make("vehicle_control_started", 77, 0, 0)])
	assert_eq(director.get_stats().control_active, 1,
			"the start records its simulation net id alias")
	var model := _fx_wire_node(world,
			EntityRef.make(MissionData.KIND_ITEM, 12, 0, DBUGGY_ITEM, 77))
	director.on_wire_node_spawned(model, MissionData.KIND_ITEM, DBUGGY_ITEM)
	assert_eq(director.get_stats().control_nodes, 1,
			"the wire node registers for its controller lifecycle")
	assert_true(_fx_rows_for_node(world, model).is_empty(),
			"event a is a simulation net id, not the presentation wire handle")


func test_synthetic_controller_effects_are_scoped_to_their_wire_sibling() -> void:
	const PLAYER_CONTROL_ITEM := 101291
	var root_dir := _stage_item_fx_fixture("wire_siblings", [
		{"id": PLAYER_CONTROL_ITEM, "attribs": "PlayerControl",
				"effect": FX_PERSISTENT_EFFECT, "userpoint": "MFlash01"},
	])
	var world := WorldFixture.boot_minimal(self, root_dir)
	var director := world.get_item_effect_director()

	var siblings: Array[ObjectModel] = []
	for wire_handle in [0x1004, 0x1005]:
		var ref := EntityRef.make(MissionData.KIND_ITEM, 0xffffff, 0,
				PLAYER_CONTROL_ITEM, wire_handle)
		ref.origin_kind = 0xff
		var model := _fx_wire_node(world, ref)
		director.on_wire_node_spawned(model, MissionData.KIND_ITEM, PLAYER_CONTROL_ITEM)
		siblings.append(model)
		assert_true(_fx_rows_for_node(world, model).is_empty(),
				"a synthetic emplacement attachment stays dormant until mounted")
	assert_eq(director.get_stats().control_nodes, 2)

	var start_first := MissionEffect.make("vehicle_control_started", 0, 0, -1)
	start_first.wire_handle = 0x1004
	world.get_runtime().effects_drained.emit([start_first])
	var first_rows := _fx_rows_for_node(world, siblings[0])
	assert_eq(first_rows.size(), 1,
			"mounting one synthetic emplacement starts only that sibling's effect")
	assert_true(_fx_rows_for_node(world, siblings[1]).is_empty())
	if first_rows.size() == 1:
		assert_eq(String((first_rows[0] as EffectGroupReport).owner_key),
				"itemfx:%d:%d" % [siblings[0].get_instance_id(), _fx_anchor_index()])

	var stop_first := MissionEffect.make("vehicle_control_stopped", 0, 0, -1)
	stop_first.wire_handle = 0x1004
	var start_second := MissionEffect.make("vehicle_control_started", 0, 0, -1)
	start_second.wire_handle = 0x1005
	world.get_runtime().effects_drained.emit([stop_first, start_second])
	assert_true(_fx_rows_for_node(world, siblings[0]).is_empty(),
			"the stop detaches exactly the mounted sibling's group")
	assert_eq(_fx_rows_for_node(world, siblings[0], true).size(), 1)
	var second_rows := _fx_rows_for_node(world, siblings[1])
	assert_eq(second_rows.size(), 1)
	if second_rows.size() == 1:
		assert_eq(String((second_rows[0] as EffectGroupReport).owner_key),
				"itemfx:%d:%d" % [siblings[1].get_instance_id(), _fx_anchor_index()],
				"the shared synthetic origin cannot activate a neighboring attachment")
	# The stopped sibling's group lingers detached until its particles die; the
	# live (attached) census is one group: the neighbor's, never both.
	var live := 0
	for row_v in world.get_effect_world().get_debug_group_report():
		if not (row_v as EffectGroupReport).detached:
			live += 1
	assert_eq(live, 1, "one live controller group: the neighbor's, never both")


func test_static_item_effects_spawn_world_bound_from_value_descriptors() -> void:
	# Real batched statics: the placer's value descriptors (StaticEffectSource)
	# feed the world's director at load. gun.3di authors MFlash01 (the matched
	# anchor); shed.3di authors no such point (the origin-fallback leg). The
	# first-16 mask RULE (duplicates, beyond-16 exclusion) is native and pinned
	# by the threedi user-point-mask ctest.
	var root_dir := _stage_item_fx_fixture("static_descriptors", [
		{"id": 108002, "graphic": FX_GUN_GRAPHIC,
				"effect": FX_PERSISTENT_EFFECT, "userpoint": "MFlash01"},
		{"id": 108003, "graphic": FX_GUN_GRAPHIC, "attribs": "PlayerControl",
				"effect": FX_PERSISTENT_EFFECT, "userpoint": "MFlash01"},
		{"id": 108009, "type": "building", "graphic": FX_SHED_GRAPHIC,
				"effect": FX_FALLBACK_EFFECT, "userpoint": "MFlash01"},
	])
	var world := WorldFixture.make_world(self)
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				mission.add_entity(
						MissionData.KIND_ITEM, 108002, Vector3(10, 20, 30), Vector3(0, 90, 0))
				mission.add_entity(
						MissionData.KIND_BUILDING, 108009, Vector3(-5, 6, 7), Vector3(0, 45, 0))
				# Pool-1 attrib 0x40 is excluded before any effect request.
				mission.add_entity(
						MissionData.KIND_ITEM, 108003, Vector3(50, 20, 0), Vector3.ZERO)), OK)
	assert_eq(world.get_mission_stats().batched, 3,
			"all three records ride the static populations (no individual node)")
	var entity_transform := MissionObjectPlacer.entity_transform(
			Vector3(10, 20, 30), Vector3(0, 90, 0))
	var fallback_transform := MissionObjectPlacer.entity_transform(
			Vector3(-5, 6, 7), Vector3(0, 45, 0))

	assert_eq(_fx_unowned_rows(world).size(), 2,
			"one matched anchor plus one origin fallback; the gated row is excluded")
	var matched := _fx_unowned_rows(world, FX_PERSISTENT_EFFECT)
	assert_eq(matched.size(), 1)
	if matched.size() == 1:
		var first: EffectGroupReport = matched[0]
		assert_eq(first.admission, EffectScene.ADMISSION_ALWAYS)
		assert_eq(first.binding, EffectScene.BINDING_WORLD)
		assert_eq(first.render_domain, EffectScene.RENDER_DOMAIN_WORLD)
		assert_null(first.owner_key, "static batches never invent follow owners")
		var anchor_info := _fx_gun_anchor_info()
		var first_transform: Transform3D = first.transform
		assert_true(first_transform.origin.is_equal_approx(
				entity_transform * anchor_info.position))
		var anchor_dir := anchor_info.rotation
		if anchor_dir.length_squared() > 0.000001:
			assert_true(first_transform.basis.z.normalized().is_equal_approx(
					(entity_transform.basis * anchor_dir).normalized()),
					"the authored direction composes through the entity basis")
	var fallback := _fx_unowned_rows(world, FX_FALLBACK_EFFECT)
	assert_eq(fallback.size(), 1)
	if fallback.size() == 1:
		var fallback_actual: Transform3D = (fallback[0] as EffectGroupReport).transform
		assert_true(fallback_actual.is_equal_approx(fallback_transform),
				"an unmatched userpoint falls back to the entity origin and basis")
	# The registered descriptors are never revisited: the retry the particle
	# switch triggers on re-enable skips them.
	world.set_particles_hidden(true)
	world.set_particles_hidden(false)
	assert_eq(_fx_unowned_rows(world).size(), 2,
			"revisiting the same descriptor index cannot duplicate its persistent effect")


func test_live_item_effect_owner_uses_each_fixed_ticks_value_pose() -> void:
	# The REAL MissionRoot pose chain: before the first completed logic tick
	# there is no present snapshot (the authored Node seeds the spawn); once a
	# tick completes, the owner follows the sim's client-view value pose; an
	# identity absent from the snapshot detaches (null). The old runtime double
	# also recorded that the entity_ref dictionary crossed the seam by identity;
	# that probe lived on the double and is gone — the pose values below only
	# resolve if the real seam consumed the same stable identity. The director's
	# resolver has no public twin, so the chain is read where it lands: the
	# attached group's pose after the effect world's owner sync, and the
	# runtime's own presented_entity_effect_transform seam for the absent leg.
	var root_dir := _stage_item_fx_fixture("item_owner_pose", [
		{"id": 108002, "effect": FX_PERSISTENT_EFFECT, "userpoint": "MFlash01"},
	])
	var world := WorldFixture.make_world(self)
	var placed := {}  # mutated (merge), never reassigned: lambda captures copy locals
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				placed.merge(mission.add_entity(
						MissionData.KIND_ITEM, 108002, Vector3(6, 4, 5), Vector3.ZERO))), OK)
	var node := _placed_node(world, placed)
	if node == null:
		return
	var effects := world.get_effect_world()
	var runtime := world.get_runtime()
	var anchor_offset: Vector3 = _fx_anchor_info().position
	assert_false(runtime.has_current_present_effect_snapshot(),
			"no logic tick has completed yet: there is no present snapshot")
	node.global_transform = Transform3D(Basis.IDENTITY, Vector3(99, 99, 99))
	effects.advance_fixed_tick(Simulation.tick_dt())
	var rows := _fx_rows_for_node(world, node)
	assert_eq(rows.size(), 1)
	if rows.size() == 1:
		var origin := (rows[0] as EffectGroupReport).transform.origin
		assert_true(origin.is_equal_approx(Vector3(99, 99, 99) + anchor_offset),
				"before the first sim snapshot, the authored Node remains the safe spawn seed")

	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	assert_true(runtime.has_current_present_effect_snapshot())
	var presented: Variant = runtime.presented_entity_effect_transform(node.entity_ref)
	assert_true(presented is Transform3D)
	rows = _fx_rows_for_node(world, node)
	assert_eq(rows.size(), 1)
	if rows.size() == 1 and presented is Transform3D:
		var value_pose := presented as Transform3D
		assert_almost_eq(value_pose.origin.x, 6.0, 0.01,
				"a fixed tick follows the current client-view value, not the stale presented Node")
		assert_almost_eq(value_pose.origin.z, -4.0, 0.01,
				"the value pose rides the canonical mission-to-Godot frame")
		var origin := (rows[0] as EffectGroupReport).transform.origin
		assert_true(origin.is_equal_approx(value_pose * anchor_offset),
				"the attached group rides the value pose after the tick")

	assert_null(runtime.presented_entity_effect_transform(
			EntityRef.make(MissionData.KIND_BUILDING, 999, 999999)),
			"an owner absent from this tick detaches instead of emitting once "
			+ "from stale presentation")


func test_static_item_effect_hidden_at_load_retries_once_when_enabled() -> void:
	var root_dir := _stage_item_fx_fixture("static_hidden", [
		{"id": 108002, "graphic": FX_GUN_GRAPHIC,
				"effect": FX_PERSISTENT_EFFECT, "userpoint": "MFlash01"},
	])
	var world := WorldFixture.make_world(self)
	world.set_particles_hidden(true)
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				mission.add_entity(
						MissionData.KIND_ITEM, 108002, Vector3(3, 4, 5), Vector3.ZERO)), OK)
	assert_true(world.get_effect_world().get_debug_group_report(true).is_empty(),
			"a world-bound persistent source survives a hidden mission load as values, unspawned")
	var director := world.get_item_effect_director()
	assert_eq(director.get_stats().pending_static, 1,
			"a world-bound persistent source survives a hidden mission load as values")
	world.set_particles_hidden(false)
	assert_eq(_fx_unowned_rows(world, FX_PERSISTENT_EFFECT).size(), 1,
			"re-enabling submits the deferred static source")
	assert_eq(director.get_stats().pending_static, 0)
	world.set_particles_hidden(false)
	assert_eq(_fx_unowned_rows(world, FX_PERSISTENT_EFFECT).size(), 1,
			"steady enabled state cannot duplicate it")


func test_blink_frame_gates_toggle_render_passes() -> void:
	# The blink letter gates (docs/render/render-occlusion-re.md §4): indoors
	# (accum bit 0x2) hides the terrain render — near detail + far foliage ride
	# the terrain node — and the sky dome + celestials; leaving restores them
	# [orig: render_main_scene @ 0x5c1353 (PolyTrn skip), the skybox skip
	# @ 0x5ca84f]. Driven end-to-end through the REAL sim by the mission's
	# force-indoors attribute [orig: Bms_AttribFlags & 0x10 @ 0x5ca1c8]; the
	# outdoors edge flips the same mission flag the load latched. The water
	# letter legs (accum bit 0x8) are gone with the sim doubles: letters
	# accumulate only inside authored blink boxes, which no fixture model
	# carries — the water gate keeps its witness in the [orig] cites of
	# occlusion_frame_pass.gd.
	var world := WorldFixture.boot_minimal(self, "", func(mission: MissionData) -> void:
		assert_true(mission.set_header_flag(MissionData.ATTRIB_FORCE_INDOORS, true)))
	var sky := world.get_node("SkyDome") as Node3D
	var water := world.get_node("Water") as Node3D
	var terrain := world.get_node("Terrain") as Node3D

	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	assert_false(terrain.visible, "indoors hides the terrain render")
	assert_false(sky.visible, "indoors skips the skybox pass")
	assert_true(water.visible, "the indoors bit alone leaves water on")

	# The outdoors edge: clear the mission attribute the load latched (the same
	# private the probe-skip test drives) and the letter gates restore.
	world.set("_mission_forces_indoors", false)
	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	assert_true(terrain.visible, "outdoors restores the terrain")
	assert_true(sky.visible, "outdoors restores the sky")
	assert_true(water.visible, "outdoors leaves the water on")

	# An unload while indoors must not leach into the next mission.
	world.set("_mission_forces_indoors", true)
	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	assert_false(sky.visible, "back indoors before the unload")
	world.unload()
	assert_true(terrain.visible, "unload restores the terrain gate")
	assert_true(sky.visible, "unload restores the sky gate")


func test_occlusion_frame_drives_building_visibility_from_the_sim() -> void:
	# The render-occlusion frame through the REAL stack (docs/render/
	# render-occlusion-re.md §3/§5): a mission-authored building enters the
	# sim's real occlusion world at boot, the per-frame camera drives its batch
	# verdict, and the pass lands the verdict on the REAL placed ObjectModel
	# [orig: Terrain_RenderSectorModels @ 0x5c5d30]. (The stub-era legs died
	# with the sim doubles: the exact section-mask word needs an OOBJ section
	# map no fixture model carries, the entity render gates need blink boxes,
	# and the g_BlinkWaterVisible override needs an authored water letter —
	# the native verdict/delta contracts behind them are pinned on the real sim
	# in simulation_test.gd.)
	var root_dir := _stage_building_fixture("occl_frame")
	var world := WorldFixture.make_world(self)
	var placed := {}  # mutated (merge), never reassigned: lambda captures copy locals
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				placed.merge(mission.add_entity(
						MissionData.KIND_BUILDING, 102001, Vector3(16, 24, 4), Vector3.ZERO))), OK)
	var bms_id := int(placed.get("bms_id", 0))
	assert_gt(bms_id, 0)
	var building := world.get_runtime().get_entity_index().resolve_single(bms_id) as Node3D
	assert_not_null(building, "the authored building placed a real ObjectModel")
	if building == null:
		return
	# Godot-space building position: mission (16, 24, 4) -> (16, 4, -24).
	var eye := Vector3(16, 6, 0)
	var toward := Transform3D(Basis.IDENTITY, eye)  # -Z forward: sees the building
	var away := Transform3D(Basis(Vector3.UP, PI), eye)  # +Z forward: it is behind

	world.tick(eye, toward, ONE_TICK_DELTA)
	assert_true(building.visible, "an in-frustum batched building stays visible")

	world.tick(eye, away, ONE_TICK_DELTA)
	assert_false(building.visible,
			"a frustum-culled batch verdict hides the placed building whole")

	world.tick(eye, toward, ONE_TICK_DELTA)
	assert_true(building.visible,
			"the release lands the building back on its present intent")

	# Unload restores the blink/occlusion state for the next mission (the
	# placed node itself is torn down with the MissionObjects container).
	world.tick(eye, away, ONE_TICK_DELTA)
	assert_false(building.visible)
	world.unload()


# (test_occlusion_never_resurrects_sim_hidden_nodes was deleted with the
# runtime/sim doubles: its subject — an occlusion RELEASE landing on the sim's
# CURRENT present intent — needed a sim that hides an entity mid-release, which
# only the deleted doubles could stage. The release-consult contract itself is
# pinned on the real sim by simulation_test.gd's
# test_occlusion_delta_calls_emit_changes_only (entity_present_visible) and the
# ObjectModel present/occlusion visibility-bit contract in mission_present_pass_test.)


func test_probe_occlusion_skip_restores_frame_state_and_keeps_iris_live() -> void:
	# The A/B probe seam on the REAL stack: entering the occlusion skip
	# releases every live occlusion claim (the behind-camera building restores
	# to its present intent) while the iris exposure feed keeps sampling into
	# the real weather node; leaving the skip re-arms the sim's delta baseline
	# and the next frame re-applies the claim. (The stub-era section-mask and
	# entity-cull legs died with the sim doubles — see the occlusion frame test
	# above for what the fixture world can witness.)
	var root_dir := _stage_building_fixture("occl_probe_skip")
	var world := WorldFixture.make_world(self)
	var placed := {}  # mutated (merge), never reassigned: lambda captures copy locals
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				placed.merge(mission.add_entity(
						MissionData.KIND_BUILDING, 102001, Vector3(16, 24, 4), Vector3.ZERO))), OK)
	var building := world.get_runtime().get_entity_index().resolve_single(
			int(placed.get("bms_id", 0))) as Node3D
	assert_not_null(building)
	if building == null:
		return
	var weather := world.get_weather_node() as Weather
	var eye := Vector3(16, 6, 0)
	var away := Transform3D(Basis(Vector3.UP, PI), eye)

	world.tick(eye, away, ONE_TICK_DELTA)
	assert_false(building.visible, "occlusion claims the behind-camera building")
	assert_eq(weather.iris_samples.size(), 3,
			"the real sim marches three iris samples into the weather node")
	assert_true((world.get("_perf_probe_spans") as Dictionary).is_empty(),
			"normal frames do not pay for or retain probe spans")

	world.set_perf_probe_enabled(true)
	world.set("_perf_probe_skip_occl", true)
	weather.iris_samples = PackedInt32Array()
	world.tick(eye, away, ONE_TICK_DELTA)
	assert_true(building.visible,
			"entering the skip releases the occlusion claim onto present intent")
	assert_eq(weather.iris_samples.size(), 3,
			"iris exposure still samples while occlusion is skipped")
	var spans: Dictionary = world.get("_perf_probe_spans")
	assert_eq(int(spans.get("occl_frame", -1)), 0,
			"a skipped phase reports zero rather than a stale prior span")
	assert_true(spans.has("iris"), "enabled probe frames publish the live iris span")

	world.set("_perf_probe_skip_occl", false)
	world.tick(eye, away, ONE_TICK_DELTA)
	assert_false(building.visible,
			"leaving the skip re-emits the claim from the sim's delta baseline")
	assert_gt(int((world.get("_perf_probe_spans") as Dictionary).get("occl_frame", -1)), -1,
			"leaving the skip resumes the render-occlusion frame span")


func test_tick_never_emits_session_lost_for_a_non_joiner() -> void:
	# A REAL single-player listen-server mission (is_joiner() false) must never
	# be treated as a lost session by the per-frame observer.
	var world := WorldFixture.boot_minimal(self)
	assert_false(bool(world.get_sim().is_joiner()))
	var reasons: Array = []
	world.session_lost.connect(func(reason: String) -> void: reasons.append(reason))
	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	assert_eq(reasons.size(), 0, "a non-joiner session never reports a loss")


func test_stats_board_captures_world_tick_legs_only_while_enabled() -> void:
	# The F3 Stats feeds (FrameStats): a disabled board costs the tick
	# nothing and receives nothing; an enabled one gets every world leg — the
	# occlusion split included — from the REAL playing stack, without touching
	# the probe dicts. The REAL sim carries the native occlusion split getters,
	# so the build/probe slots land and the glue slot is the bound-call
	# remainder (the stub-era "no split getters -> all glue" leg died with the
	# sim doubles).
	var world := WorldFixture.boot_minimal(self)
	var board := FrameStats.new()
	world.set_frame_stats(board)

	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	assert_eq(board.drain().sample_frames[FrameStats.OCCL_APPLY], 0,
			"a disabled board sees no feeds")

	board.set_capture_active(true)
	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	var counts := board.drain().sample_frames
	assert_gt(counts[FrameStats.OCCL_APPLY], 0, "the GDScript apply leg lands")
	assert_gt(counts[FrameStats.OCCL_GLUE], 0,
			"the bound-call remainder lands in the glue slot")
	assert_gt(counts[FrameStats.OCCL_BUILD], 0,
			"the real sim's native split feeds the build slot")
	assert_gt(counts[FrameStats.WORLD_WEATHER], 0)
	assert_gt(counts[FrameStats.WORLD_BLINK], 0)
	assert_gt(counts[FrameStats.WORLD_IRIS], 0)
	assert_gt(counts[FrameStats.WORLD_FOLIAGE], 0)
	assert_gt(counts[FrameStats.WORLD_RUNTIME], 0)
	assert_gt(counts[FrameStats.WORLD_AUDIO], 0)
	assert_true(world.is_water_render_stats_measured(),
			"capture enables the reflection viewport's render-time measurement")

	board.set_capture_active(false)
	assert_false(world.is_water_render_stats_measured(),
			"the capture edge tears measurement down without another world tick")
	board.set_capture_active(true)
	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	assert_true(world.is_water_render_stats_measured())
	world.set_frame_stats(null)
	assert_false(world.is_water_render_stats_measured(),
			"detaching the board also releases measurement immediately")


func _write_fixture_file(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "Fixture file should be writable: %s" % path)
	if file != null:
		file.store_string(text)
		file.close()


func _make_fixture_root(name: String) -> String:
	var root_dir := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join(
		"%s_%d" % [name, Time.get_ticks_usec()])
	assert_eq(DirAccess.make_dir_recursive_absolute(root_dir), OK)
	return root_dir


func _write_bytes(path: String, bytes: PackedByteArray) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "Fixture file should be writable: %s" % path)
	if file != null:
		file.store_buffer(bytes)
		file.close()


# The shared PFF3 fixture writer (TestPff.write), asserted here.
func _write_pff(path: String, entries: Array) -> void:
	assert_eq(TestPff.write(path, entries), OK, "PFF fixture should be writable: %s" % path)


# Stage the minimal fixture plus the armory.3di fixture (authored OOBJ
# records) as item 102001's GuardTwr1 graphic, so the placed building builds
# authored occluders.
func _stage_occluder_building_fixture(name: String) -> String:
	var root_dir := _staged(WorldFixture.stage_minimal_root(name))
	assert_eq(DirAccess.copy_absolute(
			ProjectSettings.globalize_path("res://../fixtures/threedi/synth/armory.3di"),
			root_dir.path_join("GuardTwr1.3di")), OK)
	_append_building_item(root_dir)
	return root_dir


func test_world_owns_the_occlusion_culling_switch() -> void:
	# Godot's occlusion consumer is a world-level decision (docs/render/
	# render-occlusion-re.md "Conservative device occluders"): off by default
	# (the pass costs more than it culls under the retail section verdict),
	# switched on live through the world's typed toggle only for an RD-backed
	# viewport, and reset by a load and an unload. No ObjectModel flips it.
	var root_dir := _stage_occluder_building_fixture("occl_switch")
	var world := WorldFixture.make_world(self)
	var viewport := world.get_viewport()
	viewport.use_occlusion_culling = true
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				mission.add_entity(
						MissionData.KIND_BUILDING, 102001, Vector3(16, 24, 4), Vector3.ZERO)), OK)
	var stats := world.get_mission_stats()
	assert_gt(stats.authored_occluder_models, 0,
			"the armory building placed authored occluders")
	assert_eq(world.get_authored_occluder_model_count(),
			stats.authored_occluder_models,
			"the debug row reads the placed occluder count from the world")
	assert_false(viewport.use_occlusion_culling,
			"a load re-applies the default: the consumer stays off")
	assert_false(world.is_occlusion_culling_enabled())
	var rd_backed := RenderingServer.get_rendering_device() != null
	world.set_occlusion_culling_enabled(true)
	assert_eq(viewport.use_occlusion_culling, rd_backed,
			"the toggle switches the consumer on only for an RD-backed viewport")
	assert_eq(world.is_occlusion_culling_enabled(), rd_backed)
	world.unload()
	assert_false(viewport.use_occlusion_culling,
			"unload switches the consumer off for the next mission")
