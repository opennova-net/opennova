@tool
class_name NovaCelestial
extends Node3D

# Renders the sun/moon/star 3DI bodies named in the .env, attached to the sky.
# Engine equivalents (docs/env/env-tod-re.md):
# - [orig: EffectWorld_LoadCelestialModels @ 0x5adc50] resolves sun_3di/moon_3di/
#   star_3di/glare_3di to models (glare/star under additive mode 0x300000).
# - [orig: render_skybox_layers @ 0x5ac230] places them at camera + dir*2000 with
#   no depth test, drawn after the dome and before the world.
# The 3DI diffuse stays; bodies are tinted and dimmed by the TOD sun/moon color.

const NovaObjectModelScript = preload("res://engine/object/nova_object_model.gd")
const CelestialShader = preload("res://shaders/celestial.gdshader")

# Render-priority ladder so the transparent sky pass composites dome < bodies <
# glare regardless of distance (all unshaded + depth disabled).
const PRIORITY_SUN := 1
const PRIORITY_MOON := 1
const PRIORITY_STAR := 0
const PRIORITY_GLARE := 2

@export var environment_path: NodePath

var _resource_root: NovaResourceRoot
var _cached_env: Node = null
var _cached_cam: Camera3D = null
var _bodies: Dictionary = {} # name -> { model, material, additive, priority, tint_key }
var _loaded_names: Dictionary = {}


func _ready() -> void:
	_cached_env = get_node_or_null(environment_path) if not environment_path.is_empty() else null
	_rebuild_if_needed()


func set_resource_root(value: NovaResourceRoot) -> void:
	_resource_root = value
	_loaded_names.clear()
	_rebuild_if_needed()


func _env_data() -> EnvFile:
	if _cached_env and _cached_env.has_method("get_environment_data"):
		return _cached_env.get_environment_data()
	return null


func _rebuild_if_needed() -> void:
	var env_data := _env_data()
	if env_data == null or _resource_root == null:
		return
	var wanted := {
		"sun": { "name": env_data.get_sun_3di(), "additive": false, "priority": PRIORITY_SUN, "tint": "sun" },
		"moon": { "name": env_data.get_moon_3di(), "additive": false, "priority": PRIORITY_MOON, "tint": "moon" },
		"star": { "name": env_data.get_star_3di(), "additive": true, "priority": PRIORITY_STAR, "tint": "sky" },
	}
	# Rebuild only when the set of names actually changed (undo/scrub safe).
	var signature := {}
	for key in wanted:
		signature[key] = wanted[key]["name"]
	if signature == _loaded_names:
		return
	_loaded_names = signature

	for child in get_children():
		remove_child(child)
		child.queue_free()
	_bodies.clear()

	for key in wanted:
		var spec: Dictionary = wanted[key]
		var model_name: String = spec["name"]
		if model_name.strip_edges().is_empty():
			continue
		var data := _load_object_data(model_name)
		if data == null:
			continue
		var model: Node3D = NovaObjectModelScript.new()
		model.name = "Celestial_" + key
		add_child(model)
		model.set_object_data(data)
		var material := _make_celestial_material(spec["additive"], spec["priority"])
		_apply_material_override(model, material)
		_bodies[key] = {
			"model": model,
			"material": material,
			"tint": spec["tint"],
		}


func _load_object_data(graphic: String) -> NovaObjectData:
	var model_name := graphic
	if model_name.get_extension().is_empty():
		model_name += ".3di"
	var data := NovaObjectData.new()
	if data.open_from_resource_root(_resource_root, model_name) == OK:
		return data
	return null


func _make_celestial_material(additive: bool, priority: int) -> ShaderMaterial:
	var material := ShaderMaterial.new()
	material.shader = CelestialShader
	material.render_priority = priority
	material.set_shader_parameter("u_additive", additive)
	if additive:
		material.set_shader_parameter("u_opacity", float(0x2000) / 65536.0)
	return material


# Walk the model's MeshInstance3D parts, reuse each surface's diffuse texture in
# our celestial material, and override the part's material.
func _apply_material_override(model: Node3D, base_material: ShaderMaterial) -> void:
	var meshes: Array[MeshInstance3D] = []
	_collect_meshes(model, meshes)
	for mesh_instance in meshes:
		var surface_count := mesh_instance.mesh.get_surface_count() if mesh_instance.mesh else 0
		for surface in surface_count:
			var src := mesh_instance.get_active_material(surface)
			var material := base_material.duplicate() as ShaderMaterial
			if src is ShaderMaterial:
				var diffuse = (src as ShaderMaterial).get_shader_parameter("u_diffuse")
				if diffuse:
					material.set_shader_parameter("u_diffuse", diffuse)
			mesh_instance.set_surface_override_material(surface, material)


static func _collect_meshes(node: Node, out: Array[MeshInstance3D]) -> void:
	if node is MeshInstance3D:
		out.append(node)
	for child in node.get_children():
		_collect_meshes(child, out)


func _process(_delta: float) -> void:
	if _bodies.is_empty():
		_rebuild_if_needed()
		if _bodies.is_empty():
			return
	var env := _cached_env
	if env == null or not env.has_method("is_loaded") or not env.is_loaded():
		return

	if not _cached_cam or not _cached_cam.is_inside_tree():
		_cached_cam = _find_camera()
	var cam_pos := _cached_cam.global_position if _cached_cam else Vector3.ZERO

	var sun_dir: Vector3 = env.get_sun_direction()
	var moon_dir: Vector3 = env.get_moon_direction()
	var height_scale := 1.0
	var env_data := _env_data()
	if env_data:
		height_scale = max(0.1, env_data.get_sky_height() / 175.69)

	for key in _bodies:
		var body: Dictionary = _bodies[key]
		var model: Node3D = body["model"]
		var dir := moon_dir if key == "moon" else sun_dir
		var dist := EnvRenderConstants.DOME_DISTANCE * height_scale
		model.global_position = Vector3(cam_pos.x, 0.0, cam_pos.z) + dir * dist
		var tint: Vector3 = _tint_for(env, body["tint"])
		body["material"].set_shader_parameter("u_tint", tint)
		# Hide the sun below the horizon and the moon above it.
		if key == "sun":
			model.visible = sun_dir.y > -0.1
		elif key == "moon":
			model.visible = moon_dir.y > -0.1


func _tint_for(env: Node, tint_key: String) -> Vector3:
	match tint_key:
		"sun":
			return env.get_sun_color()
		"moon":
			return env.get_moon_color()
		_:
			return env.get_sky_ambient()


func _find_camera() -> Camera3D:
	if Engine.is_editor_hint():
		var editor_interface = Engine.get_singleton("EditorInterface")
		if editor_interface:
			var viewport = editor_interface.get_editor_viewport_3d(0)
			if viewport:
				return viewport.get_camera_3d()
	var viewport := get_viewport()
	return viewport.get_camera_3d() if viewport else null


# Mirror of the env_render celestial placement constants for GDScript callers.
class EnvRenderConstants:
	const DOME_DISTANCE := 2000.0
