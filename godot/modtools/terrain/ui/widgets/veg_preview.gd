class_name VegPreview
extends Control

## 3D preview of a single foliage graphic, rendered into an internal SubViewport.
## Instantiate programmatically (`VegPreview.new()`), add as a child, then call
## `set_graphic(basename)` or `set_mesh(mesh)`.

const VegAssets := preload("res://engine/terrain/veg_assets.gd")

const _PREVIEW_PIXELS := Vector2i(192, 192)
const _BG_COLOR := Color(0.12, 0.13, 0.15, 1.0)

var _subviewport: SubViewport
var _camera: Camera3D
var _mesh_instance: MeshInstance3D
var _pending_mesh: Mesh = null
var _has_pending: bool = false
var _current_graphic: String = ""
var _resource_root: NovaResourceRoot


func set_resource_root(value: NovaResourceRoot) -> void:
	if _resource_root == value:
		return
	_resource_root = value
	_current_graphic = ""


func _ready() -> void:
	_build_internal_scene()
	if _has_pending:
		_apply_mesh(_pending_mesh)
		_has_pending = false


func _build_internal_scene() -> void:
	var container := SubViewportContainer.new()
	container.stretch = true
	container.anchor_right = 1.0
	container.anchor_bottom = 1.0
	container.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(container)

	_subviewport = SubViewport.new()
	_subviewport.size = _PREVIEW_PIXELS
	_subviewport.transparent_bg = false
	_subviewport.render_target_update_mode = SubViewport.UPDATE_DISABLED
	_subviewport.own_world_3d = true

	var world := World3D.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = _BG_COLOR
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.85, 0.88, 1.0)
	env.ambient_light_energy = 0.9
	world.environment = env
	_subviewport.world_3d = world
	container.add_child(_subviewport)

	_camera = Camera3D.new()
	_camera.current = true
	_subviewport.add_child(_camera)

	var light := DirectionalLight3D.new()
	light.rotation = Vector3(deg_to_rad(-40.0), deg_to_rad(35.0), 0.0)
	light.light_energy = 1.6
	light.light_color = Color(1.0, 0.97, 0.88)
	_subviewport.add_child(light)

	var fill := DirectionalLight3D.new()
	fill.rotation = Vector3(deg_to_rad(20.0), deg_to_rad(-135.0), 0.0)
	fill.light_energy = 0.65
	fill.light_color = Color(0.75, 0.85, 1.0)
	_subviewport.add_child(fill)

	_mesh_instance = MeshInstance3D.new()
	_subviewport.add_child(_mesh_instance)


func set_graphic(graphic: String) -> void:
	var key: String = graphic.get_file().get_basename().to_lower()
	if key == _current_graphic and _mesh_instance != null and _mesh_instance.mesh != null:
		return
	_current_graphic = key
	var mesh: Mesh = null
	if not graphic.is_empty():
		mesh = VegAssets.load_mesh(_resource_root, graphic)
	set_mesh(mesh)


func set_mesh(mesh: Mesh) -> void:
	if _subviewport == null:
		_pending_mesh = mesh
		_has_pending = true
		return
	_apply_mesh(mesh)


func _apply_mesh(mesh: Mesh) -> void:
	_mesh_instance.mesh = mesh
	if mesh == null:
		_subviewport.render_target_update_mode = SubViewport.UPDATE_DISABLED
		return
	var aabb := mesh.get_aabb()
	if aabb.size == Vector3.ZERO:
		aabb.size = Vector3.ONE
	var center := aabb.get_center()
	var radius: float = aabb.size.length() * 0.5
	var distance: float = maxf(radius * 2.4, 0.75)
	var offset_dir := Vector3(0.8, 0.55, 1.0).normalized()
	_camera.position = center + offset_dir * distance
	_camera.look_at(center, Vector3.UP)
	_subviewport.render_target_update_mode = SubViewport.UPDATE_ONCE
