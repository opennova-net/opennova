extends RefCounted

# WorldContextPreview: mounts a workspace's subject into the real world — the
# same environment / sky / weather / water furniture the game renders with
# (docs/oned/editor-runtime-parity.md, "direct runtime-node reuse") — over a
# caller-provided world root, plus the grounding sampler seam for putting
# things on the live surface. Extracted verbatim from TerrainEditor (maturity
# slice F1); TerrainEditor is the first host, the Object place-on-terrain and
# Sound at-distance previews are the next consumers.
#
# Host-specific reads stay behind seam Callables captured at construction
# (the parity doc's sampler-seam pattern, A8's captured-lambda rule): the
# surface material, the document's water height, the live-surface height
# sampler, and the host's UI fan-out. The service never reaches back into
# its host.
#
# Node names are contract — "EditorEnvironment", "EditorSky", "EditorWeather",
# "WaterPlane": sky/weather/water resolve the environment through the relative
# "../EditorEnvironment" path, and tools/tests address the nodes by name.
#
# Reference via preload(), not class_name, so it resolves without an editor
# re-import (same convention as mission_object_placer.gd).

const NovaEnvironmentScript = preload("res://engine/environment/nova_environment.gd")
const NovaSkyScript = preload("res://engine/environment/nova_sky.gd")
const NovaWaterScript = preload("res://engine/environment/nova_water.gd")
const NovaWeatherScript = preload("res://engine/environment/nova_weather.gd")

# The bound app-owned environment DOCUMENT (EnvironmentEditor); null until
# bind_environment_editor. The host keeps its own handle for shell consumers.
var environment_editor

var _world_root: Node3D
var _environment_node: Node
var _sky_node: Node3D
var _weather_node: Node3D
var _water_node: Node3D

# func() -> ShaderMaterial: the host's live surface material receiving the
# env terrain uniforms (null when no surface is live).
var _get_material: Callable
# func() -> float: the host document's authored water height (world units).
var _get_water_height: Callable
# func(world_x: float, world_z: float) -> float: live-surface height under a
# world-space point; NAN when no surface is live or the point is off it.
var _sample_height: Callable
# func() -> void: the host's UI fan-out after environment state changes.
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

	_sky_node = Node3D.new()
	_sky_node.name = "EditorSky"
	_sky_node.set_script(NovaSkyScript)
	_sky_node.environment_path = NodePath("../EditorEnvironment")
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
		_environment_node.time_of_day = preview_time
	# A discrete TOD scrub or document edit must snap the weather smoother,
	# otherwise the preview lags behind the slider.
	if _weather_node and _weather_node.has_method("resync_colors"):
		_weather_node.resync_colors()
	apply_environment_to_preview()
	_on_state_changed.call()


func apply_environment_to_preview() -> void:
	if _environment_node == null or not _environment_node.has_method("is_loaded") or not _environment_node.is_loaded():
		return
	var material: ShaderMaterial = _get_material.call()
	if material:
		# Same env -> terrain-uniform push the runtime uses (NovaEnvironment owns it).
		_environment_node.apply_terrain_uniforms(material)
	# Water color/height/murk now come from the NovaWater node (env-driven),
	# matching the runtime; nothing hardcoded here.


func get_environment_node() -> Node:
	return _environment_node


func get_sky_node() -> Node3D:
	return _sky_node


func get_weather_node() -> Node3D:
	return _weather_node


func get_water_node() -> Node3D:
	return _water_node


# --- Grounding ----------------------------------------------------------------

# Live-surface height under (world_x, world_z) through the host's sampler
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


# Teardown: unbind the document and free the furniture. Hosts whose world root
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
	for node in [_water_node, _weather_node, _sky_node, _environment_node]:
		if is_instance_valid(node):
			node.free()
	_water_node = null
	_weather_node = null
	_sky_node = null
	_environment_node = null
