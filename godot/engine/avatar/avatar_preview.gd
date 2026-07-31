class_name AvatarPreview
extends Control

const MissionRuntime := preload("res://engine/world/mission_runtime.gd")


# 3D preview for an Avatars.def character combo: composes the resolved third-person
# head + body .3di models into one scene under a shared environment, framed as a
# standing character by a fly camera with the editor grid + axis gizmo. Retail combo
# arms graphics use a larger rig than this preview skeleton and must never be overlaid.
# Forked from
# object_preview.gd — it reuses the same SubViewport scaffold, guide gizmos, and
# bounds framing.
#
# Each composed third-person part shares one skeletal idle (Dt1rst.bad rest + PI_Idle.BAD clip),
# matching the original PLAYER_INFO preview [orig: PlayerInfo_InitPreviewModel @ 0x5600d0].
# When those .bad assets aren't resolvable (e.g. a loose ONED mount that lacks them) the parts
# render static at rest — a valid degraded state. The remaining unwitnessed piece of
# D-PLAYERINFO-1 is the in-world (spawned-player) combo binding, not this preview idle.

const FlyCameraScript = preload("res://engine/fly_camera.gd")
const NovaObjectModelScript = preload("res://engine/object/nova_object_model.gd")
const NovaEnvironmentScript = preload("res://engine/environment/nova_environment.gd")

# The standing character uses only the compatible third-person slots. `resolve_combo()`
# also returns `arms`, but retail arm graphics reference bones outside this preview rig.
const THIRD_PERSON_SLOTS := ["head", "body"]
# The old 1.5x object-preview distance cropped head and feet once the malformed
# first-person arms stopped inflating the bounds. Leave a full-character margin.
const EDITOR_DISTANCE_SCALE := 2.7

# Menu-preview tuning (set_menu_preview): a front portrait of a standing soldier for
# the player.mnu PLAYER_PREVIEW pane. Camera yaw 0 puts the orbit camera on +Z and the
# .3di parts import +Z-forward (see MissionObjectPlacer.bms_to_godot_basis), so the
# character faces the viewer with no model rotation. The distance scale fits a ~1.8 m
# figure in the tall pane; the slight downward pitch reads like a person standing just
# below eye level. The pose is framed once and held stable across selections (no jump).
const MENU_DISTANCE_SCALE := 2.7
const MENU_PITCH := -0.06

# The menu's anamorphic design space (nova_menu_shell.gd scales the whole menu tree from this
# to the window). In menu mode the SubViewport is rendered at the on-screen pixel size
# (design size x this scale) so the menu's upscale no longer blurs a low-res texture.
const MENU_DESIGN_SIZE := Vector2(800.0, 600.0)

# Preview animation [orig: update_player_preview_animation @ 0x55dba0]: a damped zoom on
# hover plus a continuous idle rotation that gains a gentle sinusoidal sway on hover. BAM
# angles map 2^32 = 360 deg; the original ticks at ~62.5 Hz.
const MENU_ZOOM_DAMP := 0.05               # blend += (target-blend)*0.05 per tick (orig 0.95/0.05)
const MENU_ZOOM_IN := 0.78                 # camera distance scale at full hover (closer)
const MENU_IDLE_SPEED_DEG_PER_SEC := 43.9  # 0x800000 BAM/frame x 62.5 Hz
const MENU_SWAY_FREQ_RAD_PER_SEC := 0.8    # sin(GetTickCount * 0.0008/ms)
const MENU_SWAY_AMP_DEG := 22.5            # 2^28 BAM amplitude
const MENU_TICK_HZ := 1.0 / MissionRuntime.TICK_DT  # the original's menu update cadence = the engine tick rate

# Skeletal idle for the composed character [orig: PlayerInfo_InitPreviewModel @ 0x5600d0]:
# the original binds the rest skeleton Dt1rst.bad + the looping idle clip PI_Idle.BAD (raw
# .bad files, no .adm) and plays the idle on the skinned parts. Registered under the canonical
# idle key so play_body_clip / slot_to_key resolve it.
const PREVIEW_SKELETON_BAD := "Dt1rst.bad"
const PREVIEW_IDLE_BAD := "PI_Idle.BAD"
const PREVIEW_IDLE_KEY := "anim_idle"

var _resource_root  # NovaResourceRoot, or null (headless / no shell)

var _viewport_container: SubViewportContainer
var _status_label: Label
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
# Static menu mode (runtime PLAYER_INFO): grid/axes hidden, camera locked, a fixed
# front-facing pose framed once and held across combo changes. Off by default so the
# ONED Avatars workspace keeps its interactive fly camera + grid.
var _menu_preview := false
var _menu_pose_set := false
# Menu-portrait animation state (see _process). The part models hang off a spin node so the
# model rotates without moving the camera or grid; the camera only zooms.
var _model_root: Node3D
var _hovered := false
var _zoom_blend := 0.0      # 0 at rest, damped toward 1 while hovered
var _idle_angle := 0.0      # continuous idle rotation (radians)
var _anim_time := 0.0       # seconds, drives the hover sway
var _menu_center := Vector3.ZERO
var _menu_radius := 1.0
var _menu_framed := false

# Loaded part models keyed by slot ("head"/"body"/"arms") -> NovaObjectModel.
var _part_models: Dictionary = {}
# Shared skeletal idle (Dt1rst.bad + PI_Idle.BAD), built lazily once and bound onto every
# skinned part so the composed character plays the idle. Null when the .bad assets aren't
# resolvable; _skeletal_tried gates the one-time build so a missing-asset mount reads once.
var _skeletal  # NovaSkeletalAnim or null
var _skeletal_tried := false
# Camo tint requested per combo, applied to all part models (see apply_camo).
var _camo := Vector3.ONE
var _missing_parts := PackedStringArray()


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_STOP
	clip_contents = true
	_build_viewport()


func set_resource_root(root) -> void:
	_resource_root = root
	# Re-evaluate the skeletal idle against the new mount (a remount may add/remove the .bad set).
	_skeletal = null
	_skeletal_tried = false


func get_resource_root():
	return _resource_root


# Switch to the static menu portrait used by the runtime PLAYER_INFO screen: hide the
# grid + axis gizmo, lock the camera (no orbit / pan / fly), stop the SubViewport from
# eating clicks so the PLAYER_PREVIEW button keeps them, and frame a fixed front pose
# that stays put across combo selections. Idempotent; safe to call before or after the
# first combo loads (the framing applies on the next non-empty load_combo()).
func set_menu_preview(enabled: bool) -> void:
	_menu_preview = enabled
	if not enabled:
		return
	set_grid_visible(false)
	set_axes_visible(false)
	if _camera != null and _camera.has_method("set_gameplay_locked"):
		_camera.set_gameplay_locked(true)
	if _viewport_container != null:
		_viewport_container.mouse_filter = Control.MOUSE_FILTER_IGNORE
	if _viewport != null:
		_viewport.gui_disable_input = true
		_viewport.msaa_3d = Viewport.MSAA_4X  # edges stay clean at the higher render res
	# Render the 3D at true on-screen resolution. This subtree is scaled anamorphically by
	# the menu (nova_menu_shell.gd::_recompute_fit), so a design-size SubViewport gets upscaled
	# and blurred; sizing it to on-screen px keeps it sharp and undistorted. Recompute on
	# window resize too. No-op in the ONED workspace (no parent scale, s == 1).
	if is_inside_tree() and get_viewport() != null \
			and not get_viewport().size_changed.is_connected(_apply_menu_viewport_resolution):
		get_viewport().size_changed.connect(_apply_menu_viewport_resolution)
	_apply_menu_viewport_resolution()
	# Re-pose now if a combo is already loaded; otherwise the next load_combo() frames it.
	_menu_pose_set = false
	_refresh_preview_guides()


# Size the SubViewport to the true on-screen pixel footprint of this pane. The menu scales
# this control by s = window / 800x600; counter-scaling the container by 1/s while sizing it
# to base*s makes SubViewportContainer.stretch render the viewport at base*s (on-screen px)
# yet still visually fill the design-space rect. Guarded so a zero/!inside-tree size is a
# no-op; only runs in menu mode.
func _apply_menu_viewport_resolution() -> void:
	if not _menu_preview or _viewport_container == null or not is_inside_tree():
		return
	var vp := get_viewport()
	if vp == null:
		return
	var win := vp.get_visible_rect().size
	var base := size  # design-space size (FULL_RECT inside PLAYER_PREVIEW)
	if base.x < 1.0 or base.y < 1.0 or win.x < 1.0 or win.y < 1.0:
		return
	var s := Vector2(win.x / MENU_DESIGN_SIZE.x, win.y / MENU_DESIGN_SIZE.y)
	_viewport_container.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_viewport_container.position = Vector2.ZERO
	_viewport_container.size = base * s
	_viewport_container.scale = Vector2(1.0 / s.x, 1.0 / s.y)


func _notification(what: int) -> void:
	# The pane's design-space size is fixed, so its own RESIZED fires only when layout first
	# assigns it -- the moment to (re)apply the on-screen render resolution in menu mode.
	if what == NOTIFICATION_RESIZED and _menu_preview:
		_apply_menu_viewport_resolution()


func _build_viewport() -> void:
	_viewport_container = SubViewportContainer.new()
	_viewport_container.set_anchors_preset(Control.PRESET_FULL_RECT)
	_viewport_container.stretch = true
	_viewport_container.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(_viewport_container)

	_status_label = Label.new()
	_status_label.name = "AvatarPreviewStatus"
	_status_label.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_status_label.position = Vector2(12, 10)
	_status_label.custom_minimum_size = Vector2(320, 0)
	_status_label.theme_type_variation = &"Muted"
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status_label.visible = false
	add_child(_status_label)

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

	# Part models hang off this spin node so the menu portrait can rotate the model while
	# the camera and grid stay put. Identity (no rotation) in the ONED workspace.
	_model_root = Node3D.new()
	_model_root.name = "AvatarModelRoot"
	_root.add_child(_model_root)

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

# Compose the resolved combo's third-person head/body parts into the scene. `combo` is a
# resolve_combo() Dictionary: each present slot carries a part sub-Dictionary with
# a `graphic` basename. A missing/unknown graphic simply skips that slot (no error).
# Re-frames the camera on the composed bounds.
func load_combo(combo: Dictionary) -> void:
	clear()
	_missing_parts = PackedStringArray()
	for slot in THIRD_PERSON_SLOTS:
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
	_refresh_status_label()
	_refresh_preview_guides()


# Build the shared skeletal idle once from the two raw .bad files the original binds
# [orig: PlayerInfo_InitPreviewModel @ 0x5600d0 BoneFile_Load + AnimChannel_InitFromData].
# Returns null (parts stay static) when there's no resource root, the .bad assets aren't
# present (a loose mount that lacks them), the native raw-.bad method is missing (stale DLL),
# or the load fails — all non-fatal degraded states. Cached; _skeletal_tried reads once.
func _ensure_preview_skeletal():
	if _skeletal_tried:
		return _skeletal
	_skeletal_tried = true
	if _resource_root == null:
		return null
	if not _resource_root.has_file(PREVIEW_SKELETON_BAD) or not _resource_root.has_file(PREVIEW_IDLE_BAD):
		return null
	var sk := NovaSkeletalAnim.new()
	if not sk.load_from_bad_files(_resource_root, PREVIEW_SKELETON_BAD, {PREVIEW_IDLE_KEY: PREVIEW_IDLE_BAD}):
		return null
	_skeletal = sk
	return _skeletal


# Load one part .3di by basename into a sibling NovaObjectModel under the root.
# No resource root, an empty name, or a load failure leaves the slot empty.
func _load_part(slot: String, graphic: String) -> void:
	if graphic.is_empty():
		_missing_parts.append("%s: empty graphic" % slot)
		return
	if _resource_root == null:
		_missing_parts.append("%s: no resource root" % slot)
		return
	var data := NovaObjectData.new()
	if data.open_from_resource_root(_resource_root, graphic) != OK:
		_missing_parts.append("%s: %s not found" % [slot, graphic])
		return
	var model = NovaObjectModelScript.new()
	model.name = "AvatarPart_%s" % slot
	_model_root.add_child(model)
	model.set_environment_node(_environment)
	model.set_object_data(data)
	model.set_active_lod(0)  # always the finest LOD in the portrait (defensive; 0 is the default)
	# Bind the shared skeletal idle so the skinned part plays PI_Idle.BAD on the Dt1rst skeleton,
	# like the original PLAYER_INFO preview [orig: PlayerInfo_InitPreviewModel @ 0x5600d0 ->
	# BoneFile_Load + AnimChannel_InitFromData("PI_Idle.BAD")]. Only skinned parts pose; an absent
	# .bad set or a static part leaves the slot at rest. The transform animation (idle spin +
	# hover zoom/sway) is driven separately in _process.
	var sk = _ensure_preview_skeletal()
	if sk != null and data.is_skinned(0):
		model.set_skeletal_anim(sk)
		model.play_body_clip(PREVIEW_IDLE_KEY)
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


func get_part_model(slot: String):
	return _part_models.get(slot, null)


func get_part_model_count() -> int:
	return _part_models.size()


func clear() -> void:
	for model in _part_models.values():
		if model != null and is_instance_valid(model):
			_model_root.remove_child(model)
			model.queue_free()
	_part_models.clear()
	_camo = Vector3.ONE
	_has_framed = false
	_missing_parts = PackedStringArray()
	_refresh_status_label()


func _refresh_status_label() -> void:
	if _status_label == null or not is_instance_valid(_status_label):
		return
	if _missing_parts.is_empty():
		_status_label.visible = false
		return
	_status_label.text = "Missing: %s" % "; ".join(_missing_parts)
	_status_label.visible = true


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
	var empty := bounds.size == Vector3.ZERO
	if empty:
		bounds = AABB(Vector3(-1.0, 0.0, -1.0), Vector3(2.0, 2.0, 2.0))
	_refresh_grid(bounds)
	if _menu_preview:
		# Fixed front portrait: frame once on the first real character, then hold the
		# pose so the camera never jumps as the player cycles combos. Skip the empty
		# fallback so the pose locks to an actual soldier's bounds.
		if not _menu_pose_set and not empty:
			_frame_menu_pose(bounds)
			_menu_pose_set = true
	else:
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
		_camera.call("frame_bounds_custom", center, radius, EDITOR_DISTANCE_SCALE,
				maxf(radius * 8.0, 6.0), 2.8, -0.18)
		_has_framed = true


# Front-facing menu portrait: yaw 0 sits the camera on +Z looking down -Z, and the .3di
# parts import +Z-forward, so the character faces the viewer. Framed once and held — the
# caller gates re-entry on _menu_pose_set so selections never move the camera.
func _frame_menu_pose(bounds: AABB) -> void:
	if _camera == null:
		return
	_menu_center = bounds.get_center()
	_menu_radius = maxf(bounds.size.length() * 0.5, 1.0)
	_camera.near = clampf(_menu_radius * 0.001, 0.02, 5.0)
	_camera.far = maxf(_menu_radius * 12.0, 50.0)
	# Random initial yaw [orig: 0x5600d0 dword_25DC53C = (rand()%180)*0xB60B60 — 0-179 deg in
	# BAM]; the continuous idle spin in _process accumulates from this starting facing.
	_idle_angle = deg_to_rad(float(randi() % 180))
	_menu_framed = true
	_apply_menu_camera()  # initial pose; _process re-poses each frame with the zoom blend


# Position the menu camera front-on at the current zoom. The model's rotation lives on
# _model_root (see _process), so the camera stays put and only its distance changes.
func _apply_menu_camera() -> void:
	if _camera == null or not _menu_framed or not _camera.has_method("frame_bounds_custom"):
		return
	var dist_scale: float = MENU_DISTANCE_SCALE * lerpf(1.0, MENU_ZOOM_IN, _zoom_blend)
	_camera.call("frame_bounds_custom", _menu_center, _menu_radius, dist_scale,
		maxf(_menu_radius * 8.0, 6.0), 0.0, MENU_PITCH)


# Hover toggles the zoom + sway, matching the original's "active when over the preview/lists"
# test [orig: update_player_preview_animation @ 0x55dba0].
func set_hovered(value: bool) -> void:
	_hovered = value


# Per-frame menu portrait animation [orig: update_player_preview_animation @ 0x55dba0]:
# a damped zoom toward the hover target, a continuous idle rotation, and a sinusoidal sway
# that fades in on hover. No-op outside menu mode (the ONED workspace drives its own camera).
func _process(delta: float) -> void:
	if not _menu_preview or not _menu_framed:
		return
	# Hover by mouse-position-over-this-pane, not the PLAYER_PREVIEW widget's mouse_entered
	# signal: the menu's anamorphic scaling + the IGNORE mouse filters (which let the button
	# keep its clicks) stop that signal from firing. This matches the original's cursor-over-
	# widget position test [orig: sub_6467D0 @ 0x6467d0].
	# Only when this pane has a real laid-out rect (true in the live menu; a bare unit-test
	# preview has a zero rect, where an explicit set_hovered() drives the zoom instead).
	if is_inside_tree():
		var r := get_global_rect()
		if r.has_area():
			var hov: bool = r.has_point(get_global_mouse_position())
			if hov != _hovered:
				_hovered = hov
	# Damped zoom toward 1 (hover) / 0 (rest); the per-tick 0.05 factor is made frame-rate
	# robust by scaling against the original's 62.5 Hz cadence.
	var target := 1.0 if _hovered else 0.0
	var t: float = clampf(MENU_ZOOM_DAMP * delta * MENU_TICK_HZ, 0.0, 1.0)
	_zoom_blend = lerpf(_zoom_blend, target, t)
	# Continuous idle spin; on hover a gentle sway fades in over it (scaled by the zoom blend).
	_idle_angle += deg_to_rad(MENU_IDLE_SPEED_DEG_PER_SEC) * delta
	_anim_time += delta
	var sway: float = sin(_anim_time * MENU_SWAY_FREQ_RAD_PER_SEC) * deg_to_rad(MENU_SWAY_AMP_DEG) * _zoom_blend
	if _model_root != null:
		_model_root.rotation.y = _idle_angle + sway
	_apply_menu_camera()
