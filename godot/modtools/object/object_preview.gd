class_name ObjectPreview
extends Control

const FlyCameraScript = preload("res://engine/fly_camera.gd")
const NovaObjectModelScript = preload("res://engine/object/nova_object_model.gd")
const NovaEnvironmentScript = preload("res://engine/environment/nova_environment.gd")
const CollisionHull = preload("res://engine/object/collision_hull.gd")
const ObjectUserPointOverlayScript = preload("res://engine/object/object_user_point_overlay.gd")

var object_data: NovaObjectData

var _viewport_container: SubViewportContainer
var _viewport: SubViewport
var _root: Node3D
var _guide_root: Node3D
var _model
var _environment: NovaEnvironment
var _camera: Camera3D
var _grid_material: StandardMaterial3D
var _axis_material: StandardMaterial3D
var _collision_materials: Dictionary = {}
var _user_point_overlay: ObjectUserPointOverlay
var _environment_file: EnvFile
var _environment_time := 1200.0
var _wireframe := false
var _grid_visible := true
var _axes_visible := true
var _collision_visible := false
var _user_points_visible := true
var _has_framed := false

var _material_defs: Dictionary = {}
var _robj_nodes: Dictionary = {}
var _surface_material_indices: PackedInt32Array = PackedInt32Array()
var _surface_materials: Array = []

var _skeletal                   # NovaSkeletalAnim, or null
var _last_anim_error := ""

var _arms_model                 # NovaObjectModel arms overlay, or null
var _arms_data: NovaObjectData
var _current_clip := ""         # active clip key, mirrored onto the arms overlay
var _last_arms_error := ""


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_STOP
	clip_contents = true
	_build_viewport()
	_apply_environment_to_model()
	_sync_model_debug_refs()


func set_object_data(value: NovaObjectData) -> void:
	if object_data == value:
		return
	if object_data != value:
		_has_framed = false
	object_data = value
	if _model != null:
		_model.set_object_data(value)
		_sync_model_debug_refs()
		_refresh_preview_guides()
		_refresh_collision_overlay()
		_refresh_user_point_overlay()


func get_object_model():
	return _model


func set_environment(env_file: EnvFile, time_of_day: float) -> void:
	_environment_file = env_file
	_environment_time = time_of_day
	_apply_environment_to_model()


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
	_environment.name = "ObjectPreviewEnvironment"
	_root.add_child(_environment)

	_guide_root = Node3D.new()
	_guide_root.name = "ObjectPreviewGuides"
	_root.add_child(_guide_root)

	_model = NovaObjectModelScript.new()
	_model.name = "NovaObjectModel"
	_model.set_model_light_preview_enabled(true)
	_root.add_child(_model)
	_model.bounds_changed.connect(_on_model_bounds_changed)
	_model.set_object_data(object_data)

	_user_point_overlay = ObjectUserPointOverlayScript.new()
	_user_point_overlay.name = "ObjectUserPoints"
	_root.add_child(_user_point_overlay)
	_user_point_overlay.set_source_model(_model)
	_user_point_overlay.set_object_data(object_data)
	_user_point_overlay.set_points_visible(_user_points_visible and has_user_points())

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
	set_wireframe(_wireframe)
	_refresh_preview_guides()


func is_playing() -> bool:
	return _model == null or _model.is_playing()


func set_playing(value: bool) -> void:
	if _model != null:
		_model.set_playing(value)
	if _arms_model != null:
		_arms_model.set_playing(value)


func reset_animation_time() -> void:
	if _model != null:
		_model.reset_animation_time()
	if _arms_model != null:
		_arms_model.reset_animation_time()


# --- Skeletal animation preview (.bad/.adm) --------------------------------------
# Load a model's animation set from the mounted resource root (its .adm names the .bad
# clips), bind it to the model (builds the Skeleton3D + Skin when the model is skinned),
# and return the available clip keys. Empty on failure (see get_animation_error()).
#
# The rig ALWAYS comes from a model's bone table — count and hierarchy from the model
# rows, rest positions reconstructed from the model pivots + the reset .bad's bind
# rotations (the corpus-exact export relation; NovaSkeletalAnim); the lossy shipped
# BadBone.position is never read (12 of 43 JO viewmodel rigs ship it zeroed/stale and
# retail renders them all) [orig: BoneAnim_BuildWorldMatrices @0x40c400 walks
# modelDef+56, bounded by modelDef+52]. The skeleton belongs to the .adm's MODEL,
# not necessarily the previewed one (the FP arms ride the gun's table), so resolve the
# .adm basename's .3di and fall back to the open model — the same rule the game's
# viewmodel builder applies (MissionObjectPlacer.build_model_from_graphic).
func load_animation_set(adm_name: String, resource_root) -> PackedStringArray:
	_skeletal = null
	_last_anim_error = ""
	if _model == null:
		_last_anim_error = "No model"
		return PackedStringArray()
	if resource_root == null:
		_last_anim_error = "No resource directory mounted"
		_clear_skeletal_binding()
		return PackedStringArray()
	if adm_name.strip_edges().is_empty():
		_last_anim_error = "Enter a .adm name"
		_clear_skeletal_binding()
		return PackedStringArray()
	var skel_data := _resolve_skeleton_model(adm_name, resource_root)
	var origins: PackedVector3Array = skel_data.get_bone_origins() if skel_data != null else PackedVector3Array()
	var parents: PackedInt32Array = skel_data.get_bone_parents() if skel_data != null else PackedInt32Array()
	var sk := NovaSkeletalAnim.new()
	if not sk.load_from_resource_root(resource_root, adm_name, origins, parents):
		_last_anim_error = sk.get_last_error()
		_clear_skeletal_binding()
		return PackedStringArray()
	_skeletal = sk
	_model.set_skeletal_anim(sk)
	if _arms_model != null:
		_arms_model.set_skeletal_anim(sk)  # the arms overlay rides the same .adm skeleton
	return sk.get_clip_keys()


# The .3di whose bone table defines the rig for `adm_name`: the open model when the
# names match, else the .adm basename's own model from the resource root, else the
# open model (a rig-less .adm preview still binds; the model table just stays its own).
func _resolve_skeleton_model(adm_name: String, resource_root) -> NovaObjectData:
	var adm_base := adm_name.get_file().get_basename()
	var own_base := String(object_data.get_object_name()).get_file().get_basename() if object_data != null else ""
	if object_data != null and adm_base.nocasecmp_to(own_base) == 0:
		return object_data
	var d := NovaObjectData.new()
	if d.open_from_resource_root(resource_root, adm_base + ".3di") == OK:
		return d
	return object_data


# Drop the skeletal binding from the previewed model and the arms overlay.
func _clear_skeletal_binding() -> void:
	if _model != null:
		_model.set_skeletal_anim(null)
	if _arms_model != null:
		_arms_model.set_skeletal_anim(null)


func play_animation(clip_key: String) -> void:
	_current_clip = clip_key
	if _model != null:
		_model.play_body_clip(clip_key)
	if _arms_model != null:
		_arms_model.play_body_clip(clip_key)


func get_animation_error() -> String:
	return _last_anim_error


func has_skeleton() -> bool:
	return _model != null and _model.has_skeleton()


func get_current_clip() -> String:
	return _current_clip


func get_skeletal_anim():
	return _skeletal


## Scrub the body playhead (seconds), fanned out to the arms overlay so both
## pose in lockstep — poses apply immediately even while paused.
func set_animation_playhead(seconds: float) -> void:
	if _model != null:
		_model.set_animation_time(seconds)
	if _arms_model != null:
		_arms_model.set_animation_time(seconds)


func get_animation_playhead() -> float:
	return _model.get_animation_time() if _model != null else 0.0


# --- Arms overlay (first-person view model: skinned arms riding the same .adm skeleton) ---------
# Load a SECOND .3di (e.g. ArmsG.3di) into a sibling model that shares the main model's
# NovaSkeletalAnim, so the arms animate together with the weapon/body. Returns false on failure
# (see get_arms_error()). With no .adm loaded yet the arms render static at rest; load_animation_set()
# rebinds them when an .adm is loaded.
func load_arms(arms_name: String, resource_root) -> bool:
	_last_arms_error = ""
	if resource_root == null:
		_last_arms_error = "No resource directory mounted"
		return false
	if arms_name.strip_edges().is_empty():
		_last_arms_error = "Pick a .3di"
		return false
	var data := NovaObjectData.new()
	var err := data.open_from_resource_root(resource_root, arms_name)
	if err != OK:
		_last_arms_error = "Could not load %s (error %d)" % [arms_name, err]
		return false
	_arms_data = data
	if _arms_model == null:
		_arms_model = NovaObjectModelScript.new()
		_arms_model.name = "NovaArmsModel"
		_arms_model.set_model_light_preview_enabled(true)
		_root.add_child(_arms_model)
		_arms_model.set_environment_node(_environment)
	_arms_model.set_object_data(data)
	_arms_model.set_skeletal_anim(_skeletal)  # share the main model's .adm skeleton (may be null)
	_arms_model.set_playing(_model.is_playing() if _model != null else true)
	if not _current_clip.is_empty():
		_arms_model.play_body_clip(_current_clip)
	return true


func clear_arms() -> void:
	_arms_data = null
	if _arms_model != null:
		_root.remove_child(_arms_model)
		_arms_model.queue_free()
		_arms_model = null


func has_arms() -> bool:
	return _arms_model != null


func get_arms_error() -> String:
	return _last_arms_error


func get_animation_time_ms() -> int:
	return _model.get_animation_time_ms() if _model != null else 0


func set_active_lod(lod_index: int) -> void:
	if _model == null:
		return
	_has_framed = false
	_model.set_active_lod(lod_index)
	_sync_model_debug_refs()
	_refresh_preview_guides()
	_refresh_user_point_overlay()


func get_active_lod() -> int:
	return _model.get_active_lod() if _model != null else 0


func get_editor_camera() -> Camera3D:
	return _camera


func is_wireframe() -> bool:
	return _wireframe


func set_wireframe(value: bool) -> void:
	_wireframe = value
	if _viewport != null:
		_viewport.debug_draw = Viewport.DEBUG_DRAW_WIREFRAME if value else Viewport.DEBUG_DRAW_DISABLED


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


func is_collision_visible() -> bool:
	return _collision_visible


func set_collision_visible(value: bool) -> void:
	_collision_visible = value
	_refresh_collision_overlay()


func has_collision() -> bool:
	return object_data != null and object_data.has_collision()


func is_user_points_visible() -> bool:
	return _user_points_visible and has_user_points()


func set_user_points_visible(value: bool) -> void:
	_user_points_visible = value
	_refresh_user_point_overlay()


func has_user_points() -> bool:
	return object_data != null \
		and object_data.get_user_point_count() > 0


func _refresh_user_point_overlay() -> void:
	if _user_point_overlay == null:
		return
	_user_point_overlay.set_source_model(_model)
	_user_point_overlay.set_object_data(object_data)
	_user_point_overlay.refresh_points()
	_user_point_overlay.set_points_visible(_user_points_visible and has_user_points())


# Rebuild the collision-volume overlay: one ConvexPolygonShape3D per parsed
# collision volume (the exact hulls mission picking will use), drawn via Godot's
# own collision debug wireframe (Shape3D.get_debug_mesh) and colored by collidable
# type. The hulls come back in model-local space (the (y,z,x) collision frame), so
# parenting under _guide_root -- a sibling of the model at the same root transform --
# lands them on the rendered model with no extra offset, which is the whole point of
# validating here first.
func _refresh_collision_overlay() -> void:
	if _guide_root == null:
		return
	for child in _guide_root.get_children():
		if child.name == "ObjectCollision":
			_guide_root.remove_child(child)
			child.free()
	if not _collision_visible or object_data == null:
		return
	var volumes: Array = object_data.get_collision_volumes()
	if volumes.is_empty():
		return
	var root := Node3D.new()
	root.name = "ObjectCollision"
	_guide_root.add_child(root)
	for v in volumes:
		var pts: PackedVector3Array = CollisionHull.hull_points(v)
		if pts.size() < 4:
			continue
		var shape := ConvexPolygonShape3D.new()
		shape.points = pts
		var mi := MeshInstance3D.new()
		mi.mesh = shape.get_debug_mesh()
		mi.material_override = _collision_material_for(int((v as Dictionary).get("type", 0)))
		mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		root.add_child(mi)


# An unshaded, depth-test-off material colored by collidable type (cached per type
# so all volumes of a type share one). Drawn through the model so enclosed volumes
# stay visible.
func _collision_material_for(type: int) -> StandardMaterial3D:
	if _collision_materials.has(type):
		return _collision_materials[type]
	var m := StandardMaterial3D.new()
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.albedo_color = CollisionHull.color_for_type(type)
	m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	m.no_depth_test = true
	_collision_materials[type] = m
	return m


# Apply the current grid/axes visibility to the live guide nodes. The grid is
# rebuilt on bounds/LOD changes, so _add_grid/_add_axis_gizmo also seed visibility
# at creation; this just updates whatever is mounted right now.
func _apply_guide_visibility() -> void:
	if _guide_root == null:
		return
	for child in _guide_root.get_children():
		if child.name == "ObjectGrid":
			child.visible = _grid_visible
		elif child.name == "ObjectAxisGizmo":
			child.visible = _axes_visible


func set_ctrl_value(name: String, value: int) -> void:
	if _model != null:
		_model.set_ctrl_value(name, value)


func clear_ctrl_value(name: String) -> void:
	if _model != null:
		_model.clear_ctrl_value(name)


func clear_ctrl_values() -> void:
	if _model != null:
		_model.clear_ctrl_values()


func get_ctrl_values() -> Dictionary:
	return _model.get_ctrl_values() if _model != null else {}


func _apply_environment_to_model() -> void:
	if _environment != null:
		_environment.environment_data = _environment_file
		_environment.time_of_day = _environment_time
	if _model != null:
		_model.set_environment_node(_environment)


func _on_model_bounds_changed(bounds: AABB) -> void:
	_sync_model_debug_refs()
	call_deferred("_refresh_user_point_overlay")
	call_deferred("_refresh_bounds_guides", bounds)


func _refresh_bounds_guides(bounds: AABB) -> void:
	_refresh_grid(bounds)
	_frame_bounds(bounds)


func _sync_model_debug_refs() -> void:
	if _model == null:
		_material_defs.clear()
		_robj_nodes.clear()
		_surface_material_indices.clear()
		_surface_materials.clear()
		return
	_material_defs = _model.get_material_defs()
	_robj_nodes = _model.get_render_part_nodes()
	_surface_material_indices = _model.get_surface_material_indices()
	_surface_materials = _model.get_surface_materials()


func _compute_transformed_mesh_bounds() -> AABB:
	return _model.get_model_bounds() if _model != null else AABB()


func _refresh_preview_guides() -> void:
	var bounds := _compute_transformed_mesh_bounds()
	if bounds.size == Vector3.ZERO:
		bounds = AABB(Vector3(-2.0, 0.0, -2.0), Vector3(4.0, 1.0, 4.0))
	_refresh_grid(bounds)
	_frame_bounds(bounds)


func _refresh_grid(bounds: AABB) -> void:
	if _guide_root == null:
		return
	for child in _guide_root.get_children():
		if child.name == "ObjectGrid":
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
	grid.name = "ObjectGrid"
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
	axis.name = "ObjectAxisGizmo"
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
