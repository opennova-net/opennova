class_name EffectLightDirector
extends RefCounted

## The EffectWorld dynamic point-light director: spawns one pool light per
## authored model light record for every placed entity, and drives the
## per-frame select that feeds the technique shaders' global parameters.
## The witness map lives on engine/runtime/renderer/light_scene.h — mission
## start walks the placed pools spawning per-record instances
## [orig: Game_StartMission @ 0x525d19 -> Game_SpawnAllEntityGlowEffects @0x5227b0 ->
## Entity_SpawnGlowEffects @ 0x56c7c0], and each draw selects the nearest
## group-passing four [orig: collect_nearby_zones_by_aabb @ 0x5aa250;
## update_light_slots @ 0x5abc50]. The object pass runs per rendered model:
## one draw context per visible ObjectModel carrying BOTH witnessed groups —
## its entity as the owner group, and the building it stands inside plus that
## blink volume's section as the interior group — so owned lights (muzzle
## glow, subobject records, interior room lights) light only what retail's
## update_light_slots admits. Corona billboards draw per frame from the
## portable corona walk [orig: EffectWorld_RenderLightCoronas @ 0x5aaf40].
## The remaining D-RLIT-4 residual is foliage sampling. Authored LGHT
## positions/lifetimes are spawn-fixed; powerup respawn is routed, and a husk
## swap neither moves nor rescans lights (retail call graph cited below).

## Model gather half-extent around the camera. Light ranges are authored
## small (atten_end 8 on the fire barrels), so any model a pool light could
## touch sits well inside this radius.
const QUERY_RADIUS := 512.0

# Light owner zero is retail's unowned/world sentinel. A decoded wire handle
# is an independent 16-bit domain in which zero is valid, so tag every wire
# identity into a nonzero, non-ObjectID range before it reaches LightScene.
const WIRE_OWNER_TAG := 1 << 48
const STATIC_OWNER_TAG := 2 << 48

var _world: GameWorld
var _static_sources := Callable()
var _static_draw_sources := Callable()
var _static_draw_source_revision := Callable()
# The packed static atlas rows, rebuilt only when the placer's draw-source
# revision (rows appended, table reset, carve state) or the static source
# snapshot changes. Rows are immutable identities; only the light SELECTION
# over them runs per frame, as retail's per-batch select does.
var _static_rows_revision := -1
var _static_rows_bounds := PackedVector3Array()
var _static_rows_owner_entities := PackedInt64Array()
var _static_rows_owner_sections := PackedInt32Array()
var _static_rows_interior_owners := PackedInt64Array()
var _static_rows_interior_sections := PackedInt32Array()
var _static_rows_active := PackedByteArray()
var _scene: LightScene = LightScene.new()
var _spawned_static: Dictionary = {}
var _static_sources_snapshot: Array = []
var _static_owner_by_bms: Dictionary = {}
var _spawned_nodes: Dictionary = {}
# Entity owner id -> its ONE cached EffectWorld handle. Retail does not own a
# model-light handle array: every LGHT spawn overwrites entity+0x1B4, the
# MF_Light muzzle path reuses that same word, and Entity_Destroy clears only
# its final value [orig: Entity_SpawnGlowEffects @0x56c925..0x56c92c;
# Entity_UpdateMuzzleGlowEffect @0x56c965..0x56c9d5;
# Entity_Destroy @0x43e903..0x43e916]. Earlier model lights intentionally
# remain in the pool until mission teardown, matching retail's lifecycle.
var _entity_effect_handles: Dictionary = {}
# round presentation id -> pool light handle (the light_move follow).
var _round_handles: Dictionary = {}
# The corona billboard presenter: one MultiMesh of additive camera-facing
# quads rebuilt per frame from the portable corona walk
# [orig: EffectWorld_RenderLightCoronas @ 0x5aaf40 — the witness map lives on
# renderer::LightScene::collect_corona_quads].
var _corona_instance: MultiMeshInstance3D
var _corona_frame := 0
# Containing-building bms_id -> owner id. Owner identity is fixed for a
# node's tree lifetime (entity_ref meta is stamped once at creation), so the
# cache survives across frames and clears on container membership change,
# reset, and reattach.
var _blink_owner_cache: Dictionary = {}
# The report-only census skipped this frame (F3 capture off); run_census_now
# refreshes it on demand with the last frame's camera.
var _census_stale := false
var _census_cam_pos := Vector3.ZERO
# Per-frame walk registry: MissionObjects children that are ObjectModels with
# their meta-derived identity read once at (re)build. Membership changes mark
# it dirty (child_entered_tree/child_exiting_tree on the container); a husk
# swap neither exits the node nor rewrites entity_ref, so rows stay valid.
var _reg_models: Array[ObjectModel] = []
var _reg_owners := PackedInt64Array()
var _reg_robj_scoped := PackedByteArray()
var _reg_bms_ids := PackedInt64Array()
var _reg_dirty := true
var _reg_container_id := 0
# Reused per-frame draw-context arrays (the native call reads them whole, so
# they are cleared, not tail-truncated).
var _frame_models: Array[Node3D] = []
var _frame_owners := PackedInt64Array()
var _frame_interior_owners := PackedInt64Array()
var _frame_interior_sections := PackedInt32Array()
var _frame_robj_scoped := PackedByteArray()
# Interior-group rows latched per logic tick (they are tick products): the
# flat sim rows plus a bms_id -> base-index lookup, no per-row allocations.
var _interior_rows := PackedInt64Array()
var _interior_index: Dictionary = {}
var _interior_tick := -1


func setup(world: GameWorld, static_sources: Callable,
		static_draw_sources: Callable,
		static_draw_source_revision := Callable()) -> void:
	_world = world
	_static_sources = static_sources
	_static_draw_sources = static_draw_sources
	_static_draw_source_revision = static_draw_source_revision
	_static_rows_revision = -1


## Mission teardown: disconnect live node retirement hooks, retire every pool
## lease, and synchronously clear the shader-global output.
func reset() -> void:
	for node_id_v in _spawned_nodes.keys():
		_disconnect_wire_node_exit(_spawned_nodes[node_id_v])
	_scene.clear()
	_spawned_static.clear()
	_static_sources_snapshot.clear()
	_static_rows_revision = -1
	_static_owner_by_bms.clear()
	_spawned_nodes.clear()
	_entity_effect_handles.clear()
	_round_handles.clear()
	_blink_owner_cache.clear()
	_reg_models.clear()
	_reg_owners.clear()
	_reg_robj_scoped.clear()
	_reg_bms_ids.clear()
	_reg_dirty = true
	_interior_rows = PackedInt64Array()
	_interior_index.clear()
	_interior_tick = -1
	_clear_coronas()


## Mission start / sim-restart: reset, then respawn from the restored entity
## set — the same lifecycle the item-effect director uses.
func reattach() -> void:
	reset()
	var container: Node = _world.get_node_or_null(NodePath("MissionObjects"))
	if container != null:
		for child in container.get_children():
			var node := child as ObjectModel
			if node == null or node.entity_ref == null:
				continue
			on_wire_node_spawned(node, -1, 0)
	_static_sources_snapshot = _static_sources.call() \
			if _static_sources.is_valid() else []
	_static_rows_revision = -1
	# Build every BMS identity before resolving any blink containment. A static
	# item can spawn inside a batched building that appears later in the source
	# walk, and retail still binds it to that building's owner group.
	for source_index in range(_static_sources_snapshot.size()):
		var mapped_source: StaticEffectSource = _static_sources_snapshot[source_index]
		var bms_id := mapped_source.bms_id
		if bms_id != 0:
			_static_owner_by_bms[bms_id] = owner_id_for_static_source(source_index)
	for source_index in range(_static_sources_snapshot.size()):
		var source: StaticEffectSource = _static_sources_snapshot[source_index]
		if _spawned_static.has(source_index):
			continue
		var data: ObjectData = source.object_data
		if data == null:
			continue
		# Batched statics are entities too: a subobject record binds to this
		# tagged owner and the atlas draw row declares the same identity.
		# [orig: Entity_SpawnGlowEffects @ 0x56c8ae; SetOwnerGroup(entity,bone)]
		var xform: Transform3D = source.world_transform
		var is_building := source.kind == MissionData.KIND_BUILDING
		# Retail skips the blink query for a building's own records; every other
		# static resolves containment once at its placement origin.
		var blink_owner: Array = [] if is_building else _blink_owner_at(xform.origin)
		var handles := _spawn_model_lights(data, xform,
				owner_id_for_static_source(source_index), blink_owner, is_building)
		if not handles.is_empty():
			_spawned_static[source_index] = handles


## Wire/late spawns route here through the world's spawn router (shared with
## the item-effect director).
## The one owner id space: a model's sim wire handle when the present pass
## stamped one (live entities — the same domain fire events report shooters
## in), else the node instance id (placer statics, preview scenes). Light
## spawns and per-draw contexts must agree on this or owner gating never
## matches.
static func owner_id_for_node(node: ObjectModel) -> int:
	var ref: EntityRef = node.entity_ref
	var wire := ref.wire_handle if ref != null else -1
	return owner_id_for_wire(wire) if wire >= 0 else node.get_instance_id()


static func owner_id_for_wire(wire_handle: int) -> int:
	return WIRE_OWNER_TAG | (wire_handle & 0xffff) if wire_handle >= 0 else 0


static func owner_id_for_static_source(source_index: int) -> int:
	return STATIC_OWNER_TAG | source_index if source_index >= 0 else 0


func on_wire_node_spawned(node: ObjectModel, _kind: int, _item_id: int) -> void:
	if node == null:
		return
	var node_id := node.get_instance_id()
	if _spawned_nodes.has(node_id):
		return
	var data: ObjectData = node.get_object_data()
	if data == null:
		return
	# Retail runs the blink query once per spawning entity, and skips it
	# outright for a BUILDING — a building's own unattached records stay world
	# lights even though its blink volumes contain them (the query has no
	# self-exclusion) [orig: the ItemType_Building gate @ 0x56c7ec].
	var ref: EntityRef = node.entity_ref
	var is_building := ref != null and ref.kind == MissionData.KIND_BUILDING
	var blink_owner: Array = []
	if not is_building:
		blink_owner = _blink_owner_at(node.global_position)
	if data.get_light_count() <= 0:
		return
	var owner_id := owner_id_for_node(node)
	_entity_effect_handles[owner_id] = _spawn_node_lights(
			node, owner_id, blink_owner, is_building)
	# Register even when pool exhaustion returned zero. Retail walks a given
	# entity once; duplicate callback delivery must not turn a later free slot
	# into an invented second spawn attempt.
	var on_exit := _on_wire_node_exiting.bind(node_id)
	_spawned_nodes[node_id] = {
		"node": weakref(node),
		"owner_id": owner_id,
		"tree_exiting": on_exit,
	}
	node.tree_exiting.connect(on_exit, Object.CONNECT_ONE_SHOT)


func _on_wire_node_exiting(node_id: int) -> void:
	var record: Dictionary = _spawned_nodes.get(node_id, {})
	if record.is_empty():
		return
	_spawned_nodes.erase(node_id)
	# Entity_Destroy has one 16-bit EffectWorld word, not an owned-light list.
	# Clear exactly the lease currently cached there; a husk swap does not exit
	# the node and therefore does not touch any authored light.
	var owner_id := int(record.get("owner_id", 0))
	var cached_handle := int(_entity_effect_handles.get(owner_id, 0))
	if cached_handle != 0:
		_scene.despawn(cached_handle)
	_entity_effect_handles.erase(owner_id)


func _disconnect_wire_node_exit(record: Dictionary) -> void:
	var node_ref := record.get("node") as WeakRef
	var on_exit: Callable = record.get("tree_exiting", Callable())
	var node: Node = null
	if node_ref != null:
		node = node_ref.get_ref() as Node
	if node != null and on_exit.is_valid() and node.tree_exiting.is_connected(on_exit):
		node.tree_exiting.disconnect(on_exit)


func _spawn_model_lights(data: ObjectData, world_transform: Transform3D,
		owner_id: int = 0, blink_owner: Array = [],
		spawner_is_building: bool = false) -> Array[int]:
	var handles: Array[int] = []
	if data == null:
		return handles
	for i in range(data.get_light_count()):
		var handle := spawn_light_record(data.get_light_info(i), world_transform,
				owner_id, blink_owner, spawner_is_building)
		if handle != 0:
			handles.append(handle)
	return handles


## Live-node twin of the static source walk. Retail transforms every record's
## model-space point by the ENTITY placement matrix once; `subobject` is read
## only afterward as an owner-group section [orig: Entity_SpawnGlowEffects
## @0x56c82d..0x56c84e, then @0x56c89a..0x56c8ae]. No node/ROBJ/bone position
## follow exists. Return the final entity+0x1B4 value; retail retains no list.
func _spawn_node_lights(node: ObjectModel, owner_id: int,
		blink_owner: Array, spawner_is_building: bool) -> int:
	if node == null:
		return 0
	var data: ObjectData = node.get_object_data()
	if data == null:
		return 0
	var cached_handle := 0
	for index in range(data.get_light_count()):
		var info := data.get_light_info(index)
		var handle := spawn_light_record(info, node.global_transform, owner_id,
				blink_owner, spawner_is_building)
		cached_handle = handle
	return cached_handle


## One authored light record (ObjectData.get_light_info's ModelLight) becomes one
## pool instance. Public: the GUT seam test feeds records directly. The owner
## attach is decided by the portable policy the config feeds
## (renderer::resolve_model_light_owner): a record attached to a subobject is
## owned by its own entity + that subobject (cabin self-lights), an unattached
## record spawned INSIDE a blink box is owned by the containing building + that
## volume's section (interior room lights), and everything else — the fire
## barrels — spawns unowned and lights the world [orig: Entity_SpawnGlowEffects
## @ 0x56c89f / @ 0x56c8bd]. Every caller supplies an owner id — live nodes
## their wire handle/instance id, batched static sources a synthetic negative
## id — so an owned light passes the per-draw select only for the draws retail
## admits (per_model_light_isolation_test pins both directions). `blink_owner`
## is the [owner id, section] pair _blink_owner_at resolved, empty outdoors.
func spawn_light_record(info: ModelLight, world_transform: Transform3D,
		owner_id: int = 0, blink_owner: Array = [],
		spawner_is_building: bool = false) -> int:
	if info == null:
		return 0
	var world_pos: Vector3 = world_transform * info.position
	return _spawn_light_at(info, world_pos, owner_id, blink_owner,
			spawner_is_building)


func _spawn_light_at(info: ModelLight, world_pos: Vector3,
		owner_id: int = 0, blink_owner: Array = [],
		spawner_is_building: bool = false) -> int:
	var has_blink := blink_owner.size() >= 2
	return int(_scene.spawn_model_light({
		"position": world_pos,
		"atten_end": info.atten_end,
		"style": info.colorgen_style,
		"phase": info.colorgen_phase,
		"rate": info.colorgen_rate,
		"color_start": info.color_start,
		"color_end": info.color_end,
		"attach_bone": info.subobject,
		"spawning_entity": owner_id,
		"spawner_is_building": spawner_is_building,
		"blink_owner_entity": int(blink_owner[0]) if has_blink else 0,
		"blink_section": int(blink_owner[1]) if has_blink else 0,
		"disable_corona": info.disable_corona,
		"disable_terrain": info.disable_lightterrain,
		"disable_objects": info.disable_lightobjects,
	}))


## The blink-box owner at one world point: retail runs ONE query at the
## spawning entity's position before walking its LGHT records, and slot 0's hit
## names the containing building + section every unattached record binds to
## [orig: Entity_SpawnGlowEffects @ 0x56c7fc -> Entity_QueryBlinkBoxesAtPoint
## @ 0x4af350]. Returns [owner id, section], or an empty array outdoors. Both
## individual ObjectModels and batched buildings resolve into the active-group
## domain their respective draw contexts declare.
func _blink_owner_at(world_pos: Vector3) -> Array:
	var sim: Simulation = _sim()
	if sim == null:
		return []
	var hit: PackedInt64Array = sim.query_blink_owner_at(world_pos)
	if hit.size() < 2:
		return []
	var owner := _owner_id_for_bms(int(hit[0]))
	return [owner, int(hit[1])] if owner != 0 else []


## The owner id a containing building's bms_id resolves to — the SAME id its
## own draw context declares, or owner gating never matches.
func _owner_id_for_bms(bms_id: int) -> int:
	if bms_id == 0:
		return 0
	if _static_owner_by_bms.has(bms_id):
		return int(_static_owner_by_bms[bms_id])
	var cached: Variant = _blink_owner_cache.get(bms_id)
	if cached != null:
		return int(cached)
	var owner := 0
	var runtime: MissionPresentation = _world.get_runtime() if _world != null else null
	if runtime != null:
		var registry: EntityIndex = runtime.get_registry()
		if registry != null:
			var node: ObjectModel = registry.resolve_single(bms_id)
			if node != null:
				owner = owner_id_for_node(node)
	_blink_owner_cache[bms_id] = owner
	return owner


func _sim() -> Simulation:
	if _world == null:
		return null
	var runtime: MissionPresentation = _world.get_runtime()
	return runtime.get_sim() if runtime != null else null


## Typed accessors for co-consumers of the shared pool (the render-slot
## shadow device's dominant-light pick reads the same LightScene).
func scene() -> LightScene:
	return _scene


func light_gain() -> Vector3:
	var gain := Vector3.ONE
	var env: MissionEnvironment = _world.get_environment_node()
	if env != null:
		var state: EnvLightState = env.get_light_state()
		if state != null and state.get_values() != null:
			gain = state.get_values().get_gain()
	return gain


## Build the immutable-index static atlas rows. The placer owns row identity
## and exact ROBJ bounds; this device supplies the same owner/interior groups
## as the live-model pass, selects the witnessed nearest four, and publishes
## the RGBAF payload consumed through INSTANCE_CUSTOM.x.
func _render_static_light_rows(gain: Vector3, weather: Weather,
		time_ms: int) -> void:
	var revision := int(_static_draw_source_revision.call()) \
			if _static_draw_source_revision.is_valid() else 0
	if revision != _static_rows_revision:
		_rebuild_static_light_rows()
		_static_rows_revision = revision
	_scene.render_static_frame(_static_rows_bounds,
			_static_rows_owner_entities, _static_rows_owner_sections,
			_static_rows_interior_owners, _static_rows_interior_sections,
			_static_rows_active, gain, time_ms, weather, _static_rows_revision)


func _rebuild_static_light_rows() -> void:
	var descriptors: Array = _static_draw_sources.call() \
			if _static_draw_sources.is_valid() else []
	var row_count := 0
	for descriptor: StaticLightDrawSource in descriptors:
		row_count = max(row_count, descriptor.atlas_row + 1)
	var bounds_position_size := PackedVector3Array()
	var owner_entities := PackedInt64Array()
	var owner_sections := PackedInt32Array()
	var interior_owners := PackedInt64Array()
	var interior_sections := PackedInt32Array()
	var active := PackedByteArray()
	bounds_position_size.resize(row_count * 2)
	owner_entities.resize(row_count)
	owner_sections.resize(row_count)
	interior_owners.resize(row_count)
	interior_sections.resize(row_count)
	active.resize(row_count)
	for descriptor: StaticLightDrawSource in descriptors:
		var atlas_row := descriptor.atlas_row
		var source_index := descriptor.source_index
		if atlas_row < 0 or atlas_row >= row_count or source_index < 0 or \
				source_index >= _static_sources_snapshot.size():
			continue
		var source: StaticEffectSource = _static_sources_snapshot[source_index]
		var world_bounds := descriptor.world_bounds
		bounds_position_size[atlas_row * 2] = world_bounds.position
		bounds_position_size[atlas_row * 2 + 1] = world_bounds.size
		active[atlas_row] = 1 if descriptor.active else 0
		var static_owner := owner_id_for_static_source(source_index)
		var is_building := (descriptor.kind if descriptor.kind >= 0 else source.kind) \
				== MissionData.KIND_BUILDING
		if is_building:
			# A building declares itself as interior section zero and re-scopes
			# the owner section to this exact ROBJ.
			owner_entities[atlas_row] = 0
			owner_sections[atlas_row] = descriptor.robj_index
			interior_owners[atlas_row] = static_owner
			interior_sections[atlas_row] = 0
		else:
			owner_entities[atlas_row] = static_owner
			owner_sections[atlas_row] = 0
			var xform: Transform3D = source.world_transform
			var interior := _blink_owner_at(xform.origin)
			if interior.size() >= 2:
				interior_owners[atlas_row] = int(interior[0])
				interior_sections[atlas_row] = int(interior[1])
	_static_rows_bounds = bounds_position_size
	_static_rows_owner_entities = owner_entities
	_static_rows_owner_sections = owner_sections
	_static_rows_interior_owners = interior_owners
	_static_rows_interior_sections = interior_sections
	_static_rows_active = active


## The per-frame device leg (GameFramePipeline, after iris, before the
## material frame): one draw context per visible ObjectModel near the camera
## (owner group = that model's entity id) plus the first-person viewmodel
## parts (owner = the local player, so its own muzzle glow reaches the arms).
## The FLICKER phase reads the live weather wave ring; the ambient scale is
## the env light-state gain (the ported EffectWorld_AmbientScale channel).
func render_frame(camera: Camera3D, viewmodel_parts: Array[ObjectModel] = [],
		viewmodel_wire_handle: int = -1, run_census: bool = true) -> void:
	if camera == null:
		_scene.clear_render_output()
		_clear_coronas()
		return
	var gain := light_gain()
	var env: MissionEnvironment = _world.get_environment_node()
	var weather: Weather = _world.get_weather_node()
	var cam_pos := camera.get_camera_transform().origin
	var time_ms := Time.get_ticks_msec()
	# The reused frame arrays are appended through the members directly: a
	# local alias of a packed array shares its CoW buffer, so the first append
	# through the alias would copy it away from the member.
	_frame_models.clear()
	_frame_owners.clear()
	# interior_*: the second witnessed group — the building each draw currently
	# stands inside, plus that blink volume's section [orig:
	# setup_terrain_effect_for_entity @ 0x5c74a0 ->
	# Lighting_SetInteriorLightGroup @ 0x5a90e0].
	_frame_interior_owners.clear()
	_frame_interior_sections.clear()
	_frame_robj_scoped.clear()
	_render_static_light_rows(gain, weather, time_ms)
	_refresh_interior_groups()
	var container: Node = _world.get_node_or_null(NodePath("MissionObjects"))
	if container != null:
		_ensure_model_registry(container)
		for i in range(_reg_models.size()):
			var model := _reg_models[i]
			if not is_instance_valid(model) or not model.is_visible_in_tree():
				continue
			if model.global_position.distance_to(cam_pos) > QUERY_RADIUS:
				continue
			_frame_models.append(model)
			_frame_owners.append(_reg_owners[i])
			_frame_robj_scoped.append(_reg_robj_scoped[i])
			var interior_owner := 0
			var interior_section := 0
			var bms_id := int(_reg_bms_ids[i])
			if bms_id != 0:
				var base: Variant = _interior_index.get(bms_id)
				if base != null:
					var owner := _owner_id_for_bms(
							int(_interior_rows[int(base) + 1]))
					if owner != 0:
						interior_owner = owner
						interior_section = int(_interior_rows[int(base) + 2])
			_frame_interior_owners.append(interior_owner)
			_frame_interior_sections.append(interior_section)
	# The first-person parts inherit the LOCAL PLAYER's interior group, so the
	# room's lights reach the arms and weapon the same way they reach the
	# third-person body standing there.
	var viewmodel_interior := _local_player_interior_group()
	for part in viewmodel_parts:
		if part == null or not part.is_visible_in_tree():
			continue
		_frame_models.append(part)
		_frame_owners.append(owner_id_for_wire(viewmodel_wire_handle)
				if viewmodel_wire_handle >= 0 else part.get_instance_id())
		_frame_robj_scoped.append(0)
		_frame_interior_owners.append(int(viewmodel_interior[0]))
		_frame_interior_sections.append(int(viewmodel_interior[1]))
	# Census select first (report rows for F3 and the seam tests), then the
	# gameplay per-model pass — its mode/isolation stamp is what the report
	# ends the frame with. The census publishes nothing to materials, so the
	# hot caller skips it while the F3 Stats capture is off; a diagnostics
	# read refreshes it on demand (run_census_now).
	_census_cam_pos = cam_pos
	if run_census:
		_scene.render_frame(cam_pos, QUERY_RADIUS, gain, time_ms, weather)
		_census_stale = false
	else:
		_census_stale = true
	_scene.render_model_frame(_frame_models, _frame_owners,
			_frame_interior_owners, _frame_interior_sections,
			_frame_robj_scoped, gain, time_ms, weather)
	_render_coronas(camera, gain, weather, _frame_models, _frame_owners, env)


## On-demand census refresh for report readers while the capture is off: the
## skipped select re-runs with the last frame's camera, so an MCP/diagnostics
## read stays exact without the per-frame report cost.
func run_census_now() -> void:
	if not _census_stale:
		return
	# census_frame refreshes the rows without restamping the report's mode:
	# a report read with the F3 stats off keeps saying what the gameplay
	# pass (render_model_frame) reported.
	_scene.census_frame(_census_cam_pos, QUERY_RADIUS, light_gain(),
			Time.get_ticks_msec(), _world.get_weather_node())
	_census_stale = false


## The interior-group rows (bms_id -> containing bms_id + section for every
## entity standing inside a blink volume) are sim tick products: fetch them
## once per logic tick into the flat rows plus a bms_id -> base-index lookup,
## so the per-frame walk reads them without allocating a row per entity.
func _refresh_interior_groups() -> void:
	var sim: Simulation = _sim()
	if sim == null:
		if not _interior_index.is_empty():
			_interior_index.clear()
			_interior_rows = PackedInt64Array()
		_interior_tick = -1
		return
	var tick := int(sim.get_logic_tick())
	if tick == _interior_tick:
		return
	_interior_tick = tick
	_interior_rows = sim.get_entity_interior_groups()
	_interior_index.clear()
	var i := 0
	while i + 2 < _interior_rows.size():
		_interior_index[int(_interior_rows[i])] = i
		i += 3


## Rebuild the MissionObjects walk registry only when membership changed.
## Owner identity and kind come off entity_ref, stamped once before a node's
## first light frame, so registration-time reads hold for its tree lifetime.
func _ensure_model_registry(container: Node) -> void:
	var container_id := container.get_instance_id()
	if container_id != _reg_container_id:
		_reg_container_id = container_id
		_reg_dirty = true
		if not container.child_entered_tree.is_connected(
				_on_container_membership_changed):
			container.child_entered_tree.connect(
					_on_container_membership_changed)
		if not container.child_exiting_tree.is_connected(
				_on_container_membership_changed):
			container.child_exiting_tree.connect(
					_on_container_membership_changed)
	if _reg_dirty:
		_rebuild_model_registry(container)


func _on_container_membership_changed(_node: Node) -> void:
	_reg_dirty = true
	# A bms id's resolved owner can change with membership (despawn/respawn),
	# so the blink-owner cache follows the registry.
	_blink_owner_cache.clear()


func _rebuild_model_registry(container: Node) -> void:
	_reg_models.clear()
	_reg_owners.clear()
	_reg_robj_scoped.clear()
	_reg_bms_ids.clear()
	for child in container.get_children():
		var model := child as ObjectModel
		if model == null:
			continue
		var ref: EntityRef = model.entity_ref
		_reg_models.append(model)
		_reg_owners.append(owner_id_for_node(model))
		_reg_robj_scoped.append(1 if ref != null \
				and ref.kind == MissionData.KIND_BUILDING else 0)
		_reg_bms_ids.append(ref.bms_id if ref != null else 0)
	_reg_dirty = false


## The local player's interior group as [owner id, section]; [0, 0] outdoors.
## The player is a spawned entity with no bms_id, so it never appears in the
## interior-group rows.
func _local_player_interior_group() -> Array:
	var sim: Simulation = _sim()
	if sim == null:
		return [0, 0]
	var hit: PackedInt64Array = sim.local_player_interior_group()
	if hit.size() < 2:
		return [0, 0]
	var owner := _owner_id_for_bms(int(hit[0]))
	return [owner, int(hit[1])] if owner != 0 else [0, 0]


## The corona device leg: fetch this frame's additive quads from the portable
## walk and rebuild the MultiMesh (instance origin = segment center, uniform
## scale = half-size, instance color = the premultiplied additive color).
## The models/owners arrays are the per-model pass's own walk — models with
## an occlusion section-mask verdict gate their owned coronas on the
## visible-section bit [orig: Terrain_IsBuildingSectionBitSet @ 0x5c6960];
## the env fog rides in as the fog-to-black fold
## [orig: CD3DDevice_SetFogAndBlendMode(dev, 2) @ 0x5aafb6].
func _render_coronas(camera: Camera3D, gain: Vector3, weather: Weather,
		models: Array[Node3D], owners: PackedInt64Array,
		env: MissionEnvironment) -> void:
	_corona_frame = (_corona_frame + 1) & 3
	var fog: EnvLightValues = null
	if env != null:
		var state: EnvLightState = env.get_light_state()
		if state != null:
			fog = state.get_values()
	var instance := _ensure_corona_instance()
	if instance == null:
		return
	# One native buffer write instead of two RenderingServer commands per row
	# (the witnessed jitter re-centers every corona every frame, so there is
	# no change to gate on); rows past this frame's count stay hidden through
	# visible_instance_count.
	var rows := _scene.fill_corona_multimesh(
			camera.get_camera_transform().origin,
			-camera.get_camera_transform().basis.z, gain,
			Time.get_ticks_msec(), _corona_frame, weather, models, owners,
			fog, instance.multimesh)
	instance.visible = rows > 0


func _clear_coronas() -> void:
	if _corona_instance != null and is_instance_valid(_corona_instance):
		_corona_instance.multimesh.instance_count = 0
		_corona_instance.visible = false


func _ensure_corona_instance() -> MultiMeshInstance3D:
	if _corona_instance != null and is_instance_valid(_corona_instance):
		return _corona_instance
	if _world == null:
		return null
	var mmi := MultiMeshInstance3D.new()
	mmi.name = "EffectLightCoronas"
	var mesh := MultiMesh.new()
	mesh.transform_format = MultiMesh.TRANSFORM_3D
	mesh.use_colors = true
	var quad := QuadMesh.new()
	quad.size = Vector2(2.0, 2.0)  # VERTEX.xy in [-1, 1] x half_size
	var material := ShaderMaterial.new()
	material.shader = load("res://shaders/light_corona.gdshader")
	material.set_shader_parameter("u_corona_tex", _corona_texture())
	quad.material = material
	mesh.mesh = quad
	mmi.multimesh = mesh
	mmi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	# Coronas draw in the mirror scene too [orig: the
	# Water_RenderReflectedWorldScene call @ 0x5c85fd].
	mmi.layers = Water.VISUAL_LAYER_WORLD
	# The quads billboard in-shader from rows anywhere in the world; the
	# static AABB only seeds Godot's sort and the cull margin keeps the
	# instance from being frustum-culled once the camera leaves that box
	# (the StarField precedent in Celestial).
	mmi.custom_aabb = AABB(Vector3(-512, -512, -512), Vector3(1024, 1024, 1024))
	mmi.extra_cull_margin = 1.0e6
	_world.add_child(mmi)
	_corona_instance = mmi
	return mmi


## The procedural corona texture "texlightcrn": the law (the 128x128
## 0.4 - 0.45 d falloff, truncated, the transparent border) lives portable
## in renderer::corona_texture_argb; this only wraps the bytes in a texture.
static var _corona_texture_cache: ImageTexture


static func _corona_texture() -> ImageTexture:
	if _corona_texture_cache != null:
		return _corona_texture_cache
	var size: int = LightScene.corona_texture_size()
	var image := Image.create_from_data(size, size, false, Image.FORMAT_RGBA8,
			LightScene.corona_texture_rgba8())
	_corona_texture_cache = ImageTexture.create_from_image(image)
	return _corona_texture_cache


## The 62 Hz lifecycle decay [orig: EffectWorld_TickInstancesAndLightScale
## @ 0x5aa170 from the main loop] — beside EffectWorld.advance_fixed_tick.
func advance_fixed_tick() -> void:
	_scene.advance_fixed_tick()


## One weapon fire with the ammo MF_Light flag [orig: Entity_UpdateMuzzleGlow-
## Effect @ 0x56c960, called per shot from both fire arms]. Owner = the
## shooter, so the per-draw owner select (render_model_frame) admits the glow
## only on draws declaring that owner — the shooter's body, and the
## first-person parts the world tags with the local player's id (D-AI-8d).
## The cache is deliberately shared with model LGHT: if mission-start spawn
## left entity+0x1B4 nonzero, retail re-arms and moves that final authored
## lease instead of allocating the 1.5-unit muzzle-color light.
func on_muzzle_fire(shooter_handle: int, world_pos: Vector3) -> void:
	var owner_id := owner_id_for_wire(shooter_handle)
	var handle := int(_entity_effect_handles.get(owner_id, 0))
	if handle == 0:
		handle = int(_scene.spawn_glow({
			"position": world_pos,
			"radius": LightScene.muzzle_glow_radius(),
			"color": LightScene.muzzle_glow_color(),
			"fade_mode": 3,
			"fade_duration": -1,
			"owner_entity": owner_id,
		}))
		if handle == 0:
			return
		_entity_effect_handles[owner_id] = handle
	_scene.set_light_fade(handle, LightScene.muzzle_glow_fade_mode(),
			LightScene.muzzle_glow_fade_ticks())
	_scene.set_light_owner(handle, owner_id, 0)
	_scene.set_light_position(handle, world_pos)
	_scene.set_light_blend(handle, 1.0)


## One presented round impact whose ammo authors light_impact [orig:
## AmmoDef_ProcessImpactEffect @ 0x40a2b3 — spawned radius/2 above the
## impact, mode 2 fade, render flag 0x100]. The 0x100 flag's one witnessed
## reader is the corona walk: the billboards re-center radius/2 below the
## light, back onto the impact point [orig: EffectWorld_RenderLightCoronas
## @ 0x5ab037..0x5ab05c].
func on_impact_light(world_pos: Vector3, radius: float, color: Color,
		duration_ticks: int) -> void:
	if radius <= 0.0:
		return
	_scene.spawn_glow({
		"position": world_pos + Vector3(0.0, radius * 0.5, 0.0),
		"radius": radius,
		"color": color,
		"fade_mode": 2,
		"fade_duration": duration_ticks,
		"corona_lower_half_radius": true,
	})


## One husk death flash [orig: Entity_SpawnDeathPieces @ 0x49351a — at the
## entity position, 2x the piece model radius, corona disabled].
func on_death_light(world_pos: Vector3, radius: float) -> void:
	if radius <= 0.0:
		return
	_scene.spawn_glow({
		"position": world_pos,
		"radius": radius,
		"color": LightScene.death_flash_color(),
		"fade_mode": LightScene.death_flash_fade_mode(),
		"fade_duration": LightScene.death_flash_fade_ticks(),
		"disable_corona": true,
	})


## The in-flight light_move glows, diffed against the sim's live rows [orig:
## RoundData_SpawnRound @ 0x4ec8da spawn (mode 1, radius/2 up, terrain
## disabled), the per-tick follow @ 0x4eaa9f, Projectile_ReleaseEffects
## clear]. Rows: RoundGlowRow from Simulation.get_round_glow_rows.
func sync_round_glows(rows: Array) -> void:
	var seen: Dictionary = {}
	for row_v in rows:
		var row: RoundGlowRow = row_v
		var id := row.id
		seen[id] = true
		var radius := row.radius
		var pos := row.pos
		var handle := int(_round_handles.get(id, 0))
		if handle == 0:
			# The spawn rides radius/2 above the round; the per-tick follow
			# re-centers at the raw round position [orig: @ 0x4ec8d6 vs the
			# @ 0x4eaa9f SetPositionAndBounds follow].
			handle = int(_scene.spawn_glow({
				"position": pos + Vector3(0.0, radius * 0.5, 0.0),
				"radius": radius,
				"color": row.color,
				"fade_mode": 1,
				"fade_duration": -1,
				"disable_terrain": true,
			}))
			if handle != 0:
				_round_handles[id] = handle
		else:
			_scene.set_light_position(handle, pos)
	for id_v in _round_handles.keys():
		if not seen.has(id_v):
			_scene.despawn(int(_round_handles[id_v]))
			_round_handles.erase(id_v)


func get_report() -> EffectLightReport:
	run_census_now()
	return _scene.get_report()
