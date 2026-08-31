class_name AvatarPreview
extends Control



# Runtime PLAYER_INFO portrait for an Avatars.def character combo. It composes
# the resolved third-person head + body .3di models under a shared environment
# and holds a front-facing camera pose across combo changes. Retail combo arms
# graphics use a larger rig than this portrait skeleton and must never be overlaid.
#
# Each composed third-person part shares one skeletal idle (Dt1rst.bad rest + PI_Idle.BAD clip),
# matching the original PLAYER_INFO preview [orig: PlayerInfo_InitPreviewModel @ 0x5600d0].
# When those .bad assets aren't resolvable (e.g. a loose ONED mount that lacks them) the parts
# render static at rest — a valid degraded state. The remaining unwitnessed piece of
# D-PLAYERINFO-1 is the in-world (spawned-player) combo binding, not this preview idle.

const FlyCameraScript = preload("res://game/fly_camera.gd")

# The standing character uses only the compatible third-person slots. `resolve_combo()`
# also returns `arms`, but retail arm graphics reference bones outside this preview rig.
const THIRD_PERSON_SLOTS := ["head", "body"]
# Portrait tuning: a front portrait of a standing soldier for
# the player.mnu PLAYER_PREVIEW pane. Camera yaw 0 puts the orbit camera on +Z and the
# .3di parts import +Z-forward (see MissionObjectPlacer.bms_to_godot_basis), so the
# character faces the viewer with no model rotation. The distance scale fits a ~1.8 m
# figure in the tall pane; the slight downward pitch reads like a person standing just
# below eye level. The pose is framed once and held stable across selections (no jump).
const MENU_DISTANCE_SCALE := 2.7
const MENU_PITCH := -0.06

# The menu's anamorphic design space (menu_shell.gd scales the whole menu tree from this
# to the window). The SubViewport is rendered at the on-screen pixel size
# (design size x this scale) so the menu's upscale no longer blurs a low-res texture.
const MENU_DESIGN_SIZE := Vector2(800.0, 600.0)

# The witnessed preview animation + skeletal-idle values live at engine
# avatars/preview_animation.h, re-exported through AvatarDatabase statics.
# The idle clip registers under the canonical idle key so
# play_body_clip / slot_to_key resolve it.
const PREVIEW_IDLE_KEY := "anim_idle"

var _resource_root: ResourceRoot = null  # null when headless / no shell

var _viewport_container: SubViewportContainer
var _status_label: Label
var _viewport: SubViewport
var _root: Node3D
var _environment: MissionEnvironment
var _camera: FlyCamera
var _menu_pose_set := false
# Menu-portrait animation state (see _process). The part models hang off a spin node so the
# model rotates without moving the camera; the camera only zooms.
var _model_root: Node3D
var _hovered := false
var _zoom_blend := 0.0      # 0 at rest, damped toward 1 while hovered
var _idle_angle := 0.0      # continuous idle rotation (radians)
var _anim_time := 0.0       # seconds, drives the hover sway
var _menu_center := Vector3.ZERO
var _menu_radius := 1.0
var _menu_framed := false

# Loaded part models keyed by slot ("head"/"body"/"arms") -> ObjectModel.
var _part_models: Dictionary = {}
# Shared skeletal idle (Dt1rst.bad + PI_Idle.BAD), built lazily once and bound onto every
# skinned part so the composed character plays the idle. Null when the .bad assets aren't
# resolvable; _skeletal_tried gates the one-time build so a missing-asset mount reads once.
var _skeletal: SkeletalAnim = null
var _skeletal_tried := false
var _missing_parts := PackedStringArray()


func _ready() -> void:
	# The portrait is transparent to mouse input -- the frame pump owns hover and every
	# click, including the dropdown rows retail authors over the preview rect.
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	clip_contents = true
	_build_viewport()


func set_resource_root(root: ResourceRoot) -> void:
	_resource_root = root
	# Re-evaluate the skeletal idle against the new mount (a remount may add/remove the .bad set).
	_skeletal = null
	_skeletal_tried = false


func get_resource_root() -> ResourceRoot:
	return _resource_root


# --- Typed read seams (tests and the MCP read the portrait through these) ---

func portrait_camera() -> FlyCamera:
	return _camera


func viewport_container() -> SubViewportContainer:
	return _viewport_container


func preview_viewport() -> SubViewport:
	return _viewport


## The spin node's current yaw (idle spin + hover sway), radians.
func model_yaw() -> float:
	return _model_root.rotation.y if _model_root != null else 0.0


func menu_center() -> Vector3:
	return _menu_center


func zoom_blend() -> float:
	return _zoom_blend


## Frame the portrait on `bounds` and hold that pose across selection refreshes.
func frame_menu_pose(bounds: AABB) -> void:
	_frame_menu_pose(bounds)
	_menu_pose_set = true


## Re-run the selection refresh (frames once, then holds the pose).
func refresh_portrait() -> void:
	_refresh_portrait()


# Size the SubViewport to the true on-screen pixel footprint of this pane. The menu scales
# this control by s = window / 800x600; counter-scaling the container by 1/s while sizing it
# to base*s makes SubViewportContainer.stretch render the viewport at base*s (on-screen px)
# yet still visually fill the design-space rect. Guarded so a zero/!inside-tree size is a
# no-op.
func _apply_menu_viewport_resolution() -> void:
	if _viewport_container == null or not is_inside_tree():
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
	# assigns it -- the moment to (re)apply the on-screen render resolution.
	if what == NOTIFICATION_RESIZED:
		_apply_menu_viewport_resolution()


func _build_viewport() -> void:
	_viewport_container = SubViewportContainer.new()
	_viewport_container.set_anchors_preset(Control.PRESET_FULL_RECT)
	_viewport_container.stretch = true
	_viewport_container.mouse_filter = Control.MOUSE_FILTER_IGNORE
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
	_viewport.handle_input_locally = false
	_viewport.gui_disable_input = true
	_viewport.msaa_3d = Viewport.MSAA_4X
	_viewport_container.add_child(_viewport)

	_root = Node3D.new()
	_viewport.add_child(_root)

	_environment = MissionEnvironment.new()
	_environment.name = "AvatarPreviewEnvironment"
	_root.add_child(_environment)

	# Part models hang off this spin node so the menu portrait can rotate the model while
	# the camera stays put.
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
	_camera.set_gameplay_locked(true)
	if get_viewport() != null \
			and not get_viewport().size_changed.is_connected(_apply_menu_viewport_resolution):
		get_viewport().size_changed.connect(_apply_menu_viewport_resolution)
	_apply_menu_viewport_resolution()
	_refresh_portrait()


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
		var part_dict := part as Dictionary
		var graphic := String(part_dict.get("graphic", "")).strip_edges()
		_load_part(slot, graphic, part_dict.get("camo", []))
	_refresh_status_label()
	_refresh_portrait()


# Build the shared skeletal idle once from the two raw .bad files the original binds
# [orig: PlayerInfo_InitPreviewModel @ 0x5600d0 BoneFile_Load + AnimChannel_InitFromData].
# Returns null (parts stay static) when there's no resource root, the .bad assets aren't
# present (a loose mount that lacks them), the native raw-.bad method is missing (stale DLL),
# or the load fails — all non-fatal degraded states. Cached; _skeletal_tried reads once.
func preview_skeletal() -> SkeletalAnim:
	if _skeletal_tried:
		return _skeletal
	_skeletal_tried = true
	if _resource_root == null:
		return null
	if not _resource_root.has_file(AvatarDatabase.preview_skeleton_bad()) \
			or not _resource_root.has_file(AvatarDatabase.preview_idle_bad()):
		return null
	var sk := SkeletalAnim.new()
	if not sk.load_from_bad_files(_resource_root, AvatarDatabase.preview_skeleton_bad(),
			{PREVIEW_IDLE_KEY: AvatarDatabase.preview_idle_bad()}):
		return null
	_skeletal = sk
	return _skeletal


# Load one part .3di by basename into a sibling ObjectModel under the root.
# No resource root, an empty name, or a load failure leaves the slot empty.
func _load_part(slot: String, graphic: String, camo: Array = []) -> void:
	if graphic.is_empty():
		_missing_parts.append("%s: empty graphic" % slot)
		return
	if _resource_root == null:
		_missing_parts.append("%s: no resource root" % slot)
		return
	var data := ObjectData.new()
	if data.open_from_resource_root(_resource_root, graphic) != OK:
		_missing_parts.append("%s: %s not found" % [slot, graphic])
		return
	var model := ObjectModel.new()
	model.name = "AvatarPart_%s" % slot
	_model_root.add_child(model)
	model.set_object_data(data)
	# Each part carries its own authored camo triplet, stored immediately before
	# that part's preview submit [orig: Avatar_SetHeadCamoCtrl @0x57a370 /
	# Avatar_SetBodyCamoCtrl @0x57a390 at PlayerInfo_RenderPlayerPreview3D
	# @0x56113c/@0x56110b].
	AvatarDatabase.apply_part_camo(model, camo, "avatar_preview:camo")
	model.set_active_lod(0)  # always the finest LOD in the portrait (defensive; 0 is the default)
	# Bind the shared skeletal idle so the skinned part plays PI_Idle.BAD on the Dt1rst skeleton,
	# like the original PLAYER_INFO preview [orig: PlayerInfo_InitPreviewModel @ 0x5600d0 ->
	# BoneFile_Load + AnimChannel_InitFromData("PI_Idle.BAD")]. Only skinned parts pose; an absent
	# .bad set or a static part leaves the slot at rest. The transform animation (idle spin +
	# hover zoom/sway) is driven separately in _process.
	var sk := preview_skeletal()
	if sk != null and data.is_skinned(0):
		model.set_skeletal_anim(sk)
		model.play_body_clip(PREVIEW_IDLE_KEY)
	_part_models[slot] = model


func get_part_model(slot: String) -> ObjectModel:
	return _part_models.get(slot, null)


func get_part_model_count() -> int:
	return _part_models.size()


func clear() -> void:
	for model in _part_models.values():
		if model != null and is_instance_valid(model):
			_model_root.remove_child(model)
			model.queue_free()
	_part_models.clear()
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


func _refresh_portrait() -> void:
	var bounds := _composed_bounds()
	# Frame once on the first real character, then hold the pose so the camera
	# never jumps as the player cycles combos.
	if not _menu_pose_set and bounds.size != Vector3.ZERO:
		_frame_menu_pose(bounds)
		_menu_pose_set = true


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
	# Random initial yaw: rand() % AvatarDatabase.PREVIEW_INITIAL_YAW_RANGE_DEG
	# degrees (the witness lives at the bound name's engine home — [orig:
	# 0x5600d0 dword_25DC53C = (rand()%180)*0xB60B60, 0-179 deg in BAM]); the
	# continuous idle spin in _process accumulates from this starting facing.
	_idle_angle = deg_to_rad(float(randi() % AvatarDatabase.PREVIEW_INITIAL_YAW_RANGE_DEG))
	_menu_framed = true
	_apply_menu_camera()  # initial pose; _process re-poses each frame with the zoom blend


# Position the menu camera front-on at the current zoom. The model's rotation lives on
# _model_root (see _process), so the camera stays put and only its distance changes.
func _apply_menu_camera() -> void:
	if _camera == null or not _menu_framed:
		return
	var dist_scale: float = MENU_DISTANCE_SCALE * lerpf(1.0, AvatarDatabase.preview_zoom_in_scale(), _zoom_blend)
	_camera.frame_bounds_custom(_menu_center, _menu_radius, dist_scale,
		maxf(_menu_radius * 8.0, 6.0), 0.0, MENU_PITCH)


# Hover toggles the zoom + sway, matching the original's "active when over the preview/lists"
# test [orig: update_player_preview_animation @ 0x55dba0].
func set_hovered(value: bool) -> void:
	_hovered = value


# Per-frame PLAYER_INFO portrait animation [orig: update_player_preview_animation @ 0x55dba0]:
# a damped zoom toward the hover target, a continuous idle rotation, and a sinusoidal sway
# that fades in on hover.
func _process(delta: float) -> void:
	advance(delta)


## One portrait animation step of `delta` seconds (the _process body; tests
## drive it directly).
func advance(delta: float) -> void:
	if not _menu_framed:
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
	var t: float = clampf(AvatarDatabase.preview_zoom_damp_per_tick() \
			* delta / Simulation.tick_dt(), 0.0, 1.0)
	_zoom_blend = lerpf(_zoom_blend, target, t)
	# Continuous idle spin; on hover a gentle sway fades in over it (scaled by the zoom blend).
	_idle_angle += deg_to_rad(AvatarDatabase.preview_idle_speed_deg_per_sec()) * delta
	_anim_time += delta
	var sway: float = sin(_anim_time * AvatarDatabase.preview_sway_freq_rad_per_sec()) \
			* deg_to_rad(AvatarDatabase.preview_sway_amp_deg()) * _zoom_blend
	if _model_root != null:
		_model_root.rotation.y = _idle_angle + sway
	_apply_menu_camera()
