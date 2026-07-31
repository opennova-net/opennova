extends RefCounted

# WorldContextPreview: mounts a workspace's subject into the real world — the
# same frame-clear / environment / sky / weather / water furniture the game renders with
# (docs/oned/editor-runtime-parity.md, "direct runtime-node reuse") — over a
# caller-provided world root, plus the grounding sampler seam for putting
# things on the live surface. Extracted verbatim from TerrainEditor (maturity
# slice F1); TerrainEditor is the first owner, the Object place-on-terrain and
# Sound at-distance previews are the next consumers.
#
# Owner-specific reads stay behind seam Callables captured at construction
# (the parity doc's sampler-seam pattern, A8's captured-lambda rule): the
# surface material, the document's water height, the live-surface height
# sampler, and the owner's UI fan-out. The service never reaches back into
# its owner.
#
# Node names are contract — "EditorEnvironment", "EditorSky", "EditorWeather",
# "WaterPlane": sky/weather/water resolve the environment through the relative
# "../EditorEnvironment" path, and tools/tests address the nodes by name.
# "EditorClearColor" is the faithful below-rim frame-clear consumer.
#
# Reference via preload(), not class_name, so it resolves without an editor
# re-import (same convention as mission_object_placer.gd).

const NovaEnvironmentScript = preload("res://engine/environment/nova_environment.gd")
const NovaSkyScript = preload("res://engine/environment/nova_sky.gd")
const NovaWaterScript = preload("res://engine/environment/nova_water.gd")
const NovaWeatherScript = preload("res://engine/environment/nova_weather.gd")
const HHMM_DAY := NovaEnvironment.HHMM_DAY

# The bound app-owned environment DOCUMENT (EnvironmentEditor); null until
# bind_environment_editor. The owner keeps its own handle for shell consumers.
var environment_editor

var _world_root: Node3D
var _environment_node: Node
var _clear_color_node: WorldEnvironment
var _sky_node: Node3D
var _weather_node: Node3D
var _water_node: Node3D
var _time_of_day_override_active := false
var _time_of_day_override := 0.0
var _terrain_material_instance_id := 0
var _terrain_env_generation := -1

# func() -> ShaderMaterial: the owner's live surface material receiving the
# env terrain uniforms (null when no surface is live).
var _get_material: Callable
# func() -> float: the owner document's authored water height (world units).
var _get_water_height: Callable
# func(world_x: float, world_z: float) -> float: live-surface height under a
# world-space point; NAN when no surface is live or the point is off it.
var _sample_height: Callable
# func() -> void: the owner's UI fan-out after environment state changes.
var _on_state_changed: Callable


func _init(world_root: Node3D, get_material: Callable, get_water_height: Callable,
		sample_height: Callable, on_state_changed: Callable) -> void:
	_world_root = world_root
	_get_material = get_material
	_get_water_height = get_water_height
	_sample_height = sample_height
	_on_state_changed = on_state_changed


# World-side preview nodes only. The environment DOCUMENT (EnvironmentEditor)
# is app-owned and arrives later via bind_environment_editor. Idempotent:
# re-init never duplicates the furniture.
func init_environment_preview() -> void:
	if _environment_node != null:
		return
	_environment_node = Node.new()
	_environment_node.name = "EditorEnvironment"
	_environment_node.set_script(NovaEnvironmentScript)
	_world_root.add_child(_environment_node)
	_clear_color_node = WorldEnvironment.new()
	_clear_color_node.name = "EditorClearColor"
	var clear_environment := Environment.new()
	clear_environment.background_mode = Environment.BG_COLOR
	clear_environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	_clear_color_node.environment = clear_environment
	_world_root.add_child(_clear_color_node)


	_sky_node = Node3D.new()
	_sky_node.name = "EditorSky"
	_sky_node.set_script(NovaSkyScript)
	_sky_node.environment_path = NodePath("../EditorEnvironment")
	_sky_node.frame_clear_environment = _clear_color_node.environment
	# The weather node owns the cloud-scroll core; created just below —
	# NovaSky resolves the path lazily in _process, so order is safe.
	_sky_node.weather_path = NodePath("../EditorWeather")
	_world_root.add_child(_sky_node)

	# Runtime parity: the same weather smoothing that runs in-game also runs in
	# the preview, so scrubbing/playing TOD matches play. The tick is O(1) so it
	# does not affect brush perf; discrete scrubs call resync_colors() to snap.
	_weather_node = Node3D.new()
	_weather_node.name = "EditorWeather"
	_weather_node.set_script(NovaWeatherScript)
	_weather_node.environment_path = NodePath("../EditorEnvironment")
	_world_root.add_child(_weather_node)


func init_water_plane() -> void:
	if _water_node != null:
		return
	# Runtime parity (deferred from PR #24): the editor preview renders the same
	# NovaWater (water.gdshader, env-derived lit color) the runtime uses, instead
	# of a bespoke plane with a hardcoded color. Height stays document-driven via
	# the override hook.
	_water_node = Node3D.new()
	_water_node.name = "WaterPlane"
	_water_node.set_script(NovaWaterScript)
	_water_node.environment_path = NodePath("../EditorEnvironment")
	_water_node.weather_path = NodePath("../EditorWeather")
	_world_root.add_child(_water_node)
	_water_node.set_height_override(float(_get_water_height.call()))


## Wire the app-owned environment document into the world preview.
func bind_environment_editor(value) -> void:
	environment_editor = value
	if environment_editor == null:
		return
	if not environment_editor.environment_changed.is_connected(_on_environment_editor_changed):
		environment_editor.environment_changed.connect(_on_environment_editor_changed)
	if not environment_editor.state_changed.is_connected(_on_environment_state_changed):
		environment_editor.state_changed.connect(_on_environment_state_changed)
	_on_environment_editor_changed(environment_editor.env_file, environment_editor.time_of_day)


func _on_environment_state_changed() -> void:
	_on_state_changed.call()


func _on_environment_editor_changed(env_file: EnvFile, preview_time: float) -> void:
	if _environment_node:
		_environment_node.environment_data = env_file
		_environment_node.time_of_day = _time_of_day_override if _time_of_day_override_active else preview_time
	# A discrete TOD scrub or document edit must snap the weather smoother,
	# otherwise the preview lags behind the slider.
	if _weather_node and _weather_node.has_method("resync_colors"):
		_weather_node.resync_colors()
	apply_environment_to_preview()
	_on_state_changed.call()


## Render Mission at its BMS start time without mutating the shared ENV
## authoring document. Document changes continue to fan out while this scoped
## override owns only the world preview's clock.
func set_time_of_day_override(value: float) -> void:
	_time_of_day_override_active = true
	_time_of_day_override = fposmod(value, HHMM_DAY)
	_apply_time_of_day_override()


## Restore the latest authoring time when the Mission workspace releases its
## preview context.
func clear_time_of_day_override() -> void:
	if not _time_of_day_override_active:
		return
	_time_of_day_override_active = false
	_apply_time_of_day_override()


func _apply_time_of_day_override() -> void:
	if _environment_node == null:
		return
	if _time_of_day_override_active:
		_environment_node.time_of_day = _time_of_day_override
	elif environment_editor != null:
		_environment_node.time_of_day = float(environment_editor.time_of_day)
	if _weather_node and _weather_node.has_method("resync_colors"):
		_weather_node.resync_colors()
	apply_environment_to_preview()


func apply_environment_to_preview() -> void:
	sync_environment_to_preview(true)
	if _sky_node and _sky_node.has_method("sync_frame_clear_color"):
		_sky_node.call(&"sync_frame_clear_color")
	# Water color/height/murk now come from the NovaWater node (env-driven),
	# matching the runtime; nothing hardcoded here.


## Restamp the live terrain material when weather/TOD moves the environment or
## the owner replaces its surface material. The generation comparison keeps the
## normal per-frame editor poll O(1) after the weather smoother settles.
func sync_environment_to_preview(force: bool = false) -> void:
	if _environment_node == null or not _environment_node.has_method("is_loaded") or not _environment_node.is_loaded():
		return
	var material: ShaderMaterial = _get_material.call()
	var material_id := material.get_instance_id() if material != null else 0
	var generation := int(_environment_node.get_env_generation()) \
		if _environment_node.has_method("get_env_generation") else -1
	if not force and material_id == _terrain_material_instance_id \
			and generation == _terrain_env_generation:
		return
	if material:
		# Same env -> terrain-uniform push the runtime uses (NovaEnvironment owns it).
		_environment_node.apply_terrain_uniforms(material)
	_terrain_material_instance_id = material_id
	_terrain_env_generation = generation


func get_environment_node() -> Node:
	return _environment_node


func get_clear_color_node() -> WorldEnvironment:
	return _clear_color_node


func get_sky_node() -> Node3D:
	return _sky_node


func get_weather_node() -> Node3D:
	return _weather_node


func get_water_node() -> Node3D:
	return _water_node


# --- Grounding ----------------------------------------------------------------

# Live-surface height under (world_x, world_z) through the owner's sampler
# seam; NAN when no surface is live or the point is off it.
func sample_height(world_x: float, world_z: float) -> float:
	return float(_sample_height.call(world_x, world_z))


# Snap a world position onto the live surface. Points the sampler cannot
# ground (NAN) come back unchanged so callers keep their authored height.
func ground_position(world_pos: Vector3) -> Vector3:
	var height := sample_height(world_pos.x, world_pos.z)
	if is_nan(height):
		return world_pos
	return Vector3(world_pos.x, height, world_pos.z)


# Teardown: unbind the document and free the furniture. Owners whose world root
# dies with the scene tree may skip this (the nodes are tree children — the
# pre-extraction TerrainEditor lifecycle); explicit consumers call it. init_*
# after release() rebuilds fresh furniture.
func release() -> void:
	if environment_editor != null and is_instance_valid(environment_editor):
		if environment_editor.environment_changed.is_connected(_on_environment_editor_changed):
			environment_editor.environment_changed.disconnect(_on_environment_editor_changed)
		if environment_editor.state_changed.is_connected(_on_environment_state_changed):
			environment_editor.state_changed.disconnect(_on_environment_state_changed)
	environment_editor = null
	_time_of_day_override_active = false
	_time_of_day_override = 0.0
	_terrain_material_instance_id = 0
	_terrain_env_generation = -1
	for node in [_water_node, _weather_node, _sky_node, _clear_color_node, _environment_node]:
		if is_instance_valid(node):
			node.free()
	_water_node = null
	_weather_node = null
	_sky_node = null
	_environment_node = null
	_clear_color_node = null
