extends GutTest

# AvatarPreview composes a resolved combo's part models. The fixture's parts
# reference real .3di basenames; without a mounted resource root the part .3di
# files cannot be resolved, so load_combo composes zero models but must not error
# (a missing graphic skips its slot). With no resource root we still verify the
# clear()/load_combo lifecycle is exercised cleanly on the SubViewport scaffold.
const AvatarPreviewScript = preload("res://modtools/avatar/avatar_preview.gd")
const AVATARS_FIXTURE := "res://../fixtures/avatars/Avatars.def"

var _preview


func before_each() -> void:
	_preview = AvatarPreviewScript.new()
	add_child_autofree(_preview)
	# _ready builds the SubViewport scaffold.
	await get_tree().process_frame


func _resolved_combo() -> Dictionary:
	var path := ProjectSettings.globalize_path(AVATARS_FIXTURE)
	if not FileAccess.file_exists(path):
		return {}
	var db := NovaAvatarDatabase.new()
	if db.load(path) != OK:
		return {}
	# First nationality/division with a combo.
	for n in range(db.get_nationality_count()):
		for d in range(db.get_division_count(n)):
			if db.get_combo_count(n, d) > 0:
				return db.resolve_combo(n, d, 0)
	return {}


func test_load_combo_composes_parts_when_root_mounted() -> void:
	var combo := _resolved_combo()
	if combo.is_empty():
		pending("Avatars.def fixture missing or has no combos")
		return
	var root = _resource_root_for_fixture()
	# Compose the combo. The path must always run cleanly (a missing part .3di just
	# skips its slot — no error). Whether any model composes depends on whether the
	# mounted root actually holds the part graphics, which the avatars fixture dir
	# does not, so assert composition only when the head graphic resolves there.
	if root != null:
		_preview.set_resource_root(root)
	_preview.load_combo(combo)
	await get_tree().process_frame
	if root != null and _head_graphic_resolves(root, combo):
		assert_gt(_preview.get_part_model_count(), 0, "composes at least one part model when its .3di resolves")
	else:
		pending("Part .3di files not present in the mounted root; load_combo path exercised without error")
		assert_eq(_preview.get_part_model_count(), 0, "no parts compose when the graphics are absent")


# True when the combo's head part graphic exists in the mounted root, so part
# composition is expected.
func _head_graphic_resolves(root, combo: Dictionary) -> bool:
	var head: Variant = combo.get("head", null)
	if not (head is Dictionary):
		return false
	var graphic := String((head as Dictionary).get("graphic", "")).strip_edges()
	return not graphic.is_empty() and root.has_file(graphic)


func test_clear_removes_part_models() -> void:
	var combo := _resolved_combo()
	if combo.is_empty():
		pending("Avatars.def fixture missing or has no combos")
		return
	var root = _resource_root_for_fixture()
	if root != null:
		_preview.set_resource_root(root)
	_preview.load_combo(combo)
	await get_tree().process_frame
	_preview.clear()
	assert_eq(_preview.get_part_model_count(), 0, "clear removes all part models")


# --- Menu portrait mode (runtime PLAYER_INFO) ---------------------------------
# set_menu_preview turns the interactive editor preview into the static player.mnu
# portrait: grid/axes hidden, camera locked, clicks passed through to the
# PLAYER_PREVIEW button, character facing the viewer, pose held across selections.

# A standing-soldier AABB (feet at y=0, ~1.8 m tall) for the framing assertions,
# since the fixtures dir has no .3di to compose real bounds from.
func _soldier_bounds() -> AABB:
	return AABB(Vector3(-0.4, 0.0, -0.4), Vector3(0.8, 1.8, 0.8))


func test_menu_preview_drops_the_arms_slot() -> void:
	# The portrait shows head + body only; arms is the FP arms model and reads wrong
	# overlaid on the standing figure. The editor keeps all three for inspection.
	assert_eq(_preview._active_slots(), ["head", "body", "arms"], "editor composes all slots")
	_preview.set_menu_preview(true)
	assert_eq(_preview._active_slots(), ["head", "body"], "menu portrait drops arms")


func test_menu_preview_hides_grid_and_axes() -> void:
	_preview.set_menu_preview(true)
	assert_false(_preview.is_grid_visible(), "grid hidden in menu mode")
	assert_false(_preview.is_axes_visible(), "axis gizmo hidden in menu mode")


func test_menu_preview_locks_camera_and_passes_clicks_through() -> void:
	_preview.set_menu_preview(true)
	var cam = _preview.get_editor_camera()
	assert_true(cam.get("_gameplay_locked"), "camera locked (no orbit/pan/fly) in menu mode")
	assert_eq(_preview._viewport_container.mouse_filter, Control.MOUSE_FILTER_IGNORE,
		"the SubViewport container does not eat clicks, so the button keeps them")
	assert_true(_preview._viewport.gui_disable_input, "SubViewport GUI input disabled")


func test_menu_preview_frames_a_front_facing_pose() -> void:
	_preview.set_menu_preview(true)
	var cam = _preview.get_editor_camera()
	var bounds := _soldier_bounds()
	_preview._frame_menu_pose(bounds)
	var center := bounds.get_center()
	# The .3di parts import +Z-forward; yaw 0 sits the camera on +Z in front of the
	# figure, so the character faces the viewer and the camera looks back toward -Z.
	assert_gt(cam.global_position.z, center.z, "camera is in front (+Z) of the figure")
	var forward: Vector3 = -cam.global_transform.basis.z
	assert_lt(forward.z, 0.0, "camera looks back toward the figure (-Z)")


func test_menu_preview_pose_holds_across_selection_refresh() -> void:
	_preview.set_menu_preview(true)
	var cam = _preview.get_editor_camera()
	_preview._frame_menu_pose(_soldier_bounds())
	_preview._menu_pose_set = true
	var before: Transform3D = cam.global_transform
	# A combo change re-runs the guide refresh; the camera must not jump.
	_preview._refresh_preview_guides()
	assert_eq(cam.global_transform, before, "camera pose unchanged when the selection changes")


func test_menu_preview_renders_at_onscreen_resolution() -> void:
	# The menu scales this subtree by s = window / 800x600. In menu mode the container is
	# sized to base*s (true on-screen px) and counter-scaled by 1/s, so SubViewportContainer
	# stretch renders the SubViewport at on-screen resolution instead of the 212x241 design
	# size that the menu would otherwise upscale into a blur.
	_preview.size = Vector2(212, 241)
	_preview.set_menu_preview(true)
	var win: Vector2 = _preview.get_viewport().get_visible_rect().size
	var sx := win.x / 800.0
	var sy := win.y / 600.0
	var c = _preview._viewport_container
	assert_almost_eq(c.size.x, 212.0 * sx, 1.0, "container width sized to on-screen px")
	assert_almost_eq(c.size.y, 241.0 * sy, 1.0, "container height sized to on-screen px")
	assert_almost_eq(c.scale.x, 1.0 / sx, 0.01, "container counter-scaled in x")
	assert_almost_eq(c.scale.y, 1.0 / sy, 0.01, "container counter-scaled in y")
	assert_true(c.stretch, "stretch stays on so the SubViewport renders at the container size")


# A NovaResourceRoot mounted on the directory that holds the part .3di files, if
# one is configured for this machine. The fixtures dir holds only Avatars.def, so
# part graphics will not resolve there — return null and let the test fall back.
func _resource_root_for_fixture():
	var dir := ProjectSettings.globalize_path("res://../fixtures/avatars")
	var root := NovaResourceRoot.new()
	if root.set_root_dir(dir) != OK:
		return null
	# The fixtures dir has no .3di, so return it only to exercise has_file paths;
	# part composition will be zero, which the caller accounts for.
	return root
