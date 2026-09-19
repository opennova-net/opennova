extends GutTest

# Public binding regression for parsed snapshots and mount/refresh lifetime.
# Pointer identity and sharing with MissionKernel are covered by asset_store_test.
var _root_path: String


func before_each() -> void:
	_root_path = ProjectSettings.globalize_path(
		"res://.godot/shared_native_assets_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(_root_path), OK)


func after_each() -> void:
	var directory := DirAccess.open(_root_path)
	if directory != null:
		for name in directory.get_files():
			DirAccess.remove_absolute(_root_path.path_join(name))
		DirAccess.remove_absolute(_root_path)


func _stage(name: String, source: String) -> void:
	var file := FileAccess.open(_root_path.path_join(name), FileAccess.WRITE)
	assert_not_null(file)
	if file != null:
		file.store_buffer(FileAccess.get_file_as_bytes(source))
		file.close()


func _map(text: String) -> void:
	var file := FileAccess.open(_root_path.path_join("rig.adm"), FileAccess.WRITE)
	assert_not_null(file)
	if file != null:
		file.store_string(text)
		file.close()


func test_model_snapshots_share_until_refresh_and_survive_root_clear() -> void:
	_stage("model.3di", "res://../fixtures/threedi/synth/shed.3di")
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_root_path), OK)
	var first := ObjectData.new()
	assert_eq(first.open_from_resource_root(root, "model.3di"), OK)
	assert_false(first.is_skinned(0))

	_stage("model.3di", "res://../fixtures/threedi/synth/person.3di")
	var second := ObjectData.new()
	assert_eq(second.open_from_resource_root(root, "MODEL.3DI"), OK)
	assert_false(second.is_skinned(0), "same mount reuses the parsed snapshot")

	ResourceRoot.bump_cache_epoch()
	var refreshed := ObjectData.new()
	assert_eq(refreshed.open_from_resource_root(root, "model.3di"), OK)
	assert_true(refreshed.is_skinned(0), "explicit refresh exposes edited content")
	assert_false(first.is_skinned(0), "existing handles retain their snapshot")
	assert_false(second.is_skinned(0))

	root.clear()
	root = null
	assert_false(first.build_lod_submeshes(0).is_empty())
	assert_false(refreshed.build_lod_submeshes(0).is_empty())
	assert_true(refreshed.is_skinned(0))


func test_rig_snapshots_survive_remount_and_source_destruction() -> void:
	_stage("idle.bad", "res://../fixtures/anim/idle.bad")
	_stage("walk.bad", "res://../fixtures/anim/walk.bad")
	_map('anim_reset "idle"\nanim_walk "walk"\n')
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_root_path), OK)
	var first := SkeletalAnim.new()
	assert_true(first.load_from_resource_root(root, "rig.adm"))
	var expected: Array = first.eval_pose("anim_walk", 0.1)
	assert_false(expected.is_empty())

	_map('anim_reset "idle"\nanim_changed "walk"\n')
	assert_eq(root.set_root_dir(_root_path), OK)
	var second := SkeletalAnim.new()
	assert_true(second.load_from_resource_root(root, "rig.adm"))
	assert_true(first.has_clip("anim_walk"))
	assert_false(first.has_clip("anim_changed"))
	assert_true(second.has_clip("anim_changed"))
	assert_false(second.has_clip("anim_walk"))

	assert_ne(root.set_root_dir(_root_path.path_join("absent")), OK)
	var missing := SkeletalAnim.new()
	assert_false(missing.load_from_resource_root(root, "rig.adm"),
		"a rejected remount must not expose the previous native source")

	root.clear()
	root = null
	assert_eq(first.eval_pose("anim_walk", 0.1), expected)
	assert_eq(second.eval_pose("anim_changed", 0.1), expected)
