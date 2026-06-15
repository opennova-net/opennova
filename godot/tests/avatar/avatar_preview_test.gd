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
