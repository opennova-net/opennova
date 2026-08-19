class_name EffectLightDirector
extends RefCounted

## The EffectWorld dynamic point-light director: spawns one pool light per
## authored model light record for every placed entity, and drives the
## per-frame select that feeds the technique shaders' global parameters.
## The witness map lives on engine/runtime/renderer/light_scene.h — mission
## start walks the placed pools spawning per-record instances
## [orig: Game_StartMission @ 0x525d19 -> sub_5227B0 ->
## Entity_SpawnGlowEffects @ 0x56c7c0], and each draw selects the nearest
## group-passing four [orig: collect_nearby_zones_by_aabb @ 0x5aa250;
## update_light_slots @ 0x5abc50]. The object pass now runs per rendered
## model: one draw context per visible ObjectModel with its entity as the
## owner group, so owned lights (muzzle glow, subobject records) light only
## their owner exactly as retail's update_light_slots gates them. Remaining
## D-RLIT-4 residuals: interior groups, corona, terrain projected-texture
## light, foliage sampling, and subobject bone following.

## Model gather half-extent around the camera. Light ranges are authored
## small (atten_end 8 on the fire barrels), so any model a pool light could
## touch sits well inside this radius.
const QUERY_RADIUS := 512.0

## The muzzle glow constants [orig: Entity_UpdateMuzzleGlowEffect @ 0x56c960 —
## radius 98304 (1.5), color 0xFFE0A0, re-armed per shot to mode 4 / 5 ticks].
const MUZZLE_RADIUS := 1.5
const MUZZLE_COLOR := Color(255.0 / 255.0, 224.0 / 255.0, 160.0 / 255.0)
## The death flash [orig: Entity_SpawnDeathPieces @ 0x49351a — color 0xFFC080,
## mode 2 / 31 ticks, corona disabled].
const DEATH_COLOR := Color(255.0 / 255.0, 192.0 / 255.0, 128.0 / 255.0)
const DEATH_TICKS := 31

var _world: GameWorld
var _static_sources := Callable()
var _scene: LightScene = LightScene.new()
var _spawned_static: Dictionary = {}
var _spawned_nodes: Dictionary = {}
# shooter wire handle -> pool light handle. Mirrors retail's entity+436 cache:
# spawn once per shooter, re-arm per shot, and once the 5-tick fade kills the
# slot the stale handle stays cached (that shooter's glow is gone for this
# life — the witnessed behavior, light_scene.h map).
var _muzzle_handles: Dictionary = {}
# round presentation id -> pool light handle (the light_move follow).
var _round_handles: Dictionary = {}


func setup(world: GameWorld, static_sources: Callable) -> void:
	_world = world
	_static_sources = static_sources


## Mission teardown: disconnect live node retirement hooks, retire every pool
## lease, and synchronously clear the shader-global output.
func reset() -> void:
	for node_id_v in _spawned_nodes.keys():
		_disconnect_wire_node_exit(_spawned_nodes[node_id_v])
	_scene.clear()
	_spawned_static.clear()
	_spawned_nodes.clear()
	_muzzle_handles.clear()
	_round_handles.clear()


## Mission start / sim-restart: reset, then respawn from the restored entity
## set — the same lifecycle the item-effect director uses.
func reattach() -> void:
	reset()
	var container: Node = _world.get_node_or_null(NodePath("MissionObjects"))
	if container != null:
		for child in container.get_children():
			var node := child as ObjectModel
			if node == null or not node.has_meta("entity_ref"):
				continue
			on_wire_node_spawned(node, -1, 0)
	var sources: Array = _static_sources.call() if _static_sources.is_valid() else []
	for source_index in range(sources.size()):
		var source: Dictionary = sources[source_index]
		if _spawned_static.has(source_index):
			continue
		var data: ObjectData = source.get("object_data")
		if data == null:
			continue
		var handles := _spawn_model_lights(data,
				source.get("world_transform", Transform3D.IDENTITY))
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
	var ref: Dictionary = node.get_meta("entity_ref", {})
	var wire := int(ref.get("wire_handle", 0))
	return wire if wire != 0 else node.get_instance_id()


func on_wire_node_spawned(node: ObjectModel, _kind: int, _item_id: int) -> void:
	if node == null:
		return
	var node_id := node.get_instance_id()
	if _spawned_nodes.has(node_id):
		return
	var data: ObjectData = node.get_object_data()
	if data == null:
		return
	var handles := _spawn_model_lights(data, node.global_transform,
			owner_id_for_node(node))
	if not handles.is_empty():
		var on_exit := _on_wire_node_exiting.bind(node_id)
		_spawned_nodes[node_id] = {
			"node": weakref(node),
			"handles": handles,
			"tree_exiting": on_exit,
		}
		node.tree_exiting.connect(on_exit, Object.CONNECT_ONE_SHOT)


func _on_wire_node_exiting(node_id: int) -> void:
	var record: Dictionary = _spawned_nodes.get(node_id, {})
	if record.is_empty():
		return
	_spawned_nodes.erase(node_id)
	for handle_v in record.get("handles", []):
		_scene.despawn(int(handle_v))


func _disconnect_wire_node_exit(record: Dictionary) -> void:
	var node_ref := record.get("node") as WeakRef
	var on_exit: Callable = record.get("tree_exiting", Callable())
	var node: Node = null
	if node_ref != null:
		node = node_ref.get_ref() as Node
	if node != null and on_exit.is_valid() and node.tree_exiting.is_connected(on_exit):
		node.tree_exiting.disconnect(on_exit)


func _spawn_model_lights(data: ObjectData, world_transform: Transform3D,
		owner_id: int = 0) -> Array[int]:
	var handles: Array[int] = []
	if data == null:
		return handles
	for i in range(data.get_light_count()):
		var handle := spawn_light_record(data.get_light_info(i), world_transform,
				owner_id)
		if handle != 0:
			handles.append(handle)
	return handles


## One authored light record (the get_light_info dictionary shape) becomes one
## pool instance. Public: the GUT seam test feeds records directly. A record
## attached to a subobject retains retail's owner metadata for future per-draw
## selection (interior cabin lights)
## [orig: Entity_SpawnGlowEffects @ 0x56c8ae — SetOwner(entity, bone) when the
## record's attach bone != 0; the blink-box interior leg is a tracked
## residual]. Batched static sources have no individual owner node: their
## owner_id remains zero and they are mission-start world lights. The current
## camera-global object pass admits nonzero owners unscoped, so it does not yet
## enforce self-only illumination.
func spawn_light_record(info: Dictionary, world_transform: Transform3D,
		owner_id: int = 0) -> int:
	if info.is_empty():
		return 0
	var world_pos: Vector3 = world_transform * Vector3(
			info.get("position", Vector3.ZERO))
	var subobject := int(info.get("subobject", 0))
	return int(_scene.spawn_model_light({
		"position": world_pos,
		"atten_end": float(info.get("atten_end", 0.0)),
		"style": int(info.get("colorgen_style", 0)),
		"phase": int(info.get("colorgen_phase", 0)),
		"rate": int(info.get("colorgen_rate", 0)),
		"color_start": info.get("color_start", Color.WHITE),
		"color_end": info.get("color_end", Color.WHITE),
		"owner_entity": owner_id if subobject != 0 else 0,
		"owner_section": subobject,
		"disable_corona": bool(info.get("disable_corona", false)),
		"disable_terrain": bool(info.get("disable_lightterrain", false)),
		"disable_objects": bool(info.get("disable_lightobjects", false)),
	}))


## The per-frame device leg (GameFramePipeline, after iris, before the
## material frame): one draw context per visible ObjectModel near the camera
## (owner group = that model's entity id) plus the first-person viewmodel
## parts (owner = the local player, so its own muzzle glow reaches the arms).
## The FLICKER phase reads the live weather wave ring; the ambient scale is
## the env light-state gain (the ported EffectWorld_AmbientScale channel).
func render_frame(camera: Camera3D, viewmodel_parts: Array[ObjectModel] = [],
		viewmodel_owner: int = 0) -> void:
	if camera == null:
		_scene.clear_render_output()
		return
	var gain := Vector3.ONE
	var env: MissionEnvironment = _world.get_environment_node()
	if env != null:
		var state: EnvLightState = env.get_light_state()
		if state != null and state.get_values() != null:
			gain = state.get_values().get_gain()
	var weather: Node = _world.get_node_or_null(NodePath("Weather"))
	var cam_pos := camera.get_camera_transform().origin
	var models: Array[Node3D] = []
	var owners := PackedInt64Array()
	var container: Node = _world.get_node_or_null(NodePath("MissionObjects"))
	if container != null:
		for child in container.get_children():
			var model := child as ObjectModel
			if model == null or not model.is_visible_in_tree():
				continue
			if model.global_position.distance_to(cam_pos) > QUERY_RADIUS:
				continue
			models.append(model)
			owners.append(owner_id_for_node(model))
	for part in viewmodel_parts:
		if part == null or not part.is_visible_in_tree():
			continue
		models.append(part)
		owners.append(viewmodel_owner if viewmodel_owner != 0
				else part.get_instance_id())
	# Census select first (report rows for F3 and the seam tests), then the
	# gameplay per-model pass — its mode/isolation stamp is what the report
	# ends the frame with.
	_scene.render_frame(cam_pos, QUERY_RADIUS, gain, Time.get_ticks_msec(),
			weather)
	_scene.render_model_frame(models, owners, gain, Time.get_ticks_msec(),
			weather)


## The 62 Hz lifecycle decay [orig: EffectWorld_TickInstancesAndLightScale
## @ 0x5aa170 from the main loop] — beside EffectWorld.advance_fixed_tick.
func advance_fixed_tick() -> void:
	_scene.advance_fixed_tick()


## One weapon fire with the ammo MF_Light flag [orig: Entity_UpdateMuzzleGlow-
## Effect @ 0x56c960, called per shot from both fire arms]. Owner = the
## shooter. The metadata is preserved, but the current camera-global object
## pass admits it unscoped: the flash is visible and may light nearby objects
## until per-draw grouping closes the tracked D-RLIT-4 residual.
func on_muzzle_fire(shooter_handle: int, world_pos: Vector3) -> void:
	var handle := int(_muzzle_handles.get(shooter_handle, 0))
	if handle == 0:
		handle = int(_scene.spawn_glow({
			"position": world_pos,
			"radius": MUZZLE_RADIUS,
			"color": MUZZLE_COLOR,
			"fade_mode": 3,
			"fade_duration": -1,
			"owner_entity": shooter_handle,
		}))
		if handle == 0:
			return
		_muzzle_handles[shooter_handle] = handle
	_scene.set_light_fade(handle, 4, 5)
	_scene.set_light_position(handle, world_pos)
	_scene.set_light_blend(handle, 1.0)


## One presented round impact whose ammo authors light_impact [orig:
## AmmoDef_ProcessImpactEffect @ 0x40a2b3 — spawned radius/2 above the
## impact, mode 2 fade]. The unwitnessed render flag 0x100 is not carried.
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
	})


## One husk death flash [orig: Entity_SpawnDeathPieces @ 0x49351a — at the
## entity position, 2x the piece model radius, corona disabled].
func on_death_light(world_pos: Vector3, radius: float) -> void:
	if radius <= 0.0:
		return
	_scene.spawn_glow({
		"position": world_pos,
		"radius": radius,
		"color": DEATH_COLOR,
		"fade_mode": 2,
		"fade_duration": DEATH_TICKS,
		"disable_corona": true,
	})


## The in-flight light_move glows, diffed against the sim's live rows [orig:
## RoundData_SpawnRound @ 0x4ec8da spawn (mode 1, radius/2 up, terrain
## disabled), the per-tick follow @ 0x4eaa9f, Projectile_ReleaseEffects
## clear]. Rows: {id, pos, radius, color} from Simulation.get_round_glow_rows.
func sync_round_glows(rows: Array) -> void:
	var seen: Dictionary = {}
	for row_v in rows:
		var row: Dictionary = row_v
		var id := int(row.get("id", 0))
		seen[id] = true
		var radius := float(row.get("radius", 0.0))
		var pos: Vector3 = row.get("pos", Vector3.ZERO)
		var handle := int(_round_handles.get(id, 0))
		if handle == 0:
			# The spawn rides radius/2 above the round; the per-tick follow
			# re-centers at the raw round position [orig: @ 0x4ec8d6 vs the
			# @ 0x4eaa9f SetPositionAndBounds follow].
			handle = int(_scene.spawn_glow({
				"position": pos + Vector3(0.0, radius * 0.5, 0.0),
				"radius": radius,
				"color": row.get("color", Color.WHITE),
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
	return EffectLightReport.from_ffi_dictionary(_scene.get_report())
