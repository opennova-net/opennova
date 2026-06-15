class_name AvatarPreview
extends Control

# 3D preview for an Avatars.def character combo: composes the resolved head /
# body / arms part .3di models into one scene under a shared environment, framed
# by a fly camera with the editor grid + axis gizmo. Forked from
# object_preview.gd — it reuses the same SubViewport scaffold, guide gizmos, and
# bounds framing, and the arms-overlay idea (a sibling NovaObjectModel sharing one
# NovaSkeletalAnim) generalized to three part slots.
#
# A combo's parts are static at rest here (no .adm is bound): the original
# combo -> spawned-player model binding is unwitnessed (docs/playerinfo/avatars-re.md
# D-PLAYERINFO-1), so the preview stops at the resolved part geometry behind that seam.

const FlyCameraScript = preload("res://engine/fly_camera.gd")
const NovaObjectModelScript = preload("res://engine/object/nova_object_model.gd")
const NovaEnvironmentScript = preload("res://engine/environment/nova_environment.gd")

# Part slot keys, matching resolve_combo()'s head/body/arms sub-dictionaries.
const SLOTS := ["head", "body", "arms"]

var _resource_root  # NovaResourceRoot, or null (headless / no shell)

var _viewport_container: SubViewportContainer
var _viewport: SubViewport
var _root: Node3D
var _guide_root: Node3D
var _environment: NovaEnvironment
var _camera: Camera3D
var _grid_material: StandardMaterial3D
var _axis_material: StandardMaterial3D
var _grid_visible := true
var _axes_visible := true
var _has_framed := false

# Loaded part models keyed by slot ("head"/"body"/"arms") -> NovaObjectModel.
var _part_models: Dictionary = {}
# Camo tint requested per combo, applied to all part models (see apply_camo).
var _camo := Vector3.ONE


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_STOP
	clip_contents = true
	_build_viewport()


func set_resource_root(root) -> void:
	_resource_root = root


func get_resource_root():
	return _resource_root


func _build_viewport() -> void:
	_viewport_container = SubViewportContainer.new()
	_viewport_container.set_anchors_preset(Control.PRESET_FULL_RECT)
	_viewport_container.stretch = true
	_viewport_container.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(_viewport_container)

	_viewport = SubViewport.new()
	_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	_viewport.transparent_bg = false
	_viewport.handle_input_locally = true
	_viewport_container.add_child(_viewport)

	_root = Node3D.new()
	_viewport.add_child(_root)

	_environment = NovaEnvironmentScript.new()
	_environment.name = "AvatarPreviewEnvironment"
	_root.add_child(_environment)

	_guide_root = Node3D.new()
	_guide_root.name = "AvatarPreviewGuides"
	_root.add_child(_guide_root)

	_camera = FlyCameraScript.new()
	_camera.current = true
	_camera.fov = 42.0
	_camera.near = 0.02
	_camera.far = 500.0
	_camera.look_at_from_position(Vector3(0.0, 1.5, 6.0), Vector3.ZERO)
	_root.add_child(_camera)

	_grid_material = StandardMaterial3D.new()
	_grid_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_grid_material.vertex_color_use_as_albedo = true
	_grid_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA

	_axis_material = StandardMaterial3D.new()
	_axis_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_axis_material.vertex_color_use_as_albedo = true
	_axis_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA

	_add_axis_gizmo()
	_refresh_preview_guides()


# --- Combo composition --------------------------------------------------------

# Compose the resolved combo's head/body/arms parts into the scene. `combo` is a
# resolve_combo() Dictionary: each present slot carries a part sub-Dictionary with
# a `graphic` basename. A missing/unknown graphic simply skips that slot (no error).
# Re-frames the camera on the composed bounds.
func load_combo(combo: Dictionary) -> void:
	clear()
	for slot in SLOTS:
		var part: Variant = combo.get(slot, null)
		if part == null or not (part is Dictionary):
			continue
		var graphic := String((part as Dictionary).get("graphic", "")).strip_edges()
		_load_part(slot, graphic)
	# Pull the camo from the head part if present (the menu's per-combo tint sits
	# on the head); apply it across all loaded models.
	var head: Variant = combo.get("head", null)
	if head is Dictionary:
		var camo: Array = (head as Dictionary).get("camo", [])
		if camo.size() == 3:
			apply_camo(Vector3(float(camo[0]), float(camo[1]), float(camo[2])) / 255.0)
	_refresh_preview_guides()


# Load one part .3di by basename into a sibling NovaObjectModel under the root.
# No resource root, an empty name, or a load failure leaves the slot empty.
func _load_part(slot: String, graphic: String) -> void:
	if _resource_root == null or graphic.is_empty():
		return
	var data := NovaObjectData.new()
	if data.open_from_resource_root(_resource_root, graphic) != OK:
		return  # missing / unknown graphic: skip this slot silently
	var model = NovaObjectModelScript.new()
	model.name = "AvatarPart_%s" % slot
	_root.add_child(model)
	model.set_environment_node(_environment)
	model.set_object_data(data)
	# No .adm is bound (combo -> skeleton binding unwitnessed, D-PLAYERINFO-1), so
	# the part renders static at rest. If a future seam loads a shared skeleton it
	# would be set here via model.set_skeletal_anim(shared) before framing.
	_part_models[slot] = model


# Push the combo's camo color into each part model. The original menu tints the
# character with the part camo triple; the reimpl's per-material modulation hook
# is u_rgb_mod, but it is driven by the engine's eval_material_runtime (animated
# UV / rgb), not a free editor override — there is no witnessed editor camo path
# yet, so this stores the value and is otherwise a no-op.
# TODO camo tint: wire to a material modulation parameter once the original
# combo-camo application is witnessed (docs/playerinfo/avatars-re.md D-PLAYERINFO-1).
func apply_camo(rgb: Vector3) -> void:
	_camo = rgb


func get_camo() -> Vector3:
	return _camo


func get_part_model(slot: String):
	return _part_models.get(slot, null)


func get_part_model_count() -> int:
	return _part_models.size()


func clear() -> void:
	for model in _part_models.values():
		if model != null and is_instance_valid(model):
			_root.remove_child(model)
			model.queue_free()
	_part_models.clear()
	_camo = Vector3.ONE
	_has_framed = false


func get_editor_camera() -> Camera3D:
	return _camera


# --- View guides (grid + axis gizmo) ------------------------------------------

func is_grid_visible() -> bool:
	return _grid_visible


func set_grid_visible(value: bool) -> void:
	_grid_visible = value
	_apply_guide_visibility()


func is_axes_visible() -> bool:
	return _axes_visible


func set_axes_visible(value: bool) -> void:
	_axes_visible = value
	_apply_guide_visibility()


func _apply_guide_visibility() -> void:
	if _guide_root == null:
		return
	for child in _guide_root.get_children():
		if child.name == "AvatarGrid":
			child.visible = _grid_visible
		elif child.name == "AvatarAxisGizmo":
			child.visible = _axes_visible


func _composed_bounds() -> AABB:
	var bounds := AABB()
	var has_bounds := false
	for model in _part_models.values():
		if model == null or not is_instance_valid(model):
			continue
		var b: AABB = model.get_model_bounds()
		if b.size == Vector3.ZERO:
			continue
		bounds = b if not has_bounds else bounds.merge(b)
		has_bounds = true
	return bounds


func _refresh_preview_guides() -> void:
	var bounds := _composed_bounds()
	if bounds.size == Vector3.ZERO:
		bounds = AABB(Vector3(-1.0, 0.0, -1.0), Vector3(2.0, 2.0, 2.0))
	_refresh_grid(bounds)
	_frame_bounds(bounds)


func _refresh_grid(bounds: AABB) -> void:
	if _guide_root == null:
		return
	for child in _guide_root.get_children():
		if child.name == "AvatarGrid":
			_guide_root.remove_child(child)
			child.free()
	_add_grid(bounds)


func _add_grid(bounds: AABB) -> void:
	var vertices := PackedVector3Array()
	var colors := PackedColorArray()
	var min_x := bounds.position.x
	var max_x := bounds.end.x
	var min_z := bounds.position.z
	var max_z := bounds.end.z
	var half: float = maxf(1.0, maxf(maxf(absf(min_x), absf(max_x)), maxf(absf(min_z), absf(max_z))))
	var step: float = _grid_step_for_extent(half)
	var limit: float = ceilf(half / step + 1.0) * step
	var line_count := int(roundf(limit / step))

	for i in range(-line_count, line_count + 1):
		var p := float(i) * step
		var axis_x := is_zero_approx(p)
		_push_grid_line(vertices, colors, Vector3(p, 0.0, -limit), Vector3(p, 0.0, limit), axis_x)
		_push_grid_line(vertices, colors, Vector3(-limit, 0.0, p), Vector3(limit, 0.0, p), axis_x)

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_COLOR] = colors
	var grid_mesh := ArrayMesh.new()
	grid_mesh.add_surface_from_arrays(Mesh.PRIMITIVE_LINES, arrays)
	var grid := MeshInstance3D.new()
	grid.name = "AvatarGrid"
	grid.mesh = grid_mesh
	grid.material_override = _grid_material
	grid.visible = _grid_visible
	_guide_root.add_child(grid)


func _push_grid_line(vertices: PackedVector3Array, colors: PackedColorArray, a: Vector3, b: Vector3, axis: bool) -> void:
	var color := Color(0.80, 0.86, 0.90, 0.72) if axis else Color(0.38, 0.43, 0.48, 0.34)
	vertices.push_back(a)
	vertices.push_back(b)
	colors.push_back(color)
	colors.push_back(color)


func _grid_step_for_extent(half_extent: float) -> float:
	if half_extent <= 4.0:
		return 0.5
	if half_extent <= 16.0:
		return 1.0
	if half_extent <= 64.0:
		return 4.0
	return 16.0


func _add_axis_gizmo() -> void:
	if _guide_root == null:
		return
	var vertices := PackedVector3Array([
		Vector3.ZERO, Vector3(1.25, 0.0, 0.0),
		Vector3.ZERO, Vector3(0.0, 1.25, 0.0),
		Vector3.ZERO, Vector3(0.0, 0.0, 1.25),
	])
	var colors := PackedColorArray([
		Color(0.95, 0.24, 0.22, 0.95), Color(0.95, 0.24, 0.22, 0.95),
		Color(0.32, 0.86, 0.38, 0.95), Color(0.32, 0.86, 0.38, 0.95),
		Color(0.25, 0.52, 0.95, 0.95), Color(0.25, 0.52, 0.95, 0.95),
	])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_COLOR] = colors
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_LINES, arrays)
	var axis := MeshInstance3D.new()
	axis.name = "AvatarAxisGizmo"
	axis.mesh = mesh
	axis.material_override = _axis_material
	axis.visible = _axes_visible
	_guide_root.add_child(axis)


func _frame_bounds(bounds: AABB) -> void:
	if _camera == null:
		return
	var center := bounds.get_center()
	var radius := bounds.size.length() * 0.5
	if radius < 1.0:
		radius = 1.0
	_camera.near = clampf(radius * 0.001, 0.02, 5.0)
	_camera.far = maxf(radius * 12.0, 50.0)
	_camera.set("fly_speed", clampf(radius * 2.5, 1.0, 250.0))
	_camera.set("zoom_speed", clampf(radius * 0.18, 0.05, 20.0))
	_camera.set("pan_sensitivity", clampf(radius * 0.01, 0.01, 1.0))
	if not _has_framed:
		_camera.call("frame_bounds_custom", center, radius, 1.5, maxf(radius * 8.0, 6.0), 2.8, -0.18)
		_has_framed = true
