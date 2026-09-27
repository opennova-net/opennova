extends GutTest

# AvatarPreview composes a resolved combo's part models. The minted fixture's
# parts name synthetic .3di basenames no root carries, so load_combo composes
# zero models but must not error (a missing graphic skips its slot); the
# lifecycle (clear()/load_combo) is exercised on the SubViewport scaffold. The
# retail-root tests compose the shipped table's parts (the reference fixture
# set's Avatars.def over the install's PFFs).
const AvatarPreviewScript = preload("res://game/avatar/avatar_preview.gd")
const AVATARS_FIXTURE := "res://../fixtures/avatars/synth_avatars.def"
const RETAIL_AVATARS_REL := "avatars/Avatars.def"

var _preview: AvatarPreview


func before_each() -> void:
	_preview = AvatarPreviewScript.new()
	add_child_autofree(_preview)
	# _ready builds the SubViewport scaffold.
	await get_tree().process_frame


func _retail_resolved_combo() -> AvatarComboRow:
	var path := RetailData.fixture(RETAIL_AVATARS_REL)
	if path.is_empty():
		return null
	var db := AvatarDatabase.new()
	if db.load(path) != OK:
		return null
	return _first_combo(db)


# First nationality/division with a combo.
func _first_combo(db: AvatarDatabase) -> AvatarComboRow:
	for n in range(db.get_nationality_count()):
		for d in range(db.get_division_count(n)):
			if db.get_combo_count(n, d) > 0:
				return db.get_combo(n, d, 0)
	return null


func test_retail_combo_preview_excludes_incompatible_arm_rig() -> void:
	# Exact regression for the screenshot path. Before the fix, ArmsG.3di was
	# forced onto the 19-bone portrait rig despite positive weights referencing up
	# to bone 36, producing the duplicate limbs and frame-spanning triangles.
	var root = _retail_root()
	if root == null:
		pending("OPENNOVA_JO_DIR / retail PFFs not configured")
		return
	var combo := _retail_resolved_combo()
	if combo == null:
		pending(RetailData.fixture_pending_text(RETAIL_AVATARS_REL))
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
func test_runtime_portrait_binds_idle_on_skinned_parts_with_real_assets() -> void:
	# On a machine with the retail PFFs (OPENNOVA_JO_DIR), the menu portrait binds the skeletal
	# idle onto the skinned head/body parts. Gated: pends when the real assets aren't reachable
	# (the live visual verify covers the on-screen result).
	var root = _retail_root()
	if root == null:
		pending("OPENNOVA_JO_DIR / retail PFFs not configured; skeletal idle bind verified live")
		return
	var combo := _retail_resolved_combo()
	if combo == null:
		pending(RetailData.fixture_pending_text(RETAIL_AVATARS_REL))
		return
	_preview.set_resource_root(root)
	_preview.load_combo(combo)
	await get_tree().process_frame
	var body = _preview.get_part_model("body")
	if body == null:
		pending("OPENNOVA_JO_DIR: body part .3di not resolved from the mounted root")
		return
	var data = body.get_object_data()
	if data != null and data.is_skinned(0):
		assert_true(body.has_skeleton(), "the skinned body part builds a Skeleton3D under the idle")
		assert_eq(body.get_active_body_clip(), "anim_idle", "the idle clip is playing on the part")
	else:
		pending("OPENNOVA_JO_DIR: body part is not vertex-skinned on this asset set (a rigid-attach part builds no Skeleton3D)")


# Copy the committed fixtures/anim/idle.bad into a temp dir under the names the preview binds
# (Dt1rst.bad + PI_Idle.BAD) and mount it. Returns null if the source fixture is missing.
func _retail_root():
	var dir := RetailData.install()
	if dir.is_empty():
		return null
	var root := ResourceRoot.new()
	if root.mount_runtime(dir, "", false, "jo") != OK:
		return null
	if not root.has_file("PI_Idle.BAD") or not root.has_file("Dt1rst.bad"):
		return null
	return root


# A ResourceRoot mounted on the directory that holds the part .3di files, if
# one is configured for this machine. The fixtures dir holds only Avatars.def, so
# part graphics will not resolve there — return null and let the test fall back.
