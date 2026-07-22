extends GutTest

# AvatarPreview composes a resolved combo's part models. The fixture's parts
# reference real .3di basenames; without a mounted resource root the part .3di
# files cannot be resolved, so load_combo composes zero models but must not error
# (a missing graphic skips its slot). With no resource root we still verify the
# clear()/load_combo lifecycle is exercised cleanly on the SubViewport scaffold.
const AvatarPreviewScript = preload("res://engine/avatar/avatar_preview.gd")
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


func _model_fixture_root():
	var root := NovaResourceRoot.new()
	var dir := ProjectSettings.globalize_path("res://../fixtures/threedi/3di3")
	return root if root.set_root_dir(dir) == OK else null


func _three_slot_fixture_combo() -> Dictionary:
	var part := {"graphic": "CharModel.3di"}
	return {"head": part, "body": part, "arms": part}


func test_combo_preview_excludes_incompatible_arm_rig() -> void:
	# Retail combo arm graphics reference bones outside the 19-bone standing preview
	# rig and must not be overlaid on the third-person head + body. Drive load_combo
	# with a committed graphic in every slot so this policy regression is deterministic
	# and asset-independent.
	var root = _model_fixture_root()
	assert_not_null(root, "committed model fixture mounts")
	if root == null:
		return
	_preview.set_resource_root(root)
	_preview.load_combo(_three_slot_fixture_combo())
	var head = _preview.get_part_model("head")
	var body = _preview.get_part_model("body")
	var arms = _preview.get_part_model("arms")
	var has_head := is_instance_valid(head)
	var has_body := is_instance_valid(body)
	var has_arms := is_instance_valid(arms)
	var count: int = int(_preview.get_part_model_count())
	_preview.clear()
	await get_tree().process_frame
	assert_true(has_head, "the third-person head composes")
	assert_true(has_body, "the third-person body composes")
	assert_false(has_arms, "the incompatible arms slot is excluded")
	assert_eq(count, 2, "only the third-person character parts compose")


func test_retail_combo_preview_excludes_incompatible_arm_rig() -> void:
	# Exact regression for the screenshot path. Before the fix, ArmsG.3di was
	# forced onto the 19-bone portrait rig despite positive weights referencing up
	# to bone 36, producing the duplicate limbs and frame-spanning triangles.
	var root = _retail_root()
	if root == null:
		pending("OPENNOVA_JO_DIR / retail PFFs not configured")
		return
	var combo := _resolved_combo()
	if combo.is_empty():
		pending("Avatars.def fixture missing or has no combos")
		return
	_preview.set_resource_root(root)
	_preview.load_combo(combo)
	var head = _preview.get_part_model("head")
	var body = _preview.get_part_model("body")
	var arms = _preview.get_part_model("arms")
	var has_head := is_instance_valid(head)
	var has_body := is_instance_valid(body)
	var has_arms := is_instance_valid(arms)
	_preview.clear()
	await get_tree().process_frame
	assert_true(has_head, "the retail third-person head composes")
	assert_true(has_body, "the retail third-person body composes")
	assert_false(has_arms, "the retail incompatible arms model is not overlaid")


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


func test_editor_preview_uses_full_character_distance_margin() -> void:
	var bounds := _soldier_bounds()
	_preview._has_framed = false
	_preview._frame_bounds(bounds)
	var radius := maxf(bounds.size.length() * 0.5, 1.0)
	var distance: float = (_preview.get_editor_camera().global_position - bounds.get_center()).length()
	assert_gte(distance, radius * 2.5,
			"editor framing preserves the full-character distance margin")


func test_menu_preview_keeps_incompatible_arm_rig_excluded() -> void:
	var root = _model_fixture_root()
	assert_not_null(root, "committed model fixture mounts")
	if root == null:
		return
	_preview.set_menu_preview(true)
	_preview.set_resource_root(root)
	_preview.load_combo(_three_slot_fixture_combo())
	var has_arms := is_instance_valid(_preview.get_part_model("arms"))
	var count: int = int(_preview.get_part_model_count())
	_preview.clear()
	await get_tree().process_frame
	assert_false(has_arms, "the menu does not overlay the incompatible arms rig")
	assert_eq(count, 2, "the menu composes only the third-person character")


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


func test_menu_preview_idle_spins_and_holds_the_camera() -> void:
	# At rest the model rotates continuously (idle spin) while the camera holds its front pose.
	_preview.set_menu_preview(true)
	_preview._frame_menu_pose(_soldier_bounds())
	var cam = _preview.get_editor_camera()
	var before_angle: float = _preview._model_root.rotation.y
	var rest_dist: float = (cam.global_position - _preview._menu_center).length()
	for i in range(5):
		_preview._process(0.1)
	assert_gt(_preview._model_root.rotation.y, before_angle, "the model idle-spins")
	var dist_after: float = (cam.global_position - _preview._menu_center).length()
	assert_almost_eq(dist_after, rest_dist, 0.05, "the camera holds its distance at rest (no zoom)")
	assert_gt(cam.global_position.z, _preview._menu_center.z, "camera stays in front (+Z)")


func test_menu_preview_hover_zooms_in_and_out() -> void:
	# Mouseover ramps the zoom blend toward 1 and pulls the camera closer; un-hover relaxes it.
	_preview.set_menu_preview(true)
	_preview._frame_menu_pose(_soldier_bounds())
	var cam = _preview.get_editor_camera()
	var rest_dist: float = (cam.global_position - _preview._menu_center).length()
	_preview.set_hovered(true)
	for i in range(30):
		_preview._process(0.1)
	assert_gt(_preview._zoom_blend, 0.5, "hover ramps the zoom blend toward 1")
	var hover_dist: float = (cam.global_position - _preview._menu_center).length()
	assert_lt(hover_dist, rest_dist, "the camera zooms in (closer) on hover")
	_preview.set_hovered(false)
	for i in range(30):
		_preview._process(0.1)
	assert_lt(_preview._zoom_blend, 0.5, "un-hover relaxes the zoom blend toward 0")


# --- Skeletal idle (D-PLAYERINFO-1) -------------------------------------------
# The composed parts share one skeletal idle (Dt1rst.bad rest + PI_Idle.BAD clip) when those
# raw .bad assets resolve [orig: PlayerInfo_InitPreviewModel @ 0x5600d0]. Without them the
# preview stays static — a valid degraded state that must never error.

func test_preview_skeletal_is_null_without_bad_assets() -> void:
	# The avatars fixture dir has no Dt1rst.bad / PI_Idle.BAD, so the lazy builder returns null
	# and the preview composes static parts (no crash, no error).
	var root = _resource_root_for_fixture()
	if root == null:
		pending("fixtures/avatars not mountable")
		return
	_preview.set_resource_root(root)
	assert_null(_preview._ensure_preview_skeletal(),
		"no skeletal idle built when the .bad assets are absent")


func test_preview_skeletal_builds_when_bad_assets_resolve() -> void:
	# Stage a root that DOES carry the two raw .bad files (copy the committed idle.bad under the
	# names the preview binds) and assert the lazy builder produces a loaded, cached skeletal idle.
	var root = _staged_idle_root()
	if root == null:
		pending("could not stage Dt1rst.bad / PI_Idle.BAD from fixtures/anim")
		return
	_preview.set_resource_root(root)
	var sk = _preview._ensure_preview_skeletal()
	assert_not_null(sk, "skeletal idle built when both .bad files resolve")
	if sk != null:
		assert_true(sk.is_loaded(), "the skeletal set is loaded")
		assert_true(sk.has_clip("anim_idle"), "the idle clip is registered under anim_idle")
	assert_eq(_preview._ensure_preview_skeletal(), sk, "the skeletal idle is cached (built once)")


func test_menu_preview_binds_idle_on_skinned_parts_with_real_assets() -> void:
	# On a machine with the retail PFFs (OPENNOVA_JO_DIR), the menu portrait binds the skeletal
	# idle onto the skinned head/body parts. Gated: pends when the real assets aren't reachable
	# (the live visual verify covers the on-screen result).
	var root = _retail_root()
	if root == null:
		pending("OPENNOVA_JO_DIR / retail PFFs not configured; skeletal idle bind verified live")
		return
	var combo := _resolved_combo()
	if combo.is_empty():
		pending("Avatars.def fixture missing or has no combos")
		return
	_preview.set_menu_preview(true)
	_preview.set_resource_root(root)
	_preview.load_combo(combo)
	await get_tree().process_frame
	var body = _preview.get_part_model("body")
	if body == null:
		pending("body part .3di not resolved from the mounted root")
		return
	var data = body.get_object_data()
	if data != null and data.is_skinned(0):
		assert_true(body.has_skeleton(), "the skinned body part builds a Skeleton3D under the idle")
		assert_eq(body.get_active_body_clip(), "anim_idle", "the idle clip is playing on the part")
	else:
		pending("body part is not vertex-skinned on this asset set (rigid-attach is D-PLAYERINFO-1)")


# Copy the committed fixtures/anim/idle.bad into a temp dir under the names the preview binds
# (Dt1rst.bad + PI_Idle.BAD) and mount it. Returns null if the source fixture is missing.
func _staged_idle_root():
	# Read the committed clip through NovaResourceRoot (C++ path-normalized; FileAccess chokes on
	# the res://../ path), then write it out under the two names the preview binds.
	var src_root := NovaResourceRoot.new()
	if src_root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/anim")) != OK:
		return null
	var bytes := src_root.read_file("idle.bad")
	if bytes.is_empty():
		return null
	# Stage under the cache dir (LocalAppData), not user:// — NovaResourceRoot.set_root_dir
	# rejects any path under the Godot user-data dir (is_valid_root).
	var dir := OS.get_cache_dir().path_join("opennova_avatar_idle_test")
	DirAccess.make_dir_recursive_absolute(dir)
	for name in ["Dt1rst.bad", "PI_Idle.BAD"]:
		var f := FileAccess.open(dir.path_join(name), FileAccess.WRITE)
		if f == null:
			return null
		f.store_buffer(bytes)
		f.close()
	var root := NovaResourceRoot.new()
	if root.set_root_dir(dir) != OK:
		return null
	return root


# A NovaResourceRoot on the retail PFF install named by OPENNOVA_JO_DIR (machine-specific;
# set in settings.local.json env, never tracked). Null when unset or the .bad set is absent.
func _retail_root():
	var dir := OS.get_environment("OPENNOVA_JO_DIR")
	if dir.is_empty() or not DirAccess.dir_exists_absolute(dir):
		return null
	var root := NovaResourceRoot.new()
	if root.mount_runtime(dir, "", false, "jo") != OK:
		return null
	if not root.has_file("PI_Idle.BAD") or not root.has_file("Dt1rst.bad"):
		return null
	return root


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
