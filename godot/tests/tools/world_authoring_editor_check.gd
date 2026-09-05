@tool
extends EditorScript
## Run in a disposable editor project: drives real Inspector fields, undo,
## toolbar actions, scene switches and Godot Save, with synthetic native data.

const SCENE := "res://examples/world_authoring_check.tscn"
var failures: PackedStringArray = []
var checks: PackedStringArray = []
var _tree: SceneTree
var _directory := ""
var _bar: HBoxContainer


func _run() -> void:
	run_check()


func expect(value: bool, message: String) -> void:
	checks.append(message)
	if not value:
		failures.append(message)


func frames(count: int = 12) -> void:
	for frame in range(count):
		await _tree.process_frame


func property_named(node: Node, caption: String) -> EditorProperty:
	if node is EditorProperty and (node as EditorProperty).label == caption:
		return node as EditorProperty
	for child in node.get_children():
		var found := property_named(child, caption)
		if found != null:
			return found
	return null


func input_for(caption: String, type: String) -> Control:
	var property := property_named(EditorInterface.get_inspector(), caption)
	expect(property != null, "Inspector contains " + caption)
	if property == null:
		return null
	for child in property.get_children():
		if child.is_class(type):
			return child as Control
	return null


func dialog_named(title: String) -> ConfirmationDialog:
	for child in _bar.get_children():
		if child is ConfirmationDialog and (child as ConfirmationDialog).title == title:
			return child as ConfirmationDialog
	return null


func close_prompt(node: Node) -> ConfirmationDialog:
	if node is ConfirmationDialog and (node as ConfirmationDialog).visible \
			and (node as ConfirmationDialog).dialog_text.contains("Native world files"):
		return node as ConfirmationDialog
	for child in node.get_children():
		var found := close_prompt(child)
		if found != null:
			return found
	return null


func run_check() -> void:
	_tree = EditorInterface.get_base_control().get_tree()
	await frames(60)
	_directory = OS.get_cache_dir().path_join("opennova_editor_author_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(_directory)
	var source_dir := ProjectSettings.globalize_path("res://../examples/world_preview")
	for filename in DirAccess.get_files_at(source_dir):
		DirAccess.copy_absolute(source_dir.path_join(filename), _directory.path_join(filename))
	var source := WorldSource.new()
	source.mission_name = "preview.bms"
	source.data_directory = _directory
	var initial_mission := source.open_mission(source.open_root())
	initial_mission.set_header_int("start_time", 1743)
	initial_mission.save_as(_directory.path_join("preview.bms"))
	var terrain := TerrainData.new()
	terrain.load_from_resource_root(source.open_root(), "Tmap.trn")
	var definition := TerrainFoliageDef.new()
	definition.graphic = "crate.3di"
	definition.match = 254
	terrain.set_foliage_defs([definition])
	terrain.save_to_path(_directory.path_join("Tmap.trn"))
	var scene_world := load("res://examples/world_preview.tscn").instantiate() as GameWorld
	scene_world.world_source = source
	var packed := PackedScene.new()
	expect(packed.pack(scene_world) == OK, "Pack disposable authoring scene")
	expect(ResourceSaver.save(packed, SCENE) == OK, "Save disposable authoring scene")
	scene_world.free()
	EditorInterface.open_scene_from_path(SCENE)
	EditorInterface.set_main_screen_editor("3D")
	await frames(80)
	var world := EditorInterface.get_edited_scene_root() as GameWorld
	expect(world != null and world.is_preview_active(), "Native preview is active")
	_bar = EditorInterface.get_base_control().find_child("WorldPreviewToolbar", true, false) as HBoxContainer
	if world == null or _bar == null:
		finish()
		return
	EditorInterface.inspect_object(world)
	await frames()
	var time := input_for("Start time", "LineEdit") as LineEdit
	if time == null:
		finish()
		return
	time.focus_exited.emit()
	await frames()
	expect(world.get_loaded_mission().get_info().start_time == 1743, "Focusing time preserves native sub-minute precision")
	time.text = "18:30"
	time.text_submitted.emit(time.text)
	await frames()
	expect(world.get_loaded_mission().get_info().start_time == 0x1280, "Inspector edits the native mission")
	expect((_bar.get_node("Status") as Label).text.contains("Unsaved"), "Native dirty state appears in the toolbar")
	var manager := EditorInterface.get_editor_undo_redo()
	var history := manager.get_history_undo_redo(manager.get_object_history_id(world))
	expect(history.has_undo(), "Godot owns the native edit's undo action")
	history.undo()
	await frames()
	expect(world.get_loaded_mission().get_info().start_time != 0x1280, "Editor undo restores the native time")
	history.redo()
	await frames()
	expect(world.get_loaded_mission().get_info().start_time == 0x1280, "Editor redo restores the edit")
	EditorInterface.inspect_object(world.get_node("MissionEnvironment"))
	await frames()
	var height := input_for("Sky height", "SpinBox") as SpinBox
	if height == null:
		finish()
		return
	height.value = 233
	await frames()
	expect((world.get_node("MissionEnvironment") as MissionEnvironment).environment_data.sky_height == 233,
			"Inspector updates the effective environment preview")
	EditorInterface.inspect_object(world.get_node("Terrain/FoliageDispatcher"))
	await frames()
	var map_match := input_for("Map match", "SpinBox") as SpinBox
	var shadow := input_for("Cast shadow", "CheckBox") as CheckBox
	if map_match == null or shadow == null:
		finish()
		return
	map_match.value = 253
	shadow.button_pressed = true
	await frames()
	expect((world.get_terrain_data().get_foliage_defs()[0] as TerrainFoliageDef).match == 253,
			"Inspector edits native foliage definitions")
	var screenshot := ProjectSettings.globalize_path("res://../.scratch/world-authoring-inspector.png")
	DirAccess.make_dir_recursive_absolute(screenshot.get_base_dir())
	if DisplayServer.get_name() != "headless":
		EditorInterface.get_base_control().get_viewport().get_texture().get_image().save_png(screenshot)
	(_bar.get_node("Reload") as Button).pressed.emit()
	await frames()
	var reload := dialog_named("Reload native world files")
	expect(reload != null and reload.visible, "Reload asks before discarding native edits")
	reload.hide()
	var close := InputEventKey.new()
	close.keycode = KEY_W
	close.ctrl_pressed = true
	close.shift_pressed = true
	close.pressed = true
	Input.parse_input_event(close)
	await frames()
	var prompt := close_prompt(_tree.root)
	expect(prompt != null, "Godot Close Scene asks about unsaved native files")
	if prompt != null:
		prompt.get_cancel_button().pressed.emit()
	close.pressed = false
	Input.parse_input_event(close)
	await frames()
	EditorInterface.open_scene_from_path("res://game/world/game_world.tscn")
	await frames(30)
	EditorInterface.open_scene_from_path(SCENE)
	await frames(30)
	world = EditorInterface.get_edited_scene_root() as GameWorld
	expect(world != null and world.get_loaded_mission().get_info().start_time == 0x1280,
			"Scene switches retain pending native documents")
	expect(EditorInterface.save_scene() == OK, "Godot Save completes")
	await frames()
	var mission := MissionData.new()
	expect(mission.open_file(_directory.path_join("preview.bms")) == OK, "Saved BMS reopens")
	expect(mission.get_info().start_time == 0x1280, "Godot Save persisted native time")
	var environment := EnvFile.new()
	environment.source_path = _directory.path_join("mnml.env")
	environment.load()
	expect(environment.sky_height == 233, "Godot Save persisted base sky height")
	expect(not (_bar.get_node("Status") as Label).text.contains("Unsaved"), "Save clears native dirty state")
	var saved_scene := FileAccess.get_file_as_string(SCENE)
	for forbidden in ["TerrainData", "MissionObjects", "WorldEditSession", "CompositorEffect"]:
		expect(not saved_scene.contains(forbidden), "Scene excludes " + forbidden)
	(_bar.get_node("Copy") as Button).pressed.emit()
	await frames()
	var copy_dialog := dialog_named("Create editable world copy")
	for child in copy_dialog.get_children():
		if child is LineEdit:
			(child as LineEdit).text = "editor_copy"
	copy_dialog.confirmed.emit()
	await frames(35)
	expect(world.world_source.mission_name == "editor_copy.bms", "Create Copy selects the new WorldSource")
	expect(world.get_loaded_mission().get_terrain_ref() == "editor_copy", "The copied mission uses the copied terrain")
	EditorInterface.save_all_scenes()
	await frames()
	EditorInterface.close_scene()
	await frames()
	EditorInterface.open_scene_from_path(SCENE)
	await frames(35)
	world = EditorInterface.get_edited_scene_root() as GameWorld
	expect(world.world_source.mission_name == "editor_copy.bms", "Saved source reopens")
	expect(world.get_loaded_mission().get_info().start_time == 0x1280, "Reopened preview reads the saved native world")
	EditorInterface.close_scene()
	await frames()
	DirAccess.remove_absolute(ProjectSettings.globalize_path(SCENE))
	TestFs.remove_dir_recursive(_directory)
	finish()


func finish() -> void:
	var path := ProjectSettings.globalize_path("res://../.scratch/world-authoring-editor-check.json")
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(JSON.stringify({"checks": checks, "failures": failures}, "  "))
	file.close()
	print("World authoring editor check: ", checks.size(), " checks, ", failures.size(), " failures. ", path)
	if OS.get_cmdline_user_args().has("--world-authoring-check"):
		_tree.quit(0 if failures.is_empty() else 1)
