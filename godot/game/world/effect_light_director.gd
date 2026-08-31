class_name EffectLightDirector
extends RefCounted

## The EffectWorld dynamic point-light director: spawns one pool light per
## authored model light record for every placed entity, and presents the pool
## as real scene OmniLight3D nodes (ADR 0043 — LightScene.sync_scene_lights;
## Godot's clustered lighting replaced the retired per-draw select and its
## instance-uniform / static-atlas delivery). The spawn lifecycle stays the
## witnessed one — mission start walks the placed pools spawning per-record
## instances [orig: Game_StartMission @ 0x525d19 ->
## Game_SpawnAllEntityGlowEffects @0x5227b0 -> Entity_SpawnGlowEffects
## @ 0x56c7c0]; authored LGHT positions/lifetimes are spawn-fixed; powerup
## respawn is routed, and a husk swap neither moves nor rescans lights.
## Corona billboards draw per frame from the portable corona walk
## [orig: EffectWorld_RenderLightCoronas @ 0x5aaf40].

## Model gather half-extent around the camera for the corona owner walk.
const QUERY_RADIUS := 512.0

# Light owner zero is retail's unowned/world sentinel. A decoded wire handle
# is an independent 16-bit domain in which zero is valid, so tag every wire
# identity into a nonzero, non-ObjectID range before it reaches LightScene.
const WIRE_OWNER_TAG := 1 << 48
const STATIC_OWNER_TAG := 2 << 48

var _world: GameWorld
var _static_sources := Callable()
var _scene: LightScene = LightScene.new()
# The scene-light parent node the pooled OmniLight3D presentation lives under
# (ADR 0043).
var _light_parent: Node3D
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
# Containing-building bms_id -> owner id, rebuilt per spawn pass and per render
# frame (nodes are recreated across reloads, so nothing survives a pass).
var _blink_owner_cache: Dictionary = {}


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
	_static_sources_snapshot.clear()
	_static_owner_by_bms.clear()
	_spawned_nodes.clear()
	_entity_effect_handles.clear()
	_round_handles.clear()
	_blink_owner_cache.clear()
	_clear_coronas()


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
	_static_sources_snapshot = _static_sources.call() \
			if _static_sources.is_valid() else []
	# Build every BMS identity before resolving any blink containment. A static
	# item can spawn inside a batched building that appears later in the source
	# walk, and retail still binds it to that building's owner group.
	for source_index in range(_static_sources_snapshot.size()):
		var mapped_source: Dictionary = _static_sources_snapshot[source_index]
		var bms_id := int(mapped_source.get("bms_id", 0))
		if bms_id != 0:
			_static_owner_by_bms[bms_id] = owner_id_for_static_source(source_index)
	for source_index in range(_static_sources_snapshot.size()):
		var source: Dictionary = _static_sources_snapshot[source_index]
		if _spawned_static.has(source_index):
			continue
		var data: ObjectData = source.get("object_data")
		if data == null:
			continue
		# Batched statics are entities too: a subobject record binds to this
		# tagged owner identity.
		# [orig: Entity_SpawnGlowEffects @ 0x56c8ae; SetOwnerGroup(entity,bone)]
		var xform: Transform3D = source.get("world_transform", Transform3D.IDENTITY)
		var is_building := int(source.get("kind", -1)) == \
				MissionData.KIND_BUILDING
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
	var ref: Dictionary = node.get_meta("entity_ref", {})
	var wire := int(ref.get("wire_handle", -1))
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
	var ref: Dictionary = node.get_meta("entity_ref", {})
	var is_building := int(ref.get("kind", -1)) == MissionData.KIND_BUILDING
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


## One authored light record (the get_light_info dictionary shape) becomes one
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
func spawn_light_record(info: Dictionary, world_transform: Transform3D,
		owner_id: int = 0, blink_owner: Array = [],
		spawner_is_building: bool = false) -> int:
	if info.is_empty():
		return 0
	var world_pos: Vector3 = world_transform * Vector3(
			info.get("position", Vector3.ZERO))
	return _spawn_light_at(info, world_pos, owner_id, blink_owner,
			spawner_is_building)


func _spawn_light_at(info: Dictionary, world_pos: Vector3,
		owner_id: int = 0, blink_owner: Array = [],
		spawner_is_building: bool = false) -> int:
	var has_blink := blink_owner.size() >= 2
	return int(_scene.spawn_model_light({
		"position": world_pos,
		"atten_end": float(info.get("atten_end", 0.0)),
		"style": int(info.get("colorgen_style", 0)),
		"phase": int(info.get("colorgen_phase", 0)),
		"rate": int(info.get("colorgen_rate", 0)),
		"color_start": info.get("color_start", Color.WHITE),
		"color_end": info.get("color_end", Color.WHITE),
		"attach_bone": int(info.get("subobject", 0)),
		"spawning_entity": owner_id,
		"spawner_is_building": spawner_is_building,
		"blink_owner_entity": int(blink_owner[0]) if has_blink else 0,
		"blink_section": int(blink_owner[1]) if has_blink else 0,
		"disable_corona": bool(info.get("disable_corona", false)),
		"disable_terrain": bool(info.get("disable_lightterrain", false)),
		"disable_objects": bool(info.get("disable_lightobjects", false)),
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


## The per-frame device leg (GameFramePipeline, after iris, before the
## material frame): present the pool as scene omni lights (ADR 0043) and
## rebuild the corona billboards. The FLICKER phase reads the live weather
## wave ring; the ambient scale is the env light-state gain (the ported
## EffectWorld_AmbientScale channel).
func render_frame(camera: Camera3D) -> void:
	if camera == null:
		_scene.clear_render_output()
		_clear_coronas()
		return
	var gain := light_gain()
	var env: MissionEnvironment = _world.get_environment_node()
	var weather: Weather = _world.get_weather_node()
	var cam_pos := camera.get_camera_transform().origin
	var time_ms := Time.get_ticks_msec()
	# The corona owner walk: models near the camera, so owned coronas gate on
	# their owner's visible-section bits.
	var models: Array[Node3D] = []
	var owners := PackedInt64Array()
	_blink_owner_cache.clear()
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
	# Census select (report rows for F3 and the seam tests), then the scene
	# omni sync — the presentation the lit pipeline consumes.
	_scene.render_frame(cam_pos, QUERY_RADIUS, gain, time_ms, weather)
	_scene.sync_scene_lights(_ensure_light_parent(), gain, time_ms, weather)
	_render_coronas(camera, gain, weather, models, owners, env)


func _ensure_light_parent() -> Node3D:
	if _light_parent != null and is_instance_valid(_light_parent):
		return _light_parent
	_light_parent = Node3D.new()
	_light_parent.name = "EffectLights"
	_world.add_child(_light_parent)
	return _light_parent


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
	var fog: Dictionary = {}
	if env != null:
		var state: EnvLightState = env.get_light_state()
		if state != null and state.get_values() != null:
			var values: EnvLightValues = state.get_values()
			fog = {
				"enabled": values.get_fog_enabled(),
				"type": values.get_fog_type(),
				"start": values.get_fog_start(),
				"end": values.get_fog_end(),
			}
	var rows: Array = _scene.collect_corona_rows(
			camera.get_camera_transform().origin,
			-camera.get_camera_transform().basis.z, gain,
			Time.get_ticks_msec(), _corona_frame, weather, models, owners,
			fog)
	var instance := _ensure_corona_instance()
	if instance == null:
		return
	var mesh: MultiMesh = instance.multimesh
	mesh.instance_count = rows.size()
	instance.visible = not rows.is_empty()
	for i in range(rows.size()):
		var row: Dictionary = rows[i]
		var half := float(row.get("half_size", 0.0))
		var center: Vector3 = row.get("position", Vector3.ZERO)
		var color: Color = row.get("color", Color.BLACK)
		mesh.set_instance_transform(i, Transform3D(
				Basis.IDENTITY.scaled(Vector3(half, half, half)), center))
		mesh.set_instance_color(i, color)


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
## shooter (under ADR 0043 the glow is a scene light; the owner id still
## keys the shared entity+0x1B4 lease cache below).
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
