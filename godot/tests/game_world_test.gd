extends GutTest

const WORLD_TEST_ROOT := "game_world_test"
const ArmoryPresenter := preload("res://game/world/armory_presenter.gd")
const MissionPresentation := preload("res://game/world/mission_presentation.gd")
const FirstPersonArmsWitness := preload(
		"res://game/world/first_person_arms_witness.gd")


func after_each() -> void:
	_cleanup_fx_defs()
	TestFs.remove_dir_recursive(OS.get_cache_dir().path_join(WORLD_TEST_ROOT))


# (The Node runtime/sim doubles that used to live here — TransportRuntimeStub,
# ProfilingRuntimeStub, FxRuntimeStub, ItemPoseRuntimeStub, the anchor/blink/
# occlusion/joiner stubs — are gone: GameWorld._runtime is typed MissionPresentation
# and MissionPresentation._sim is typed Simulation, so every runtime-consuming
# test now boots the REAL stack through the public load path.)


class ViewmodelPlacerStub:
	extends RefCounted
	var graphics: Array[String] = []
	func resolve_player_visual_spec(_runtime_type_id: int,
			_character_id: int = 0) -> PlayerVisualSpec:
		return PlayerVisualSpec.new()  # fallback
	func build_model_from_graphic(graphic: String, _adm_name: String,
			_parent: Node3D, _clip_key: String, _env_node, _rig_graphic: String):
		graphics.append(graphic)
		return null


class SelectedAvatarViewmodelPlacerStub:
	extends RefCounted
	var graphics: Array[String] = []
	func resolve_player_visual_spec(_runtime_type_id: int,
			_character_id: int = 0) -> PlayerVisualSpec:
		var spec := PlayerVisualSpec.new()
		spec.fallback = false
		spec.arms = "SelectedArms"
		spec.arms_camo = Vector3i(17, 34, 51)
		return spec
	func build_model_from_graphic(graphic: String, _adm_name: String,
			parent: Node3D, _clip_key: String, _rig_graphic: String):
		graphics.append(graphic)
		var model := ObjectModel.new()
		model.name = graphic
		parent.add_child(model)
		return model


# A local player whose packed character id resolves to no combo (empty
# registry): retail submits the gun alone -- there is no weapon.def arms field.
class NoCharacterViewmodelPlacerStub:
	extends SelectedAvatarViewmodelPlacerStub
	func resolve_player_visual_spec(_runtime_type_id: int,
			_character_id: int = 0) -> PlayerVisualSpec:
		return PlayerVisualSpec.new()  # fallback


class ViewmodelWorldHarness:
	extends GameWorld
	var requested_def: PlayerViewmodelDef
	var model_availability: Array[bool] = []
	var fixture_character_id := 0x1234
	func install_viewmodel_fixture(def: PlayerViewmodelDef, placer) -> void:
		requested_def = def
		_placer = placer
	func local_player_viewmodel_def() -> PlayerViewmodelDef:
		return requested_def
	func local_player_character_id() -> int:
		return fixture_character_id
	func _set_local_player_first_person_model_available(available: bool) -> void:
		model_availability.append(available)


class ChallengePrewarmPlacerStub:
	extends RefCounted
	var loaded: Array[String] = []
	# The joiner's resolved character: its arms are the ONLY first-person arms
	# source (retail discards weapon.def gfx1a).
	func resolve_player_visual_spec(_runtime_type_id: int,
			_character_id: int = 0) -> PlayerVisualSpec:
		var spec := PlayerVisualSpec.new()
		spec.fallback = false
		spec.arms = "test_arms"
		return spec
	func resolve_player_visual_item_id(_runtime_type_id: int) -> int:
		return 101001
	func graphic_for(_item_id: int) -> String:
		return "player_body"
	func object_data_for(graphic: String):
		loaded.append(graphic)
		return null


class ChallengePrewarmWorldHarness:
	extends GameWorld
	var requested_def: PlayerViewmodelDef
	func install_prewarm_fixture(def: PlayerViewmodelDef, placer) -> void:
		requested_def = def
		_placer = placer
	func local_player_viewmodel_def() -> PlayerViewmodelDef:
		return requested_def
	func prewarm_challenge_models() -> void:
		_prewarm_loaded_model_challenge_definitions()


class ImpactAudioStub:
	extends MissionAudio
	var fires: Array = []
	func fire_soundset(set_name: String, world_pos: Vector3, _source_bms_id: int = 0) -> bool:
		fires.append({'name': set_name, 'position': world_pos})
		return true


class FxWorldStub:
	extends EffectWorld
	var spawns: Array = []
	var attached_spawns: Array = []
	var request_spawns: Array = []
	var stopped_groups: Array[int] = []
	var timeline: Array[String] = []
	func spawn_effect_request(effect: String, transform: Transform3D,
			options: EffectSpawnOptions = null) -> EffectSpawnReceipt:
		if are_particles_hidden():
			return EffectSpawnReceipt.make(false)
		request_spawns.append({
			"effect": effect,
			"transform": transform,
			"options": options if options != null else EffectSpawnOptions.new(),
		})
		return EffectSpawnReceipt.make(true, 1, request_spawns.size())
	func spawn_effect(effect: String, position: Vector3,
			orientation: Vector3 = Vector3.ZERO) -> int:
		spawns.append({
			'effect': effect,
			'position': position,
			'orientation': orientation,
		})
		return 1
	func spawn_effect_transient(effect: String, position: Vector3,
			orientation: Vector3 = Vector3.ZERO, initial_age_ticks: int = 0,
			render_domain: int = RENDER_DOMAIN_WORLD, source_tick: int = 0,
			source_order: int = 0) -> int:
		timeline.append("impact")
		spawns.append({
			'effect': effect,
			'position': position,
			'orientation': orientation,
			'initial_age_ticks': initial_age_ticks,
			'render_domain': render_domain,
			'source_tick': source_tick,
			'source_order': source_order,
		})
		return 1
	func advance_fixed_tick(_delta: float) -> void:
		timeline.append("advance")
	func spawn_effect_owned(owner_key: Variant, effect: String, position: Vector3,
			orientation: Vector3 = Vector3.ZERO) -> int:
		spawns.append({
			"owner": owner_key,
			"effect": effect,
			"position": position,
			"orientation": orientation,
		})
		return 1
	func spawn_effect_attached(owner_key: Variant, effect: String,
			initial_transform: Transform3D, local_pos: Vector3, local_dir: Vector3) -> int:
		var receipt := spawn_effect_attached_request(
				owner_key, effect, initial_transform, local_pos, local_dir)
		return receipt.effect_handle if receipt.spawned else 0
	func spawn_effect_attached_request(owner_key: Variant, effect: String,
			initial_transform: Transform3D, local_pos: Vector3,
			local_dir: Vector3) -> EffectSpawnReceipt:
		if are_particles_hidden():
			return EffectSpawnReceipt.make(false)
		attached_spawns.append({
			"owner": owner_key,
			"effect": effect,
			"transform": initial_transform,
			"local_pos": local_pos,
			"local_dir": local_dir,
		})
		return EffectSpawnReceipt.make(true, 1, attached_spawns.size())
	func stop_group(group_id: int) -> void:
		stopped_groups.append(group_id)


class ItemFxPlacerStub:
	extends RefCounted
	var item_db: ItemDatabase
	static func static_source(kind: int, item_id: int, graphic: String,
			world_transform: Transform3D, object_data: ObjectData) -> StaticEffectSource:
		var source := StaticEffectSource.new()
		source.kind = kind
		source.item_id = item_id
		source.graphic = graphic
		source.world_transform = world_transform
		source.object_data = object_data
		return source
	var static_sources: Array = []
	var static_light_draw_sources: Array = []
	func get_item_db() -> ItemDatabase:
		return item_db
	func get_static_item_effect_sources() -> Array:
		return static_sources.duplicate(true)
	func get_static_light_draw_sources() -> Array:
		return static_light_draw_sources.duplicate(true)


# --- Real item-fx fixtures (ADR 0034): the item database is an authored
# items.def parsed by the REAL ItemDatabase; the model data is the committed
# mount.3di (its MFlash01 user point anchors the effect) or shed.3di (no
# matching point -> the origin-fallback leg). The first-16 mask RULE itself is
# native and pinned by the threedi user-point-mask ctest.
static var _fx_data_cache: Dictionary = {}
var _fx_tmp_defs: Array[String] = []


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


func _fx_plain_data() -> ObjectData:
	return _fx_object_data("res://../fixtures/threedi/synth", "shed.3di")


# The anchor point's index/info on the real model (MFlash01 on mount).
func _fx_anchor_index() -> int:
	var mask := int(_fx_anchor_data().get_user_point_bone_mask("MFlash01"))
	assert_gt(mask, 0, "mount authors the MFlash01 user point in the first 16")
	for i in range(16):
		if (mask & (1 << i)) != 0:
			return i
	return -1


func _fx_anchor_info() -> ModelUserPoint:
	return _fx_anchor_data().get_user_point_info(_fx_anchor_index())


func _fx_model(parent: Node) -> ObjectModel:
	# Parent FIRST: set_object_data builds render children against the live
	# global transform, which needs the node inside the tree.
	var model := ObjectModel.new()
	parent.add_child(model)
	model.set_object_data(_fx_anchor_data())
	return model


# rows: [{id, attribs (optional token string), effect+userpoint (optional)}]
func _fx_item_db(rows: Array) -> ItemDatabase:
	var text := ""
	for row_v in rows:
		var row: Dictionary = row_v
		text += 'begin "fx item"
  id %d
  type object
' % int(row.get("id", 0))
		var attribs := String(row.get("attribs", ""))
		if not attribs.is_empty():
			text += "  attrib: %s
" % attribs
		var effect := String(row.get("effect", ""))
		if not effect.is_empty():
			text += "  particlefx %s %s
" % [effect, String(row.get("userpoint", ""))]
		text += "end

"
	var path := OS.get_temp_dir().replace("\\", "/") + 			"/opennova_fx_items_%d_%d.def" % [Time.get_ticks_usec(), _fx_tmp_defs.size()]
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file)
	if file != null:
		file.store_string(text)
		file.close()
	_fx_tmp_defs.append(path)
	var db := ItemDatabase.new()
	assert_eq(db.load(path), OK, "the authored item-fx items.def parses")
	return db


func _cleanup_fx_defs() -> void:
	for path in _fx_tmp_defs:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(path)
	_fx_tmp_defs.clear()


class ItemFxDirectorProbe:
	extends ItemEffectDirector
	# Probe wrappers over the REAL director's private internals: the pokes stay
	# implicit-self inside the subclass, keeping the private-poke ratchet flat.
	func attach_to_node(node: ObjectModel, kind: int, item_id: int) -> int:
		return _attach_item_effect_to_node(node, kind, item_id)
	func attach_to_static(source: StaticEffectSource, source_index: int) -> int:
		return _attach_item_effect_to_static(source, source_index)
	func pending_node_count() -> int:
		return _item_fx_pending_nodes.size()
	func pending_static_count() -> int:
		return _item_fx_pending_static.size()
	func deferred_control_count() -> int:
		return _item_fx_control_nodes.size()
	func active_identity_count() -> int:
		return _item_fx_control_active.size()
	func seed_owner(key: String, node: Node3D, entity_ref: EntityRef) -> void:
		_item_fx_nodes[key] = node
		_item_fx_owner_refs[key] = entity_ref
	func resolve_owner(key: String) -> Variant:
		return _effect_owner_transform(key)


class ItemFxGameWorldHarness:
	extends GameWorld
	# _item_fx is the world's sanctioned director-injection seam (like the
	# _runtime seam below): swap in a probe subclass of the real director so
	# tests drive the extracted code through the world's own wiring.
	var fx: ItemFxDirectorProbe
	func _init() -> void:
		# GameWorld._init first: defining _init here otherwise SKIPS the
		# world's internal child construction (_net_drive / _debug_views /
		# _occlusion / the real director), which the mission load path needs.
		super()
		fx = ItemFxDirectorProbe.new()
		fx.setup(self,
				func() -> Array:
					return _placer.get_static_item_effect_sources() if _placer != null else [],
				func() -> Variant:
					return _placer.get_item_db() if _placer != null else null)
		_item_fx = fx
	func configure_item_fx(effects: EffectWorld, placer: RefCounted) -> void:
		_effect_world = effects
		_placer = placer
	func present_item_fx(node: ObjectModel, kind: int, item_id: int) -> int:
		return fx.attach_to_node(node, kind, item_id)
	func attach_all_item_fx() -> void:
		fx.reattach()
	func present_static_item_fx(source: StaticEffectSource, source_index: int) -> int:
		return fx.attach_to_static(source, source_index)
	func pending_item_fx_count() -> int:
		return fx.pending_node_count()
	func pending_static_item_fx_count() -> int:
		return fx.pending_static_count()
	func deferred_control_item_fx_count() -> int:
		return fx.deferred_control_count()
	func active_control_identity_count() -> int:
		return fx.active_identity_count()
	func consume_runtime_effects(effects: Array) -> void:
		_on_runtime_effects(effects)
	func configure_item_owner(key: String, node: Node3D,
			entity_ref: EntityRef) -> void:
		# The runtime the resolve consults is the REAL MissionPresentation the world
		# built in _start_runtime — the typed seam admits nothing else.
		fx.seed_owner(key, node, entity_ref)
	func resolve_item_owner(key: String) -> Variant:
		return fx.resolve_owner(key)


# Impact-routing harness: the runtime/sim stack is REAL (loaded through the
# public mission path — _add_engine_children makes a code-built GameWorld
# mission-loadable). Only the two presentation SINKS swap for recording
# SUBCLASSES of their real classes, through implicit-self privates. No _init
# here: defining one would shadow GameWorld._init and skip the internal
# child construction (_net_drive / _debug_views / _occlusion / _item_fx).
class ImpactGameWorldHarness:
	extends GameWorld
	func install_probes(effects: EffectWorld, audio: MissionAudio) -> void:
		_effect_world = effects
		_mission_audio = audio


class WarmEffectWorldStub:
	extends EffectWorld
	var hidden_during_warm := true
	var calls: Array[String] = []
	var visibility_changes: Array[bool] = []

	func set_particles_hidden(hidden: bool) -> void:
		visibility_changes.append(hidden)
		super.set_particles_hidden(hidden)

	func warm_all_effects(_position: Vector3) -> int:
		hidden_during_warm = are_particles_hidden()
		calls.append("warm")
		return 1

	func advance_fixed_tick(_delta: float) -> void:
		calls.append("advance")

	func render_now() -> int:
		calls.append("render")
		return 1

	func reset_runtime_state() -> void:
		calls.append("reset")


class WarmItemFxStub:
	extends ItemEffectDirector
	# Director double for the warm-pass ordering probe: reattach() records the
	# particle switch it ran under instead of attaching real item effects.
	var attached_while_hidden := false

	func reattach() -> void:
		var effect_world: EffectWorld = _world.get_effect_world()
		attached_while_hidden = effect_world.are_particles_hidden()


class WarmGameWorldHarness:
	extends GameWorld
	# Injects a director double through the sanctioned _item_fx seam (the
	# overridable-hook role the world's own _attach_item_effects used to play).
	var fx_stub := WarmItemFxStub.new()

	var attached_while_hidden: bool:
		get:
			return fx_stub.attached_while_hidden

	func _init() -> void:
		fx_stub.setup(self, Callable(), Callable())
		_item_fx = fx_stub

	func configure_warm_effects(effects: EffectWorld) -> void:
		_effect_world = effects

	func warm_effect_catalog() -> int:
		return load_stages().warm_effect_world_catalog()


# The staged ammo.def for the impact-routing tests: the minimal fixture rows
# plus authored effects_table blocks, so a real terrain hit resolves a real
# per-surface impact row through the witnessed bake (world/ammo_table.h). Every
# plausible minimal-terrain surface tag carries the same pair so the assert is
# surface-agnostic; AM_SOUNDONLY authors 'none' effects (sound-only rows).
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
		dirt          Effect_AmHitDirt    imp_bullet_dirt   15
		grass         Effect_AmHitDirt    imp_bullet_dirt   15
		snow          Effect_AmHitDirt    imp_bullet_dirt   15
		cement        Effect_AmHitDirt    imp_bullet_dirt   15
		sand          Effect_AmHitDirt    imp_bullet_dirt   15
		packeddirt    Effect_AmHitDirt    imp_bullet_dirt   15
		stone         Effect_AmHitDirt    imp_bullet_dirt   15
		mud           Effect_AmHitDirt    imp_bullet_dirt   15
		water         Effect_AmHitDirt    imp_bullet_dirt   15
		uwaterdeep    Effect_AmHitDirt    imp_bullet_dirt   15
		uwatershallow Effect_AmHitDirt    imp_bullet_dirt   15
		uwatersurface Effect_AmHitDirt    imp_bullet_dirt   15
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


# Load the minimal fixture mission onto `world` through the PUBLIC path: the
# REAL MissionPresentation + Simulation stack (no doubles can enter the typed
# _runtime seam). `mutator` edits the opened document before the load.
func _load_minimal_mission(world: GameWorld, root_dir: String = "",
		mutator: Callable = Callable()) -> void:
	if root_dir.is_empty():
		root_dir = ProjectSettings.globalize_path("res://../assets")
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK)
	world.set_resource_root(root)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, "mnml.bms"), OK)
	if mutator.is_valid():
		mutator.call(mission)
	assert_eq(world.load_mission_data(mission, "mnml.bms"), OK)


# The engine children a code-built GameWorld needs before entering the tree:
# the typed mission path drives $Terrain directly and stamps
# _env.light_state onto every placed batch.
func _add_engine_children(world: GameWorld) -> void:
	var terrain := Terrain.new()
	terrain.name = "Terrain"
	world.add_child(terrain)
	var env := MissionEnvironment.new()
	env.name = "MissionEnvironment"
	world.add_child(env)


# Stage the minimal fixture over the synthetic Tmap terrain plus the staged
# impact ammo. The minimal mnml map is flat, so rounds would never ground on
# it; Tmap carries the relief the round flight can actually hit.
func _stage_impact_fixture(name: String) -> String:
	var root_dir := _make_fixture_root(name)
	for source_dir in [
		ProjectSettings.globalize_path("res://../fixtures/terrain/tmap"),
		ProjectSettings.globalize_path("res://../assets"),
	]:
		for file_name in DirAccess.get_files_at(source_dir):
			assert_eq(DirAccess.copy_absolute(
				source_dir.path_join(file_name), root_dir.path_join(file_name)), OK)
	_write_fixture_file(root_dir.path_join("ammo.def"), IMPACT_AMMO_DEF)
	return root_dir


# The staged harness item for KIND_BUILDING tests. The shipped assets/items.def
# carries only what the minimal mission places or the engine spawns by fixed id,
# so the staged root appends its own catalogue entry for the fixture model the
# stagers copy in as GuardTwr1.3di. CRLF: retail's .def parser stops at a bare LF.
const BUILDING_ITEM_DEF := "\r\nbegin \"Guard Tower\"\r\n  id 102001\r\n  type building\r\n  graphic GuardTwr1\r\n  sid guardtwr1\r\n  anim_def GuardTwr1\r\n  husk GuardTwr1X\r\n  hp 5000\r\nend\r\n"


func _append_building_item(root_dir: String) -> void:
	var f := FileAccess.open(root_dir.path_join("items.def"), FileAccess.READ_WRITE)
	assert_not_null(f, "the staged root carries items.def to append to")
	f.seek_end(0)
	f.store_string(BUILDING_ITEM_DEF)
	f.close()


# Stage the minimal fixture plus the house.3di collision fixture as item
# 102001's GuardTwr1 graphic, so authored KIND_BUILDING entities place a REAL
# ObjectModel and enter the sim's real collision/occlusion world.
func _stage_building_fixture(name: String) -> String:
	var root_dir := _stage_minimal_fixture(name)
	assert_eq(DirAccess.copy_absolute(
			ProjectSettings.globalize_path("res://../fixtures/threedi/synth/house.3di"),
			root_dir.path_join("GuardTwr1.3di")), OK)
	_append_building_item(root_dir)
	return root_dir


func _stage_lit_building_fixture(name: String) -> String:
	var root_dir := _stage_minimal_fixture(name)
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
	var world := _make_world()
	add_child_autofree(world)
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
	var world := _make_world()
	add_child_autofree(world)
	for presenter_name in ["Weather", "SkyDome", "Celestial", "Water", "SunShadow"]:
		var presenter := world.get_node_or_null(NodePath(presenter_name)) as Node
		assert_not_null(presenter, "%s exists under the world" % presenter_name)
		if presenter != null:
			assert_false(presenter.is_processing(),
					"%s must be pipeline-clocked, never self-clocked" % presenter_name)
			assert_false(presenter.is_physics_processing(),
					"%s must not self-clock on the physics tick either" % presenter_name)


## A first-person viewmodel definition authored by hand (the record the
## weapon.def slice decodes to): the gun model, the flags, no arms — the FP arms
## are the character's, never a weapon.def field (retail parses-and-discards
## gfx1a/gfx1b [orig: WeaponDefs_ParseLineCallback @0x5448d0/@0x5448e6]).
func _viewmodel_def(name: String, gfx1: String, flags: int) -> PlayerViewmodelDef:
	var def := PlayerViewmodelDef.new()
	def.weapon_name = name
	def.gfx1 = gfx1
	def.flags = flags
	return def


func test_manual_perf_probe_routes_through_the_public_runtime_gate() -> void:
	var world := _make_world()
	add_child_autofree(world)
	_load_minimal_mission(world)
	var sim := world.get_sim()
	assert_not_null(sim)
	assert_false(bool(sim.get_runtime_perf_counters().get("runtime_profiling_enabled", true)),
			"a fresh mission runtime starts with the native probe timer off")

	world.set_perf_probe_enabled(true)
	assert_true(bool(sim.get_runtime_perf_counters().get("runtime_profiling_enabled", false)),
			"GameWorld forwards consumer intent through MissionPresentation's public seam")
	world.set_perf_probe_enabled(false)
	assert_false(bool(sim.get_runtime_perf_counters().get("runtime_profiling_enabled", true)),
			"disabling the probe releases the native timer through the same seam")


func test_perf_counters_estimate_the_retained_instance_uniform_geometry() -> void:
	var world := _make_world()
	add_child_autofree(world)
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
	_load_minimal_mission(world, root_dir, func(mission: MissionData) -> void:
		mission.add_entity(
				MissionData.KIND_BUILDING, 102001, Vector3(16, 24, 4), Vector3.ZERO))
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
	var world := _make_world()
	add_child_autofree(world)
	_load_minimal_mission(world)
	var runtime: MissionPresentation = world.get_runtime()
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
# row lands in the recording sinks. Returns the number of world frames run.
func _tick_until_impact(world: GameWorld, effects: FxWorldStub,
		audio: ImpactAudioStub) -> int:
	for frame in range(30):
		world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
		if not audio.fires.is_empty() or not effects.spawns.is_empty():
			return frame + 1
	return 30


func test_round_light_move_rows_reach_world_selected_output() -> void:
	var root_dir := _stage_impact_fixture("round_light_move")
	var world := _make_world()
	add_child_autofree(world)
	_load_minimal_mission(world, root_dir, func(mission: MissionData) -> void:
		assert_true(mission.set_header_string("terrain", "Tmap")))
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
	var world := ImpactGameWorldHarness.new()
	_add_engine_children(world)
	add_child_autofree(world)
	_load_minimal_mission(world, root_dir, func(mission: MissionData) -> void:
		assert_true(mission.set_header_string("terrain", "Tmap")))
	var effects := FxWorldStub.new()
	world.add_child(effects)
	var audio := ImpactAudioStub.new(null, null)
	world.install_probes(effects, audio)

	var sim := world.get_sim()
	assert_gte(int(sim.debug_spawn_round(
			Vector3(16, 60, -16), Vector3.DOWN, "AM_556MM")), 0,
			"the real flight sim accepts the staged rifle round")
	_tick_until_impact(world, effects, audio)

	assert_eq(effects.spawns.size(), 1,
			"one real terrain hit presents exactly one impact transient")
	if effects.spawns.size() == 1:
		var spawn: Dictionary = effects.spawns[0]
		assert_eq(String(spawn.get("effect", "")), "Effect_AmHitDirt",
				"the surface row's authored .ptl effect reaches the effect world")
		assert_lt((spawn.get("orientation", Vector3.ZERO) as Vector3).distance_to(
				Vector3.DOWN), 0.05,
				"the transient carries the real incoming flight direction")
		# The engine stamps imp.tick DURING the producing tick and bumps
		# logic_tick before the shell's fixed-tick drain runs, so a same-frame
		# impact reads age 1 (one counter bump), never real catch-up aging.
		assert_lte(int(spawn.get("initial_age_ticks", -1)), 1,
				"an impact presented in its production frame carries no catch-up aging")
		assert_gte(int(spawn.get("initial_age_ticks", -1)), 0)
		assert_eq(int(spawn.get("render_domain", -1)),
				EffectScene.RENDER_DOMAIN_WORLD)
		assert_gt(int(spawn.get("source_tick", 0)), 0,
				"the row carries its production tick for catch-up chronology")
	assert_eq(audio.fires.size(), 1)
	if audio.fires.size() == 1 and effects.spawns.size() == 1:
		assert_eq(String(audio.fires[0].get("name", "")), "imp_bullet_dirt",
				"impact audio fires the same surface row's soundset")
		assert_eq(audio.fires[0].get("position", Vector3.ZERO),
				effects.spawns[0].get("position", Vector3.INF),
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
	assert_eq(effects.spawns.size(), 1,
			"resolved impact rows cannot accumulate between presentation frames")
	assert_eq(audio.fires.size(), 1)
	world.unload()


func test_round_impacts_route_sound_only_without_a_particle() -> void:
	var root_dir := _stage_impact_fixture("impact_sound_only")
	var world := ImpactGameWorldHarness.new()
	_add_engine_children(world)
	add_child_autofree(world)
	_load_minimal_mission(world, root_dir, func(mission: MissionData) -> void:
		assert_true(mission.set_header_string("terrain", "Tmap")))
	var effects := FxWorldStub.new()
	world.add_child(effects)
	var audio := ImpactAudioStub.new(null, null)
	world.install_probes(effects, audio)

	assert_gte(int(world.get_sim().debug_spawn_round(
			Vector3(16, 60, -16), Vector3.DOWN, "AM_SOUNDONLY")), 0)
	_tick_until_impact(world, effects, audio)

	assert_true(effects.spawns.is_empty(),
			"a 'none' effect column must not spawn an impact particle")
	assert_eq(audio.fires.size(), 1)
	if audio.fires.size() == 1:
		assert_eq(String(audio.fires[0].get("name", "")), "imp_gren_dirt",
				"the sound-only surface row retains its authored soundset")


func test_fixed_tick_orders_weapon_and_impact_before_particle_advance() -> void:
	var root_dir := _stage_impact_fixture("impact_order")
	var world := ImpactGameWorldHarness.new()
	_add_engine_children(world)
	add_child_autofree(world)
	_load_minimal_mission(world, root_dir, func(mission: MissionData) -> void:
		assert_true(mission.set_header_string("terrain", "Tmap")))
	var effects := FxWorldStub.new()
	world.add_child(effects)
	var audio := ImpactAudioStub.new(null, null)
	world.install_probes(effects, audio)
	world.set_local_player_weapon_tick_consumer(
			func(_events: Array[PlayerWeaponEvent]) -> void:
				effects.timeline.append("weapon"))

	assert_gte(int(world.get_sim().debug_spawn_round(
			Vector3(16, 60, -16), Vector3.DOWN, "AM_556MM")), 0)
	_tick_until_impact(world, effects, audio)

	assert_true(effects.timeline.has("impact"), "the real round impacted")
	assert_eq(effects.timeline.slice(effects.timeline.size() - 3),
			["weapon", "impact", "advance"],
			"the source tick is presented chronologically before its particle pass")
	assert_eq(effects.spawns.size(), 1, "the terrain hit presented its transient")
	if effects.spawns.size() == 1:
		# Same-frame drain: the logic counter has already bumped once past the
		# row's production tick (see the generic routing test above).
		assert_lte(int(effects.spawns[0].initial_age_ticks), 1,
				"a physical collision is visible in its production frame without catch-up aging")


func test_fx2ssn_routes_position_owner_and_terrain_orientation() -> void:
	var root_dir := _stage_building_terrain_fixture("fx2ssn")
	var world := ImpactGameWorldHarness.new()
	_add_engine_children(world)
	add_child_autofree(world)
	var placed := {}  # mutated (merge), never reassigned: lambda captures copy locals
	_load_minimal_mission(world, root_dir, func(mission: MissionData) -> void:
		assert_true(mission.set_header_string("terrain", "Tmap"))
		placed.merge(mission.add_entity(
				MissionData.KIND_BUILDING, 102001, Vector3(6, 4, 5), Vector3.ZERO)))
	var ssn := int(placed.get("bms_id", 0))
	assert_gt(ssn, 0, "the authored building carries a WAC/BMS-addressable SSN")
	var effects := FxWorldStub.new()
	world.add_child(effects)
	world.install_probes(effects, null)

	world.route_mission_effects([{"kind": "fx2ssn", "b": ssn, "str": "Dust"}])
	assert_eq(effects.spawns.size(), 1)
	if effects.spawns.is_empty():
		return
	assert_eq(effects.spawns[0].owner, ssn)
	assert_eq(effects.spawns[0].effect, "Dust")
	var spawn_pos: Vector3 = effects.spawns[0].position
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
	var orientation: Vector3 = effects.spawns[0].orientation
	assert_lt(orientation.distance_to(expected), 0.00001,
			"fx2ssn receives the terrain cell's recovered surface normal")
	assert_gt(orientation.distance_to(Vector3.UP), 0.01,
			"the ramp witness cannot regress to the old UP placeholder")


func test_round_outcome_effects_pass_through_to_hud_consumers() -> void:
	# The round-outcome trio (win/lose/round_end) is produced sim-side [orig: the WAC
	# win/lose handlers @0x4ed4a0/@0x4ed3f0 + Server_ProcessRoundEnd @0x5164f0] and is
	# host presentation on this side: the router must pass every row through to
	# mission_effects (the HUD banner + the shell's end-of-mission flow consume there).
	var world := _make_item_fx_world()
	add_child_autofree(world)
	watch_signals(world)
	var rows := [
		{"kind": "lose", "a": 0, "str": "STRMISC_KILLEDGREEN"},
		{"kind": "win", "a": 1},
		{"kind": "round_end", "a": 2},
	]
	world.consume_runtime_effects(rows)
	assert_signal_emitted_with_parameters(world, "mission_effects", [rows])


func test_load_world_requires_hardcoded_environment_in_global_root() -> void:
	var root := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join("missing_env_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root)
	# Pass the runtime archive gate so this fixture reaches the missing-environment contract.
	_write_pff(root.path_join("resource.pff"), [])
	_write_fixture_file(root.path_join("Tmap.trn"), "terrain_name \"Tmap\"\n")

	var world := _make_world()
	add_child_autofree(world)
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
	for child_name in ["Terrain", "MissionEnvironment", "SkyDome", "Weather", "Water", "Celestial"]:
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
	var world := _make_world()
	add_child_autofree(world)
	_load_minimal_mission(world, root_dir, func(mission: MissionData) -> void:
		mission.add_entity(MissionData.KIND_BUILDING, 102001,
				Vector3(6, 4, 5), Vector3.ZERO))
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
	var root_dir := _stage_minimal_fixture("first_armory_open")
	for rel in staged:
		var target := root_dir.path_join(staged[rel])
		if FileAccess.file_exists(target):
			assert_eq(DirAccess.remove_absolute(target), OK)
		assert_eq(DirAccess.copy_absolute(RetailData.fixture(rel), target), OK)

	var world := _make_world()
	add_child_autofree(world)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK)
	world.set_resource_root(root)
	world.set_local_player_spawn_loadout({
		"primary": "WPN_M4AUTO",
		"accessory": "WPN_SATCHEL_CHARGE",
		"player_class": 8,
	})
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
	presenter.setup(world, null, overlay)
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
	var world := _make_world()
	var camera := Camera3D.new()
	world.add_child(camera)
	add_child_autofree(world)
	camera.make_current()
	world.set_playable(false)
	_load_minimal_mission(world)

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
	var world := _make_world()
	var camera := Camera3D.new()
	world.add_child(camera)
	add_child_autofree(world)
	camera.make_current()
	world.set_playable(false)
	_load_minimal_mission(world)
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
	var world := _make_world()
	add_child_autofree(world)
	assert_true(world.is_playable(),
			"normal and F6 standalone launches spawn a player by default")
	world.set_playable(false)
	assert_false(world.is_playable(),
			"diagnostic fixtures can explicitly opt out of local-player setup")


func test_load_mission_data_rejects_an_empty_document() -> void:
	var world := _make_world()
	add_child_autofree(world)
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
	assert_eq(int(world.get_runtime().get_perf_counters().get("ticks", 0)), 8,
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
		sim_ticks += int(world.get_runtime().get_perf_counters().get("ticks", 0))
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

	var world := _make_world()
	add_child_autofree(world)
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
	var world := _make_world()
	add_child_autofree(world)
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
	var world := _make_world()
	add_child_autofree(world)
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

	var world := _make_world()
	add_child_autofree(world)
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

	var world := _make_world()
	add_child_autofree(world)
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

	var world := _make_world()
	add_child_autofree(world)
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
	var world := _make_world()
	add_child_autofree(world)
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
	var world := _make_world()
	add_child_autofree(world)
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
	var world := _make_world()
	add_child_autofree(world)
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
	host.free()


func test_failed_join_load_does_not_make_the_next_mission_wire_only() -> void:
	var blocker := UdpPump.new()
	assert_eq(blocker.bind_listen(0), OK)
	var world := _make_world()
	add_child_autofree(world)
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
		assert_eq(String(spans[0].get("name", "")), "environment")
		assert_gt(int(spans[0].get("end_us", 0)), 0,
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

	var world := _make_world()
	add_child_autofree(world)
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
		assert_eq(String(spans[0].get("name", "")), "environment")
		assert_eq(String(spans[1].get("name", "")), "terrain")
		assert_gt(int(spans[1].get("end_us", 0)), 0,
			"finish closes the terrain span left open by the early return")


func test_successful_mission_load_exposes_the_loaded_file_until_unload() -> void:
	var world := _make_world()
	add_child_autofree(world)
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
	var root_dir := _stage_minimal_fixture("archive_only_bms")
	var archived_bms := FileAccess.get_file_as_bytes(root_dir.path_join("mnml.bms"))
	_write_pff(root_dir.path_join("resource.pff"), [{
		"name": "mnml.bms",
		"bytes": archived_bms,
	}])
	_write_bytes(root_dir.path_join("mnml.bms"), "not a mission".to_utf8_buffer())

	var resource_root := ResourceRoot.new()
	assert_eq(resource_root.mount_runtime(root_dir, "", true), OK,
		"the runtime fixture mounts with /d loose overrides enabled")
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	world.set_resource_root(resource_root)

	assert_eq(world.load_mission("mnml.bms"), OK,
		"the witnessed BMS caller bypasses the corrupt loose override")
	assert_eq(world.get_loaded_mission_file(), "mnml.bms")
	world.unload()


func test_editor_run_loads_the_exact_saved_loose_bms() -> void:
	var root_dir := _stage_minimal_fixture("exact_loose_bms")
	_write_pff(root_dir.path_join("resource.pff"), [{
		"name": "mnml.bms",
		"bytes": "not a mission".to_utf8_buffer(),
	}])

	var resource_root := ResourceRoot.new()
	assert_eq(resource_root.mount_runtime(root_dir, "", true), OK)
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	world.set_resource_root(resource_root)

	assert_eq(world.load_loose_mission("mnml.bms"), OK,
		"F6 opens the valid disk BMS even when the archive row is corrupt")
	assert_eq(world.get_loaded_mission_file(), "mnml.bms")
	world.unload()


func test_editor_run_rejects_non_top_level_or_non_bms_paths() -> void:
	var world := _make_world()
	add_child_autofree(world)
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
	var world := _make_world()
	add_child_autofree(world)
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
	var world := _make_world()
	add_child_autofree(world)
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
	var world := _make_world()
	add_child_autofree(world)
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


func test_valid_emplaced_def_without_gfx1_builds_no_fallback_gun() -> void:
	# AVENGER has a valid retail weapon definition but no fpModel. That means an
	# intentionally empty FP pass, not the bring-up AK fallback used when no
	# definition resolves at all.
	var world: ViewmodelWorldHarness = autofree(ViewmodelWorldHarness.new())
	var placer := ViewmodelPlacerStub.new()
	world.install_viewmodel_fixture(_viewmodel_def("WPN_AVENGER", "", 0x80), placer)
	var viewmodel := world.build_local_player_viewmodel()
	assert_not_null(viewmodel,
			"a valid no-model definition is a stable empty FP presentation epoch")
	assert_true(placer.graphics.is_empty(),
			"a missing authored gfx1 must not substitute the AK first-person gun")
	assert_eq(world.model_availability, [false],
			"the render gate observes that no first-person gun model resolved")


func test_first_person_uses_selected_arms_and_raw_part_local_camo() -> void:
	var world := ViewmodelWorldHarness.new()
	var terrain := Terrain.new()
	terrain.name = "Terrain"
	world.add_child(terrain)
	add_child_autofree(world)
	var placer := SelectedAvatarViewmodelPlacerStub.new()
	world.install_viewmodel_fixture(_viewmodel_def("WPN_TEST", "TestGun", 0), placer)
	var container := world.build_local_player_viewmodel()
	assert_not_null(container)
	assert_eq(placer.graphics, ["SelectedArms", "TestGun"],
			"the first-person arms are the selected character's combo arms")
	var parts := world.local_player_viewmodel_parts()
	assert_eq(parts.size(), 2)
	var arms: ObjectModel = parts[0]
	var gun: ObjectModel = parts[1]
	# The arms carry their authored raw triplet for the rig's per-submit FP
	# writer (Avatar_SetArmsCamoCtrl runs before each arms submit, never at
	# load); the gun part is never a camo target.
	assert_eq(arms.avatar_part, ObjectModel.AVATAR_PART_ARMS)
	assert_eq(arms.avatar_camo, Vector3i(17, 34, 51),
			"first-person arms carry the authored raw CTRL bytes")
	assert_eq(gun.avatar_part, ObjectModel.AVATAR_PART_NONE,
			"the arms' per-draw TEX_CAMO state does not leak into the gun")
	assert_true(arms.get_ctrl_values().is_empty(),
			"no FP CTRL writer runs at build time")
	var witness: FirstPersonArmsWitness = \
			world.local_player_first_person_arms_witness()
	assert_true(witness.is_valid())
	assert_eq(witness.character_id, 0x1234)
	assert_eq(witness.arms_graphic, "SelectedArms")
	assert_eq(Array(witness.arms_camo), [17, 34, 51])
	arms.visible = false
	assert_false(world.local_player_first_person_arms_witness().is_valid(),
			"a hidden arms submit cannot witness a visible comparison frame")
	arms.visible = true
	container.visible = false
	assert_false(world.local_player_first_person_arms_witness().is_valid(),
			"arms hidden by their viewmodel ancestor cannot pass the witness")


func test_first_person_without_character_arms_submits_the_gun_alone() -> void:
	var world: ViewmodelWorldHarness = autofree(ViewmodelWorldHarness.new())
	var placer := NoCharacterViewmodelPlacerStub.new()
	world.install_viewmodel_fixture(_viewmodel_def("WPN_TEST", "TestGun", 0), placer)
	var container := world.build_local_player_viewmodel()
	assert_not_null(container)
	assert_eq(placer.graphics, ["TestGun"],
			"no resolved character arms means no arms submit -- gfx1a is not a fallback "
			+ "[orig: Player_RenderFirstPersonViewModel @0x4df064/@0x4df06b]")
	assert_eq(world.local_player_viewmodel_parts().size(), 1)


func test_joiner_challenge_prewarm_loads_player_and_current_viewmodels_before_freeze() -> void:
	var world: ChallengePrewarmWorldHarness = autofree(
			ChallengePrewarmWorldHarness.new())
	var placer := ChallengePrewarmPlacerStub.new()
	world.install_prewarm_fixture(_viewmodel_def("WPN_TEST", "test_gun", 0), placer)

	world.prewarm_challenge_models()

	assert_eq(placer.loaded, ["player_body", "test_gun", "test_arms"],
		"the frozen 0x3D source includes every .3DI the first player frame would load "
		+ "(the character's arms, not a weapon.def field)")


func test_joiner_accepts_novaworld_advertised_mission_basename() -> void:
	var world := _make_world()
	add_child_autofree(world)
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
	host.free()
	# The admission watchdog used to be a coroutine owned by NetSessionDrive.
	# Freeing its sole GameWorld owner while it awaited process_frame made Godot
	# resume a method whose class instance was already gone on the next frame.
	world.free()
	world = null
	await get_tree().process_frame
	assert_engine_error_count(0,
			"tearing down a successfully wire-loaded world leaves no suspended owner method")


func test_skeleton_debug_builds_and_frees_the_view() -> void:
	# The F3 overlay's "Show skeletons" toggle routes here: enabling builds a child
	# SkeletonDebugView under the world, disabling frees it (mirrors set_pick_debug).
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	assert_false(world.debug_views().is_skeleton_debug(), "off by default")
	assert_null(world.get_node_or_null("SkeletonDebug"), "...with no view node")

	world.debug_views().set_skeleton_debug(true)
	assert_true(world.debug_views().is_skeleton_debug())
	assert_not_null(world.get_node_or_null("SkeletonDebug"), "enabling builds the 3D view")

	world.debug_views().set_skeleton_debug(false)
	assert_false(world.debug_views().is_skeleton_debug())
	await get_tree().process_frame  # queue_free lands at frame end
	assert_null(world.get_node_or_null("SkeletonDebug"), "disabling frees it")


func test_user_point_debug_builds_frees_and_cleans_up_on_unload() -> void:
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	assert_false(world.debug_views().is_user_point_debug(), "off by default")
	assert_null(world.get_node_or_null("UserPointDebug"), "...with no view node")

	world.debug_views().set_user_point_debug(true)
	assert_true(world.debug_views().is_user_point_debug())
	assert_not_null(world.get_node_or_null("UserPointDebug"),
			"enabling builds the world-wide user-point view")

	world.unload()
	assert_true(world.debug_views().is_user_point_debug(),
			"the checked toggle survives teardown so the next load can re-arm it")
	assert_null(world.get_node_or_null("UserPointDebug"),
			"teardown detaches the view immediately, before deferred destruction")

	world.debug_views().set_user_point_debug(false)
	assert_false(world.debug_views().is_user_point_debug())


func test_retained_debug_views_rearm_after_unload_and_reload() -> void:
	var root_dir := _stage_minimal_fixture("debug_view_reload")
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK)
	world.set_resource_root(root)
	assert_eq(world.load_mission("mnml.bms"), OK)

	world.debug_views().set_skeleton_debug(true)
	world.debug_views().set_user_point_debug(true)
	world.debug_views().set_collision_debug(true)
	world.debug_views().set_occlusion_debug(true)
	world.debug_views().set_round_debug(true)
	world.debug_views().set_hitbox_debug(true)
	var debug_names := [
		"SkeletonDebug",
		"UserPointDebug",
		"CollisionDebug",
		"OcclusionDebug",
		"RoundDebug",
		"HitboxDebug",
	]
	for debug_name in debug_names:
		assert_not_null(world.get_node_or_null(NodePath(debug_name)),
				"%s exists before reload" % debug_name)

	world.unload()
	assert_true(world.debug_views().is_skeleton_debug())
	assert_true(world.debug_views().is_user_point_debug())
	assert_true(world.debug_views().is_collision_debug())
	assert_true(world.debug_views().is_occlusion_debug())
	assert_true(world.debug_views().is_round_debug())
	assert_true(world.debug_views().is_hitbox_debug())
	for debug_name in debug_names:
		assert_null(world.get_node_or_null(NodePath(debug_name)),
				"%s detaches immediately during unload" % debug_name)

	assert_eq(world.load_mission("mnml.bms"), OK)
	for debug_name in debug_names:
		assert_not_null(world.get_node_or_null(NodePath(debug_name)),
				"%s is rebuilt for the replacement mission" % debug_name)
	world.unload()


func test_pick_helpers_keep_stable_names_on_same_frame_replacement() -> void:
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	var first_picks := DebugPickList.new()
	var replacement_picks := DebugPickList.new()

	world.debug_views().set_pick_debug(first_picks)
	var first_view := world.get_node_or_null("PickDebug")
	assert_not_null(first_view)
	world.debug_views().set_pick_debug(replacement_picks)
	var replacement_view := world.get_node_or_null("PickDebug")
	assert_not_null(replacement_view)
	assert_ne(replacement_view, first_view)
	assert_null(first_view.get_parent(),
			"the old view detaches before its deferred destruction")
	assert_eq(replacement_view.name, &"PickDebug")

	world.debug_views().set_pick_click_enabled(true)
	var first_catcher := world.get_node_or_null("PickClickCatcher")
	assert_not_null(first_catcher)
	world.debug_views().set_pick_click_enabled(true)
	var replacement_catcher := world.get_node_or_null("PickClickCatcher")
	assert_not_null(replacement_catcher)
	assert_ne(replacement_catcher, first_catcher)
	assert_null(first_catcher.get_parent(),
			"the old click catcher also detaches before replacement")
	assert_eq(replacement_catcher.name, &"PickClickCatcher")


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
	var world := _make_world()
	add_child_autofree(world)
	_load_minimal_mission(world)
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
	var world := _make_world()
	add_child_autofree(world)
	_load_minimal_mission(world)
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
	var world := WarmGameWorldHarness.new()
	autofree(world)
	var effects := WarmEffectWorldStub.new()
	autofree(effects)
	world.configure_warm_effects(effects)
	world.set_particles_hidden(true)

	assert_eq(world.warm_effect_catalog(), 1)

	assert_false(effects.hidden_during_warm,
			"the persistent gameplay preference cannot suppress load warming")
	assert_eq(effects.calls, ["warm", "advance", "render", "reset"],
			"the warm snapshot is submitted before its runtime values reset")
	assert_eq(effects.visibility_changes, [true, false, true],
			"warming lifts the switch only for the covered load pass")
	assert_true(effects.are_particles_hidden(),
			"the user's particle preference is restored before returning")
	assert_true(world.attached_while_hidden,
			"persistent item effects reattach under the restored preference")


func test_item_effect_attach_uses_the_original_pool_specific_gates() -> void:
	# Mission kinds preserve the original pool mapping. Pool 0 is not walked;
	# pool 1 skips attrib 0x42; pools 2/3 skip only powerup bit 0x2.
	# [orig: resolve_item_materials_and_spawn_bone_trails @ 0x522ee0,
	#  gates @ 0x523233 / @ 0x523272 / @ 0x5232af]
	var world := _make_item_fx_world()
	add_child_autofree(world)
	var effects := FxWorldStub.new()
	world.add_child(effects)
	var db := _fx_item_db([
		{"id": 1, "effect": "Effect_Test", "userpoint": "MFlash01"},
		{"id": 2, "effect": "Effect_Test", "userpoint": "MFlash01"},
		{"id": 3, "attribs": "PlayerControl",
				"effect": "Effect_Test", "userpoint": "MFlash01"},
		{"id": 4, "attribs": "Powerup",
				"effect": "Effect_Test", "userpoint": "MFlash01"},
		{"id": 5, "attribs": "PlayerControl",
				"effect": "Effect_Test", "userpoint": "MFlash01"},
		{"id": 6, "effect": "Effect_Test", "userpoint": "MFlash01"},
		{"id": 7, "effect": "Effect_Test", "userpoint": "MFlash01"},
		{"id": 8, "effect": "Effect_Test", "userpoint": "MFlash01"},
	])
	var placer := ItemFxPlacerStub.new()
	placer.item_db = db
	world.configure_item_fx(effects, placer)

	var cases := [
		[MissionData.KIND_ORGANIC, 1, 0],
		[MissionData.KIND_ITEM, 2, 1],
		[MissionData.KIND_ITEM, 3, 0],
		[MissionData.KIND_BUILDING, 4, 0],
		[MissionData.KIND_BUILDING, 5, 1],
		[MissionData.KIND_MARKER, 6, 1],
	]
	var nodes: Array[Node3D] = []
	for case_v in cases:
		var case: Array = case_v
		var model := _fx_model(world)
		nodes.append(model)
		assert_eq(world.present_item_fx(model, int(case[0]), int(case[1])),
				int(case[2]), "kind %d attrib 0x%x" % [int(case[0]), db.get_attrib(int(case[1]))])

	assert_eq(effects.attached_spawns.size(), 3,
			"only normal pool-1 plus allowed pool-2/3 entities attach")
	var anchor_info := _fx_anchor_info()
	assert_eq(effects.attached_spawns[0].local_pos,
			anchor_info.position)
	assert_eq(effects.attached_spawns[0].local_dir,
			anchor_info.rotation)
	assert_eq(world.present_item_fx(nodes[1], MissionData.KIND_ITEM, 2), 0,
			"a replayed wire-node callback cannot duplicate an existing attach")
	assert_eq(effects.attached_spawns.size(), 3)

	# A persistent item first materialized while the retail master switch is off
	# must attach once when particles are re-enabled; a mission loaded hidden
	# otherwise loses that effect permanently.
	world.set_particles_hidden(true)
	var hidden_model := _fx_model(world)
	assert_eq(world.present_item_fx(
			hidden_model, MissionData.KIND_ITEM, 7), 0)
	assert_eq(effects.attached_spawns.size(), 3)
	world.set_particles_hidden(false)
	assert_eq(effects.attached_spawns.size(), 4,
			"re-enable retries the hidden-at-load persistent attachment once")
	world.set_particles_hidden(false)
	assert_eq(effects.attached_spawns.size(), 4, "steady enabled state cannot duplicate it")

	world.set_particles_hidden(true)
	var despawned_model := _fx_model(world)
	assert_eq(world.present_item_fx(
			despawned_model, MissionData.KIND_ITEM, 8), 0)
	despawned_model.free()
	world.set_particles_hidden(false)
	assert_eq(effects.attached_spawns.size(), 4,
			"a wire node freed while hidden is pruned instead of retried")
	assert_eq(world.pending_item_fx_count(), 0)


func test_dbuggy_fx00_follows_controller_lifecycle_with_pre_node_race() -> void:
	# Shipped DBuggy1.3di: ITEMS.DEF item 101291 is PlayerControl (0x40) and
	# authors Effect_whiteExhaust at model userpoint FX00. It stays dormant in
	# the mission-start pool walk, then follows controller occupancy events.
	const DBUGGY_ITEM := 101291
	var world := _make_item_fx_world()
	add_child_autofree(world)
	var effects := FxWorldStub.new()
	world.add_child(effects)
	var db := _fx_item_db([{"id": DBUGGY_ITEM, "attribs": "PlayerControl",
			"effect": "Effect_whiteExhaust", "userpoint": "MFlash01"}])
	var placer := ItemFxPlacerStub.new()
	placer.item_db = db
	world.configure_item_fx(effects, placer)
	watch_signals(world)

	var spawn_origin := (MissionData.KIND_ITEM << 24) | 3
	var started := {
		"kind": "vehicle_control_started",
		"a": 71,
		"b": 9001,
		"c": spawn_origin,
	}
	var stopped := {
		"kind": "vehicle_control_stopped",
		"a": 71,
		"b": 9001,
		"c": spawn_origin,
	}

	# The simulation event can precede the present pass's wire/model callback.
	world.consume_runtime_effects([started])
	assert_eq(world.active_control_identity_count(), 3)
	assert_signal_not_emitted(world, "mission_effects",
			"render-internal lifecycle events never leak to HUD consumers")
	var model := _fx_model(world)
	model.entity_ref = EntityRef.make(MissionData.KIND_ITEM, 3, 9001, DBUGGY_ITEM)
	assert_eq(world.present_item_fx(model, MissionData.KIND_ITEM, DBUGGY_ITEM), 1)
	assert_eq(world.deferred_control_item_fx_count(), 1)
	assert_eq(effects.attached_spawns.size(), 1)
	assert_eq(String(effects.attached_spawns[0].effect), "Effect_whiteExhaust")
	var anchor_info := _fx_anchor_info()
	assert_eq(Vector3(effects.attached_spawns[0].local_pos),
			anchor_info.position,
			"the effect anchors at the real model's MFlash01 point")
	assert_eq(Vector3(effects.attached_spawns[0].local_dir),
			anchor_info.rotation)

	# Replayed starts are idempotent; a single transition stop detaches the
	# exact native group id returned by the receipt-bearing facade.
	world.consume_runtime_effects([started])
	assert_eq(effects.attached_spawns.size(), 1)
	world.consume_runtime_effects([stopped])
	assert_eq(effects.stopped_groups, [1])
	assert_eq(world.active_control_identity_count(), 0)

	# A later control transition can create a fresh group, and a mixed drain
	# exposes only the public effect after consuming the lifecycle row.
	world.consume_runtime_effects([started])
	assert_eq(effects.attached_spawns.size(), 2)
	var public_effect := {"kind": "text", "str": "still public"}
	world.consume_runtime_effects([stopped, public_effect])
	assert_eq(effects.stopped_groups, [1, 2])
	assert_signal_emitted_with_parameters(
			world, "mission_effects", [[public_effect]])


func test_dbuggy_hidden_pending_is_cancelled_when_control_stops() -> void:
	const DBUGGY_ITEM := 101291
	var world := _make_item_fx_world()
	add_child_autofree(world)
	var effects := FxWorldStub.new()
	world.add_child(effects)
	var db := _fx_item_db([{"id": DBUGGY_ITEM, "attribs": "PlayerControl",
			"effect": "Effect_whiteExhaust", "userpoint": "MFlash01"}])
	var placer := ItemFxPlacerStub.new()
	placer.item_db = db
	world.configure_item_fx(effects, placer)
	world.set_particles_hidden(true)

	var model := _fx_model(world)
	model.entity_ref = EntityRef.make(MissionData.KIND_ITEM, 8, 9010, DBUGGY_ITEM)
	assert_eq(world.present_item_fx(model, MissionData.KIND_ITEM, DBUGGY_ITEM), 0,
			"the unchanged mission-start 0x42 gate keeps PlayerControl dormant")
	var spawn_origin := (MissionData.KIND_ITEM << 24) | 8
	world.consume_runtime_effects([{
		"kind": "vehicle_control_started",
		"a": 81,
		"b": 9010,
		"c": spawn_origin,
	}])
	assert_eq(world.pending_item_fx_count(), 1)
	world.consume_runtime_effects([{
		"kind": "vehicle_control_stopped",
		"a": 81,
		"b": 9010,
		"c": spawn_origin,
	}])
	assert_eq(world.pending_item_fx_count(), 0,
			"stop cancels a hidden activation before particles are re-enabled")
	world.set_particles_hidden(false)
	assert_eq(effects.attached_spawns.size(), 0,
			"re-enable cannot resurrect a stopped controller attachment")


func test_controller_net_id_does_not_alias_a_wire_handle() -> void:
	const DBUGGY_ITEM := 101291
	var world := _make_item_fx_world()
	add_child_autofree(world)
	var effects := FxWorldStub.new()
	world.add_child(effects)
	var db := _fx_item_db([{"id": DBUGGY_ITEM, "attribs": "PlayerControl",
			"effect": "Effect_whiteExhaust", "userpoint": "MFlash01"}])
	var placer := ItemFxPlacerStub.new()
	placer.item_db = db
	world.configure_item_fx(effects, placer)

	world.consume_runtime_effects([{
		"kind": "vehicle_control_started",
		"a": 77,
		"b": 0,
		"c": 0,
	}])
	var model := _fx_model(world)
	model.entity_ref = EntityRef.make(MissionData.KIND_ITEM, 12, 0, DBUGGY_ITEM, 77)
	assert_eq(world.present_item_fx(model, MissionData.KIND_ITEM, DBUGGY_ITEM), 0)
	assert_eq(effects.attached_spawns.size(), 0,
			"event a is a simulation net id, not the presentation wire handle")


func test_synthetic_controller_effects_are_scoped_to_their_wire_sibling() -> void:
	const PLAYER_CONTROL_ITEM := 101291
	var world := _make_item_fx_world()
	add_child_autofree(world)
	var effects := FxWorldStub.new()
	world.add_child(effects)
	var db := _fx_item_db([{"id": PLAYER_CONTROL_ITEM, "attribs": "PlayerControl",
			"effect": "Effect_whiteExhaust", "userpoint": "MFlash01"}])
	var placer := ItemFxPlacerStub.new()
	placer.item_db = db
	world.configure_item_fx(effects, placer)

	var siblings: Array[ObjectModel] = []
	for wire_handle in [0x1004, 0x1005]:
		var model := _fx_model(world)
		var ref := EntityRef.make(MissionData.KIND_ITEM, 0xffffff, 0,
				PLAYER_CONTROL_ITEM, wire_handle)
		ref.origin_kind = 0xff
		model.entity_ref = ref
		siblings.append(model)
		assert_eq(world.present_item_fx(
				model, MissionData.KIND_ITEM, PLAYER_CONTROL_ITEM), 0)

	world.consume_runtime_effects([{
		"kind": "vehicle_control_started",
		"a": 0,
		"b": 0,
		"c": -1,
		"wire_handle": 0x1004,
	}])
	assert_eq(effects.attached_spawns.size(), 1,
			"mounting one synthetic emplacement starts only that sibling's effect")
	assert_eq(String(effects.attached_spawns[0].owner),
			"itemfx:%d:%d" % [siblings[0].get_instance_id(), _fx_anchor_index()])

	world.consume_runtime_effects([{
		"kind": "vehicle_control_stopped",
		"a": 0,
		"b": 0,
		"c": -1,
		"wire_handle": 0x1004,
	}, {
		"kind": "vehicle_control_started",
		"a": 0,
		"b": 0,
		"c": -1,
		"wire_handle": 0x1005,
	}])
	assert_eq(effects.stopped_groups, [1])
	assert_eq(effects.attached_spawns.size(), 2)
	assert_eq(String(effects.attached_spawns[1].owner),
			"itemfx:%d:%d" % [siblings[1].get_instance_id(), _fx_anchor_index()],
			"the shared synthetic origin cannot activate a neighboring attachment")


func test_static_item_effects_spawn_world_bound_from_value_descriptors() -> void:
	var world := _make_item_fx_world()
	add_child_autofree(world)
	var effects := FxWorldStub.new()
	world.add_child(effects)
	var db := _fx_item_db([
		{"id": 2, "effect": "Effect_Test", "userpoint": "MFlash01"},
		{"id": 3, "attribs": "PlayerControl",
				"effect": "Effect_Test", "userpoint": "MFlash01"},
		{"id": 9, "effect": "Effect_Fallback", "userpoint": "MFlash01"},
	])
	var placer := ItemFxPlacerStub.new()
	placer.item_db = db

	# Real model data: mount authors MFlash01 (the matched anchor); shed
	# authors no such point (the origin-fallback leg). The first-16 mask RULE
	# (duplicates, beyond-16 exclusion) is native and pinned by the threedi
	# user-point-mask ctest.
	var matched_data := _fx_anchor_data()
	var fallback_data := _fx_plain_data()
	var entity_transform := Transform3D(
			Basis(Vector3.UP, PI * 0.5), Vector3(10, 20, 30))
	var fallback_transform := Transform3D(
			Basis(Vector3.RIGHT, PI * 0.25), Vector3(-5, 6, 7))
	placer.static_sources = [
		ItemFxPlacerStub.static_source(MissionData.KIND_ITEM, 2, "StaticVehicle1",
				entity_transform, matched_data),
		ItemFxPlacerStub.static_source(MissionData.KIND_BUILDING, 9, "StaticBuilding1",
				fallback_transform, fallback_data),
		# Pool-1 attrib 0x40 is excluded before any effect request.
		ItemFxPlacerStub.static_source(MissionData.KIND_ITEM, 3, "BlockedStatic",
				Transform3D.IDENTITY, matched_data),
	]
	world.configure_item_fx(effects, placer)

	world.attach_all_item_fx()

	assert_eq(effects.request_spawns.size(), 2,
			"one matched anchor plus one origin fallback; the gated row is excluded")
	var first: Dictionary = effects.request_spawns[0]
	var first_options: EffectSpawnOptions = first.get("options")
	assert_eq(first_options.admission, EffectScene.ADMISSION_ALWAYS)
	assert_eq(first_options.binding, EffectScene.BINDING_WORLD)
	assert_eq(first_options.render_domain, EffectScene.RENDER_DOMAIN_WORLD)
	assert_null(first_options.owner_key, "static batches never invent follow owners")
	assert_null(first_options.slot_key, "Always spawns need no synthetic slot identity")
	var anchor_info := _fx_anchor_info()
	var first_transform: Transform3D = first.get("transform", Transform3D.IDENTITY)
	assert_true(first_transform.origin.is_equal_approx(
			entity_transform * anchor_info.position))
	var anchor_dir := anchor_info.rotation
	if anchor_dir.length_squared() > 0.000001:
		assert_true(first_transform.basis.z.normalized().is_equal_approx(
				(entity_transform.basis * anchor_dir).normalized()),
				"the authored direction composes through the entity basis")
	var fallback_request: Dictionary = effects.request_spawns[1]
	assert_eq(String(fallback_request.get("effect", "")), "Effect_Fallback")
	var fallback_actual: Transform3D = fallback_request.get(
			"transform", Transform3D.IDENTITY)
	assert_true(fallback_actual.is_equal_approx(fallback_transform),
			"an unmatched userpoint falls back to the entity origin and basis")
	assert_eq(world.present_static_item_fx(placer.static_sources[0], 0), 0,
			"revisiting the same descriptor index cannot duplicate its persistent effect")
	assert_eq(effects.request_spawns.size(), 2)


func test_live_item_effect_owner_uses_each_fixed_ticks_value_pose() -> void:
	# The REAL MissionPresentation pose chain: before the first completed logic tick
	# there is no present snapshot (the authored Node seeds the spawn); once a
	# tick completes, the owner follows the sim's client-view value pose; an
	# identity absent from the snapshot detaches (null). The old runtime double
	# also recorded that the entity_ref dictionary crossed the seam by identity;
	# that probe lived on the double and is gone — the pose values below only
	# resolve if the real seam consumed the same stable identity.
	var root_dir := _stage_building_fixture("item_owner_pose")
	var world := _make_item_fx_world()
	add_child_autofree(world)
	var placed := {}  # mutated (merge), never reassigned: lambda captures copy locals
	_load_minimal_mission(world, root_dir, func(mission: MissionData) -> void:
		placed.merge(mission.add_entity(
				MissionData.KIND_BUILDING, 102001, Vector3(6, 4, 5), Vector3.ZERO)))
	var bms_id := int(placed.get("bms_id", 0))
	assert_gt(bms_id, 0)
	var node := _fx_model(world)
	node.transform = Transform3D(Basis.IDENTITY, Vector3(99, 99, 99))
	var key := "itemfx:%d:0" % bms_id
	world.configure_item_owner(key, node,
			EntityRef.make(MissionData.KIND_BUILDING, 0, bms_id))

	var resolved: Variant = world.resolve_item_owner(key)
	assert_true(resolved is Transform3D)
	assert_true((resolved as Transform3D).origin.is_equal_approx(Vector3(99, 99, 99)),
			"before the first sim snapshot, the authored Node remains the safe spawn seed")

	world.tick(Vector3.ZERO, Transform3D(), ONE_TICK_DELTA)
	resolved = world.resolve_item_owner(key)
	assert_true(resolved is Transform3D)
	if resolved is Transform3D:
		var origin := (resolved as Transform3D).origin
		assert_almost_eq(origin.x, 6.0, 0.01,
				"a fixed tick follows the current client-view value, not the stale presented Node")
		assert_almost_eq(origin.z, -4.0, 0.01,
				"the value pose rides the canonical mission-to-Godot frame")

	var absent_key := "itemfx:999999:0"
	world.configure_item_owner(absent_key, node,
			EntityRef.make(MissionData.KIND_BUILDING, 999, 999999))
	assert_null(world.resolve_item_owner(absent_key),
			"an owner absent from this tick detaches instead of emitting once from stale presentation")


func test_static_item_effect_hidden_at_load_retries_once_when_enabled() -> void:
	var world := _make_item_fx_world()
	add_child_autofree(world)
	var effects := FxWorldStub.new()
	world.add_child(effects)
	var db := _fx_item_db([
		{"id": 2, "effect": "Effect_Test", "userpoint": "MFlash01"}])
	var placer := ItemFxPlacerStub.new()
	placer.item_db = db
	placer.static_sources = [
		ItemFxPlacerStub.static_source(MissionData.KIND_ITEM, 2, "StaticVehicle1",
				Transform3D(Basis.IDENTITY, Vector3(3, 4, 5)), _fx_anchor_data()),
	]
	world.configure_item_fx(effects, placer)

	world.set_particles_hidden(true)
	world.attach_all_item_fx()
	assert_eq(effects.request_spawns.size(), 0)
	assert_eq(world.pending_static_item_fx_count(), 1,
			"a world-bound persistent source survives a hidden mission load as values")
	world.set_particles_hidden(false)
	assert_eq(effects.request_spawns.size(), 1,
			"re-enabling submits the deferred static source")
	assert_eq(world.pending_static_item_fx_count(), 0)
	world.set_particles_hidden(false)
	assert_eq(effects.request_spawns.size(), 1, "steady enabled state cannot duplicate it")


func _make_item_fx_world() -> ItemFxGameWorldHarness:
	var world := ItemFxGameWorldHarness.new()
	var terrain := Terrain.new()
	terrain.name = "Terrain"
	world.add_child(terrain)
	# The environment child makes this harness mission-loadable: the typed
	# placement path stamps _env.light_state onto every placed batch.
	var env := MissionEnvironment.new()
	env.name = "MissionEnvironment"
	world.add_child(env)
	return world


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
	var world := _make_world()
	add_child_autofree(world)
	_load_minimal_mission(world, "", func(mission: MissionData) -> void:
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
	var world := _make_world()
	add_child_autofree(world)
	var placed := {}  # mutated (merge), never reassigned: lambda captures copy locals
	_load_minimal_mission(world, root_dir, func(mission: MissionData) -> void:
		placed.merge(mission.add_entity(
				MissionData.KIND_BUILDING, 102001, Vector3(16, 24, 4), Vector3.ZERO)))
	var bms_id := int(placed.get("bms_id", 0))
	assert_gt(bms_id, 0)
	var building := world.get_runtime().get_registry().resolve_single(bms_id) as Node3D
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
# shared present-visibility dictionary contract in mission_present_pass_test.)


func test_probe_occlusion_skip_restores_frame_state_and_keeps_iris_live() -> void:
	# The A/B probe seam on the REAL stack: entering the occlusion skip
	# releases every live occlusion claim (the behind-camera building restores
	# to its present intent) while the iris exposure feed keeps sampling into
	# the real weather node; leaving the skip re-arms the sim's delta baseline
	# and the next frame re-applies the claim. (The stub-era section-mask and
	# entity-cull legs died with the sim doubles — see the occlusion frame test
	# above for what the fixture world can witness.)
	var root_dir := _stage_building_fixture("occl_probe_skip")
	var world := _make_world()
	add_child_autofree(world)
	var placed := {}  # mutated (merge), never reassigned: lambda captures copy locals
	_load_minimal_mission(world, root_dir, func(mission: MissionData) -> void:
		placed.merge(mission.add_entity(
				MissionData.KIND_BUILDING, 102001, Vector3(16, 24, 4), Vector3.ZERO)))
	var building := world.get_runtime().get_registry().resolve_single(
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


# (test_occlusion_steady_frames_touch_no_nodes was deleted with the sim
# doubles: counting section-mask dispatches needed the recording node stub. The
# churn contract it pinned — steady verdicts emit no deltas, so steady frames
# walk nothing — is the native delta contract, pinned on the real sim by
# simulation_test.gd's test_occlusion_delta_calls_emit_changes_only.)


func test_occlusion_debug_view_builds_and_frees() -> void:
	# The F3 overlay's "Show portal faces" toggle: GameWorld builds/frees the
	# OcclusionDebugView child (the collision-view contract). Without a sim the
	# built view clears instead of erroring.
	var world := _make_world()
	add_child_autofree(world)
	world.debug_views().set_occlusion_debug(true)
	var view := world.get_node_or_null(NodePath("OcclusionDebug"))
	assert_not_null(view, "enabling builds the occlusion debug child")
	assert_true(world.debug_views().is_occlusion_debug())
	view.refresh_now()  # no sim resolved: clears, no error
	world.debug_views().set_occlusion_debug(false)
	assert_false(world.debug_views().is_occlusion_debug())
	assert_true(view.is_queued_for_deletion(), "disabling frees the view")


# (test_tick_emits_session_lost_once_for_an_in_match_loss and
# test_tick_holds_split_grant_join_until_the_deploy_policy_is_complete were
# deleted with the joiner sim doubles: staging an in-match loss edge or a
# split-grant zoned admission on the REAL stack needs either the 120 s silence
# reap [orig: cs_dir0.timeout_ms @ 0x4ca4a0] or a live zoned host — neither
# fits a unit frame. The latch/edge machine those tests exercised — loss wins
# and emits ONCE per session, the deploy pick holds until the policy/grant
# boundary completes, the ready edge is once per join — is the native
# NetSessionPolicy, pinned on the real policy by
# tests/npruntime/join_session_policy_test.cpp (test_session_loss_latch,
# test_admission_deploy_edge_and_ready). GameWorld's forwarding of the real
# sim predicates into that policy is covered by the live joiner tests above
# (test_joiner_accepts_novaworld_advertised_mission_basename,
# test_escape_aborts_the_joiner_admission_wait) and the non-joiner leg below.)


func test_tick_never_emits_session_lost_for_a_non_joiner() -> void:
	# A REAL single-player listen-server mission (is_joiner() false) must never
	# be treated as a lost session by the per-frame observer.
	var world := _make_world()
	add_child_autofree(world)
	_load_minimal_mission(world)
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
	var world := _make_world()
	add_child_autofree(world)
	_load_minimal_mission(world)
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


# The REAL packaged world scene: since the typed sweep the mission path drives
# the scene's engine nodes directly (env light-state stamps, water lifecycle,
# weather prewarm), so focused tests instance game_world.tscn like the shells
# do instead of hand-building partial worlds.
func _make_world() -> GameWorld:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	return packed.instantiate() as GameWorld


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


func _stage_minimal_fixture(name: String) -> String:
	var root_dir := _make_fixture_root(name)
	var source_dir := ProjectSettings.globalize_path("res://../assets")
	for file_name in DirAccess.get_files_at(source_dir):
		assert_eq(DirAccess.copy_absolute(
			source_dir.path_join(file_name), root_dir.path_join(file_name)), OK)
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
	var root_dir := _stage_minimal_fixture(name)
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
	var world := _make_world()
	add_child_autofree(world)
	var viewport := world.get_viewport()
	viewport.use_occlusion_culling = true
	_load_minimal_mission(world, root_dir, func(mission: MissionData) -> void:
		mission.add_entity(
				MissionData.KIND_BUILDING, 102001, Vector3(16, 24, 4), Vector3.ZERO))
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
