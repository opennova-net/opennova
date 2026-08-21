class_name EffectLightDirector
extends RefCounted

## The EffectWorld dynamic-light director: spawns one pool light per authored
## model light record for every placed entity, evaluates that portable pool,
## and presents every live entry as a pooled Godot OmniLight3D/SpotLight3D.
## The witness map lives on engine/runtime/renderer/light_scene.h — mission
## start walks the placed pools spawning per-record instances
## [orig: Game_StartMission @ 0x525d19 -> sub_5227B0 ->
## Entity_SpawnGlowEffects @ 0x56c7c0], and each draw selects the nearest
## group-passing four [orig: collect_nearby_zones_by_aabb @ 0x5aa250;
## update_light_slots @ 0x5abc50]. The object pass now runs per rendered
## model. Native clustered lighting replaces the fixed-function nearest-four
## delivery; authored ownership remains diagnostic metadata. Corona billboards
## draw per frame from the portable corona walk [orig:
## EffectWorld_RenderLightCoronas @ 0x5aaf40]. Remaining D-RLIT-4 residuals:
## interior groups, terrain projected-texture light, foliage sampling, and
## subobject bone following.

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
# Opaque pool handle -> active native node. Retired nodes stay hidden in their
# kind-specific free list and are reused by later transient lights.
var _native_nodes: Dictionary = {}
var _free_omni: Array[OmniLight3D] = []
var _free_spot: Array[SpotLight3D] = []
var _native_node_serial := 0
# The corona billboard presenter: one MultiMesh of additive camera-facing
# quads rebuilt per frame from the portable corona walk
# [orig: EffectWorld_RenderLightCoronas @ 0x5aaf40 — the witness map lives on
# renderer::LightScene::collect_corona_quads].
var _corona_instance: MultiMeshInstance3D
var _corona_frame := 0


func setup(world: GameWorld, static_sources: Callable) -> void:
	_world = world
	_static_sources = static_sources


## Mission teardown: disconnect live node retirement hooks, retire every pool
## lease, and synchronously retire the active native-light nodes.
func reset() -> void:
	for node_id_v in _spawned_nodes.keys():
		_disconnect_wire_node_exit(_spawned_nodes[node_id_v])
	_scene.clear()
	_clear_native_lights()
	_spawned_static.clear()
	_spawned_nodes.clear()
	_muzzle_handles.clear()
	_round_handles.clear()
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
	var sources: Array = _static_sources.call() if _static_sources.is_valid() else []
	for source_index in range(sources.size()):
		var source: Dictionary = sources[source_index]
		if _spawned_static.has(source_index):
			continue
		var data: ObjectData = source.get("object_data")
		if data == null:
			continue
		# Batched statics have no ObjectModel identity, so each source owns a
		# synthetic negative id. The exact ItemDef type decides whether retail
		# skipped the one spawn-time blink query; native illumination stays
		# spatial regardless of the retained owner metadata.
		var xform: Transform3D = source.get(
				"world_transform", Transform3D.IDENTITY)
		var item_id := int(source.get("item_id", 0))
		var is_building := _is_itemdef_building(item_id)
		var blink_owner: Array = []
		if not is_building:
			blink_owner = _blink_owner_at(xform.origin)
		var handles := _spawn_model_lights(data, xform,
				-(source_index + 1), blink_owner, is_building)
		if not handles.is_empty():
			_spawned_static[source_index] = handles


## Wire/late spawns route here through the world's spawn router (shared with
## the item-effect director).
## The one owner id space: a model's sim wire handle when the present pass
## stamped one (live entities — the same domain fire events report shooters
## in), else the node instance id (placer statics, preview scenes). The same
## id is retained on pool snapshots for diagnostics and corona visibility.
static func owner_id_for_node(node: ObjectModel) -> int:
	var ref: Dictionary = node.get_meta("entity_ref", {})
	var wire := int(ref.get("wire_handle", 0))
	return wire if wire != 0 else node.get_instance_id()


func on_wire_node_spawned(node: ObjectModel, _kind: int, item_id: int) -> void:
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
	var resolved_item_id := item_id if item_id > 0 else int(ref.get("item_id", 0))
	var is_building := _is_itemdef_building(resolved_item_id)
	var blink_owner: Array = []
	if not is_building:
		blink_owner = _blink_owner_at(node.global_position)
	var handles := _spawn_model_lights(data, node.global_transform,
			owner_id_for_node(node), blink_owner, is_building)
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


## One authored light record (the get_light_info dictionary shape) becomes one
## pool instance. Public: the GUT seam test feeds records directly. A record
## keeps the witnessed owner metadata for diagnostics and the corona visibility
## gate: an authored subobject wins, otherwise an enclosing blink section owns
## the record unless the spawning ItemDef is exactly Building. Native clustered
## illumination remains spatial rather than reconstructed per-draw group state.
func spawn_light_record(info: Dictionary, world_transform: Transform3D,
		owner_id: int = 0, blink_owner: Array = [],
		spawner_is_building: bool = false) -> int:
	if info.is_empty():
		return 0
	var world_pos: Vector3 = world_transform * Vector3(
			info.get("position", Vector3.ZERO))
	var local_direction: Vector3 = info.get("direction", Vector3(0.0, 0.0, -1.0))
	var world_direction := (world_transform.basis * local_direction).normalized()
	var has_blink := blink_owner.size() >= 2
	return int(_scene.spawn_model_light({
		"position": world_pos,
		"direction": world_direction,
		"target": int(info.get("light_type", 0)) != 0,
		"spot_angle_degrees": float(info.get("falloff_deg", 45.0)),
		"atten_start": float(info.get("atten_start", 0.0)),
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
## @ 0x4af350]. Returns [owner id, section], or an empty array outdoors — and
## also when the containing building has no presentation node to name, since a
## batched building cannot be addressed in the per-model owner id space
## (tracked on D-RLIT-4).
func _blink_owner_at(world_pos: Vector3) -> Array:
	var sim: Simulation = _sim()
	if sim == null:
		return []
	var hit: PackedInt64Array = sim.query_blink_owner_at(world_pos)
	if hit.size() < 2:
		return []
	var owner := _owner_id_for_bms(int(hit[0]))
	return [owner, int(hit[1])] if owner != 0 else []


## The presentation owner id a containing building's bms_id resolves to. The
## corona visibility walk uses the same id domain as its ObjectModel rows.
func _owner_id_for_bms(bms_id: int) -> int:
	if bms_id == 0:
		return 0
	var runtime: MissionPresentation = _world.get_runtime() if _world != null else null
	if runtime != null:
		var registry: EntityIndex = runtime.get_registry()
		if registry != null:
			var node: ObjectModel = registry.resolve_single(bms_id)
			if node != null:
				return owner_id_for_node(node)
	return 0


## Retail gates on ItemDef.type == Building (5), not MissionData's placement
## kind: that broader kind also contains Decoration/Foliage type 2.
func _is_itemdef_building(item_id: int) -> bool:
	if _world == null or item_id <= 0:
		return false
	var item_db: ItemDatabase = _world.get_item_db()
	return item_db != null \
			and item_db.get_item_type(item_id) == ItemDatabase.TYPE_BUILDING


func _sim() -> Simulation:
	if _world == null:
		return null
	var runtime: MissionPresentation = _world.get_runtime()
	return runtime.get_sim() if runtime != null else null


## The per-frame device leg (GameFramePipeline, after iris, before the
## material frame): evaluate the portable pool once and synchronize native
## clustered lights. The nearby-model walk remains only for owned-corona
## visibility; native lights no longer write per-model shader uniforms.
func render_frame(camera: Camera3D, viewmodel_parts: Array[ObjectModel] = [],
		viewmodel_owner: int = 0) -> void:
	if camera == null or _world == null:
		_scene.clear_render_output()
		_clear_native_lights()
		_clear_coronas()
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
	var light_rows: Array = _scene.collect_active_rows(
			Time.get_ticks_msec(), weather)
	_sync_native_lights(light_rows)
	_render_coronas(camera, gain, weather, models, owners, env)


func _sync_native_lights(rows: Array) -> void:
	var seen: Dictionary = {}
	for value in rows:
		var row: Dictionary = value
		var handle := int(row.get("handle", 0))
		if handle == 0:
			continue
		seen[handle] = true
		var wants_spot := String(row.get("kind", "omni")) == "spot"
		var light := _native_nodes.get(handle) as Light3D
		if light != null and (light is SpotLight3D) != wants_spot:
			_release_native_light(handle)
			light = null
		if light == null:
			light = _acquire_native_light(wants_spot)
			if light == null:
				continue
			_native_nodes[handle] = light
		_configure_native_light(light, row)
	for handle_value in _native_nodes.keys():
		var handle := int(handle_value)
		if not seen.has(handle):
			_release_native_light(handle)


func _acquire_native_light(wants_spot: bool) -> Light3D:
	var light: Light3D
	if wants_spot:
		if not _free_spot.is_empty():
			light = _free_spot.pop_back()
		else:
			light = SpotLight3D.new()
	else:
		if not _free_omni.is_empty():
			light = _free_omni.pop_back()
		else:
			light = OmniLight3D.new()
	if light.get_parent() == null:
		_native_node_serial += 1
		light.name = "EffectLight%04d" % _native_node_serial
		_world.add_child(light)
		light.set_as_top_level(true)
	light.shadow_enabled = false
	return light


func _configure_native_light(light: Light3D, row: Dictionary) -> void:
	var source_color: Color = row.get("color", Color.WHITE)
	light.light_color = source_color
	light.light_energy = maxf(float(row.get("energy", 0.0)), 0.0)
	light.light_cull_mask = _native_light_cull_mask(row)
	light.shadow_enabled = false
	light.global_position = row.get("position", Vector3.ZERO)
	var light_range := maxf(float(row.get("range", 0.0)), 0.05)
	if light is SpotLight3D:
		var spot := light as SpotLight3D
		spot.spot_range = light_range
		spot.spot_angle = clampf(
				float(row.get("spot_angle_degrees", 45.0)), 1.0, 89.0)
		spot.spot_attenuation = 1.0
		var direction: Vector3 = row.get("direction", Vector3(0.0, 0.0, -1.0))
		if direction.length_squared() > 0.000001:
			direction = direction.normalized()
			var up := Vector3.FORWARD if absf(direction.dot(Vector3.UP)) > 0.99 \
					else Vector3.UP
			spot.look_at(spot.global_position + direction, up)
	else:
		var omni := light as OmniLight3D
		omni.omni_range = light_range
		omni.omni_attenuation = 1.0
	light.set_meta("effect_light_handle", int(row.get("handle", 0)))
	light.set_meta("attenuation_start", float(row.get("atten_start", 0.0)))
	light.visible = light.light_energy > 0.001 and light.light_cull_mask != 0


func _native_light_cull_mask(row: Dictionary) -> int:
	var mask := 0
	# Terrain and ordinary world objects currently share the world layer. The
	# authored participation flags still suppress a light completely and keep
	# the viewmodel leg object-only without growing another layer taxonomy.
	if bool(row.get("lights_terrain", true)) \
			or bool(row.get("lights_objects", true)):
		mask |= Water.VISUAL_LAYER_WORLD | Water.VISUAL_LAYER_WORLD_NO_MIRROR
	if bool(row.get("lights_objects", true)):
		mask |= Water.VISUAL_LAYER_VIEWMODEL
	return mask


func _release_native_light(handle: int) -> void:
	var light := _native_nodes.get(handle) as Light3D
	_native_nodes.erase(handle)
	if light == null or not is_instance_valid(light):
		return
	light.visible = false
	light.set_meta("effect_light_handle", 0)
	if light is SpotLight3D:
		_free_spot.append(light as SpotLight3D)
	else:
		_free_omni.append(light as OmniLight3D)


func _clear_native_lights() -> void:
	for handle_value in _native_nodes.keys():
		_release_native_light(int(handle_value))


## The corona device leg: fetch this frame's additive quads from the portable
## walk and rebuild the MultiMesh (instance origin = segment center, uniform
## scale = half-size, instance color = the premultiplied additive color).
## The models/owners arrays are the per-model pass's own walk — models with
## an occlusion section-mask verdict gate their owned coronas on the
## visible-section bit [orig: Terrain_IsBuildingSectionBitSet @ 0x5c6960];
## the env fog rides in as the fog-to-black fold
## [orig: CD3DDevice_SetFogAndBlendMode(dev, 2) @ 0x5aafb6].
func _render_coronas(camera: Camera3D, gain: Vector3, weather: Node,
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
	# The quads billboard in-shader; keep them from being frustum-culled by
	# their degenerate static AABB.
	mmi.custom_aabb = AABB(Vector3(-512, -512, -512), Vector3(1024, 1024, 1024))
	_world.add_child(mmi)
	_corona_instance = mmi
	return mmi


## The procedural corona texture [orig: Lighting_InitTextures @ 0x5a94f0 —
## "texlightcrn": 128x128, intensity = 255 x (0.4 - 0.45 x d) clamped >= 0,
## d = sqrt(((x-64)/64)^2 + ((y-64)/64)^2), border texels forced 0].
static var _corona_texture_cache: ImageTexture


static func _corona_texture() -> ImageTexture:
	if _corona_texture_cache != null:
		return _corona_texture_cache
	var image := Image.create(128, 128, false, Image.FORMAT_RGB8)
	for y in range(128):
		for x in range(128):
			var value := 0.0
			if x != 0 and x != 127 and y != 0 and y != 127:
				var dx := absf(x - 64.0) / 64.0
				var dy := absf(y - 64.0) / 64.0
				value = maxf(0.4 - 0.45 * sqrt(dx * dx + dy * dy), 0.0)
			image.set_pixel(x, y, Color(value, value, value))
	_corona_texture_cache = ImageTexture.create_from_image(image)
	return _corona_texture_cache


## The 62 Hz lifecycle decay [orig: EffectWorld_TickInstancesAndLightScale
## @ 0x5aa170 from the main loop] — beside EffectWorld.advance_fixed_tick.
func advance_fixed_tick() -> void:
	_scene.advance_fixed_tick()


## One weapon fire with the ammo MF_Light flag [orig: Entity_UpdateMuzzleGlow-
## Effect @ 0x56c960, called per shot from both fire arms]. Owner = the
## shooter. The metadata is preserved for diagnostics/coronas; the native
## light spatially illuminates nearby receivers, including the viewmodel.
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
