extends RefCounted

## The editor as the GUT tests boot it: boot makes the OpenNova Editor (editor_root.tscn, the editor-enabled
## variant) in the test's tree with its settings in a scratch folder of its own, and answers its typed seam
## (editor_seam.gd; its `app` the editor); teardown removes the folders a test made. Test-only, preloaded by path
## (`preload("res://tests/authoring/editor_fixture.gd")`), as the seam is, so no class cache stands between a fresh
## worktree and its tests.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")


## The editor booted in `test`'s tree, its settings in a scratch folder named after `label` (kept in `dirs` for
## teardown): its seam, null when the editor scene does not load (the test's failure says so). `frames` (a windowed
## run's) run first, the ImGui context past its first frames before the editor attaches to it (a context whose first
## NewFrame ran without docking asserts when docking is enabled exactly at its second: imgui.cpp's frame sanity
## check), and then one more once the editor is in the tree.
static func boot(test: GutTest, label: String, dirs: Array[String], frames := 0) -> RefCounted:
	for _frame in frames:
		await test.get_tree().process_frame
	var packed := load(EDITOR_SCENE) as PackedScene
	test.assert_not_null(packed, "the editor scene loads (the editor-enabled variant is loaded)")
	if packed == null:
		return null
	var app := packed.instantiate()
	var settings_dir := OS.get_cache_dir().path_join("opennova editor %s %d" % [label, Time.get_ticks_usec()])
	test.assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	dirs.append(settings_dir)
	app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	test.add_child_autofree(app)
	if frames > 0:
		await test.get_tree().process_frame
	return EditorSeam.new(app)


## Every folder in `dirs` removed, then a frame: a device's rebuild queues its old nodes for deletion (ObjectModel's
## clear), which the frame frees before GUT counts orphans.
static func teardown(test: GutTest, dirs: Array[String]) -> void:
	for dir in dirs:
		TestFs.remove_dir_recursive(dir)
	dirs.clear()
	await test.get_tree().process_frame
