class_name AvatarPreview
extends Control



# Runtime PLAYER_INFO portrait for an Avatars.def character combo: the device that
# draws the engine's portrait model (PlayerInfoPreview over menu::PlayerInfoPreview,
# which carries the witnesses: the parts it composes, their idle, its motion and
# hover). It composes the parts under a shared environment and holds a
# front-facing camera pose across combo changes. Without the idle's .bad pair
# (a loose-data mount that lacks them) the parts render static at rest, a valid
# degraded state. The remaining unwitnessed piece of D-PLAYERINFO-1 is the
# in-world (spawned-player) combo binding, not this preview idle.

const FlyCameraScript = preload("res://game/fly_camera.gd")

## Every frame the pane draws (its _process, which runs while it is visible): retail's
## PLAYER_PREVIEW custom draw stores the time of day it lights by into the process clock
## (PlayerInfoMenuCompanion.player_preview_drawn; engine
## inmatch::RemoteAdminConsoles::player_preview_drawn).
signal drawn

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

# A BAM word as radians: 2^32 to a turn.
const BAM_TO_RADIANS := TAU / 4294967296.0

var _resource_root: ResourceRoot = null  # null when headless / no shell

var _viewport_container: SubViewportContainer
var _status_label: Label
var _viewport: SubViewport
var _root: Node3D
var _environment: MissionEnvironment
var _camera: FlyCamera
var _menu_pose_set := false
# The part models hang off a spin node so the model rotates without moving the camera; the
# camera only zooms.
var _model_root: Node3D
## Whether the pointer hovers the portrait at all (the title's: MenuDriver.
## player_info_preview_hovers, engine Title::player_info_preview_hover); false, it is
## never hovered, so it spins at rest and never zooms or sways.
var hovers := true
# The engine's portrait model: the facing, the zoom blend, the hover and the update bank.
var _model := PlayerInfoPreview.new()
var _menu_center := Vector3.ZERO
var _menu_radius := 1.0

# Loaded part models keyed by slot ("head"/"body") -> ObjectModel.
var _part_models: Dictionary = {}
# The shared skeletal idle (PlayerInfoPreview.load_idle), built lazily once and bound onto
# every skinned part. Null when the .bad assets aren't resolvable; _skeletal_tried gates the
# one-time build so a missing-asset mount reads once.
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
	return _model.zoom_blend()


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
	elif what == NOTIFICATION_VISIBILITY_CHANGED:
		_sync_render_activity()


# Render activity follows visibility (the water reflection-viewport rule): a
# hidden portrait must not keep paying for a full 3D pass or its input-driven
# animation. UPDATE_ALWAYS returns with visibility.
func _sync_render_activity() -> void:
	var active := is_visible_in_tree()
	if _viewport != null:
		_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS \
				if active else SubViewport.UPDATE_DISABLED
	set_process(active)


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
	# Retail shaders write gamma-domain values and rely on one terminal display
	# decode per 3D view; previews without a FrameFx install it here.
	var display_decode := DisplayDecode.new()
	display_decode.name = "AvatarPreviewDisplayDecode"
	_root.add_child(display_decode)

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
	_sync_render_activity()
	_refresh_portrait()


# --- Combo composition --------------------------------------------------------

# Compose the parts the engine's portrait composes from the resolved combo
# (PlayerInfoPreview.composed_parts). A missing/unknown graphic simply skips that
# slot (no error). Re-frames the camera on the composed bounds.
func load_combo(combo: AvatarComboRow) -> void:
	clear()
	_missing_parts = PackedStringArray()
	for part: AvatarPartRow in PlayerInfoPreview.composed_parts(combo):
		var slot := "head" if part.get_kind() == AvatarDatabase.PART_HEAD else "body"
		_load_part(slot, part.graphic.strip_edges(), part.camo)
	_refresh_status_label()
	_refresh_portrait()


# The shared skeletal idle, built once over the mount (PlayerInfoPreview.load_idle);
# null (parts stay static) without a root, without the .bad pair or on a failed load.
func preview_skeletal() -> SkeletalAnim:
	if not _skeletal_tried:
		_skeletal_tried = true
		_skeletal = PlayerInfoPreview.load_idle(_resource_root)
	return _skeletal


# Load one part .3di by basename into a sibling ObjectModel under the root.
# No resource root, an empty name, or a load failure leaves the slot empty.
func _load_part(slot: String, graphic: String, camo: Vector3i) -> void:
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
	# Each part's own authored camo triplet, stored ahead of its submit (the engine's
	# kPlayerInfoPreviewParts carries the witness).
	AvatarDatabase.apply_part_camo(model, camo, "avatar_preview:camo")
	model.set_active_lod(0)  # always the finest LOD in the portrait (defensive; 0 is the default)
	# A skinned part plays the shared idle; an absent .bad set or a static part stands at rest.
	PlayerInfoPreview.pose_part(model, preview_skeletal())
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
	# A fresh model: the engine's initial facing and its update bank afresh.
	_model.frame()
	_apply_yaw()
	_apply_menu_camera()  # initial pose; _process re-poses each frame with the zoom blend


# Position the menu camera front-on at the current zoom. The model's rotation lives on
# _model_root (see _process), so the camera stays put and only its distance changes.
func _apply_menu_camera() -> void:
	if _camera == null or not _model.is_framed():
		return
	var dist_scale: float = _model.camera_distance_scale(MENU_DISTANCE_SCALE)
	_camera.frame_bounds_custom(_menu_center, _menu_radius, dist_scale,
		maxf(_menu_radius * 8.0, 6.0), 0.0, MENU_PITCH)


func _apply_yaw() -> void:
	if _model_root != null:
		_model_root.rotation.y = _model.yaw_bam() * BAM_TO_RADIANS


# The PLAYER_PREVIEW window's mouse-over edge (the menu pump's claim, which the
# companion relays), under the title's hover rule.
func set_hovered(value: bool) -> void:
	_model.set_hovered(value, hovers)


# The PLAYER_INFO portrait animation [orig: PlayerInfo_UpdatePlayerPreviewAnimation @ 0x55dba0]:
# the zoom blend toward the hover target, the yaw's rest spin or its hovered sway, once a
# menu update: the updates the game loop's one bank drained this frame (the engine
# frontend's menu bank, GameFrontend.last_menu_updates).
func _process(_delta: float) -> void:
	advance(GameFrontend.last_menu_updates())
	drawn.emit()


## `updates` menu updates of the portrait's motion (the _process body; tests drive it
## directly): the engine model steps once a drained menu update.
func advance(updates: int) -> void:
	if not _model.is_framed():
		return
	_model.advance(updates)
	_apply_yaw()
	_apply_menu_camera()
