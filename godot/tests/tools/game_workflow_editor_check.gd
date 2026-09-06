@tool
extends EditorScript
## Run in a disposable graphical editor project. Uses real dock controls and
## native files; no production subclass or private-field access.

var scene_path := "res://examples/game_workflow_%d.tscn" % Time.get_ticks_usec()
var checks: PackedStringArray = []
var failures: PackedStringArray = []
var _tree: SceneTree


func _run() -> void:
	run_check()


func frames(count: int = 12) -> void:
	for frame in range(count):
		await _tree.process_frame


func expect(value: bool, message: String) -> void:
	checks.append(message)
	print("Workflow check: ", "PASS " if value else "FAIL ", message)
	if not value:
		failures.append(message)


func run_check() -> void:
	_tree = EditorInterface.get_base_control().get_tree()
	await frames(60)
	var directory := TestFs.stage_terrain_root("editor_workflow_%d" % Time.get_ticks_usec())
	var example := ProjectSettings.globalize_path("res://../assets")
	for name in DirAccess.get_files_at(example):
		DirAccess.copy_absolute(example.path_join(name), directory.path_join(name))
	var output := directory + "_packed"
	var source := WorldSource.new()
	source.data_directory = directory
	source.source_kind = WorldSource.LOOSE_SOURCE
	source.mission_name = "mnml.bms"
	var scene := load("res://examples/world_preview.tscn").instantiate() as GameWorld
	scene.world_source = source
	var packed := PackedScene.new()
	expect(packed.pack(scene) == OK and ResourceSaver.save(packed, scene_path) == OK, "Save disposable workflow scene")
	scene.free()
	EditorInterface.open_scene_from_path(scene_path)
	EditorInterface.set_main_screen_editor("3D")
	await frames(80)
	var world := EditorInterface.get_edited_scene_root() as GameWorld
	var panel := EditorInterface.get_base_control().find_child("GameData", true, false) as Control
	expect(world != null and world.is_preview_active(), "Real world preview is active")
	expect(panel != null, "OpenNova game data dock exists")
	if panel == null or world == null:
		finish()
		return
	var dock := panel.get_parent() as EditorDock
	expect(dock != null, "Godot owns the dock and its layout")
	dock.make_visible()
	await frames()
	var destination := panel.get_node("%OutputDirectory") as LineEdit
	var pack_button := panel.get_node("%Pack") as Button
	var run_button := panel.get_node("%RunGame") as Button
	var stop_button := panel.get_node("%Stop") as Button
	var status := panel.get_node("%Status") as Label
	var retail_button := panel.get_node("%StageRetail") as Button
	expect((panel.get_node("%Source") as Label).text.contains(directory), "Dock uses the active world's selected directory")
	expect(not run_button.disabled and stop_button.disabled, "Run enabled; Stop disabled before launch")
	(panel.get_node("%RetailDirectory") as LineEdit).text = ""
	(panel.get_node("%RetailDirectory") as LineEdit).text_changed.emit("")
	expect(retail_button.disabled, "Missing retail installation blocks staging")
	destination.text = directory
	destination.text_changed.emit(directory)
	expect(pack_button.disabled, "Source folder cannot be the pack output")
	destination.text = output
	destination.text_changed.emit(output)
	expect(not pack_button.disabled, "Separate output enables packing for a loose world")
	# A real Inspector pending edit must be flushed by the dock's Pack action.
	EditorInterface.inspect_object(world)
	await frames()
	var time_property := find_property(EditorInterface.get_inspector(), "Start time")
	expect(time_property != null, "Native time control is in the Inspector")
	if time_property != null:
		for child in time_property.get_children():
			if child is LineEdit:
				var input := child as LineEdit
				input.grab_focus()
				input.text = "06:30"
	pack_button.pressed.emit()
	await frames(30)
	expect(FileAccess.file_exists(output.path_join("localres.pff")), "Pack button writes a real archive")
	expect(status.text.begins_with("Packed "), "Dock reports pack result")
	var root := ResourceRoot.new()
	expect(root.mount_runtime(output) == OK, "Ordinary runtime mounts the packed game data")
	var readback := WorldSource.new()
	readback.data_directory = output
	readback.source_kind = WorldSource.RETAIL_INSTALL
	readback.mission_name = "mnml.bms"
	var mission := readback.open_mission(root)
	expect(mission != null and mission.get_info().start_time == 0x0680, "Pack flushes pending Inspector input into the native archive")
	root.clear()
	run_button.pressed.emit()
	await frames(10)
	expect(not stop_button.disabled and status.text.contains("OpenNova is running"), "Run Game starts an owned source-project child")
	stop_button.pressed.emit()
	await frames(40)
	expect(stop_button.disabled, "Stop releases the child")
	# Change source policy through the actual scene resource and reload toolbar.
	world.world_source.source_kind = WorldSource.RETAIL_INSTALL
	world.world_source.data_directory = output
	world.world_source.emit_changed()
	await frames()
	expect(run_button.disabled and pack_button.disabled, "Changed source blocks actions until Reload")
	var toolbar := EditorInterface.get_base_control().find_child("WorldPreviewToolbar", true, false)
	(toolbar.get_node("Reload") as Button).pressed.emit()
	await frames(45)
	expect(world.is_preview_active(), "Archive-backed packed world previews through the normal loader")
	expect(pack_button.disabled and pack_button.tooltip_text.contains("loose source"), "Archive-backed sources cannot silently drop dependencies during packing")
	world.world_source.source_kind = WorldSource.LOOSE_SOURCE
	world.world_source.data_directory = directory
	world.world_source.emit_changed()
	await frames()
	(toolbar.get_node("Reload") as Button).pressed.emit()
	await frames(45)
	expect(not pack_button.disabled, "Returning to the loose source restores packing")
	# Capture only the minimal world, and hide machine paths from the evidence.
	(panel.get_node("%Source") as Label).text = "mnml.bms | Minimal game data"
	destination.text = "Minimal pack output"
	status.text = "Packed game data verified through the ordinary runtime loader."
	await frames()
	await RenderingServer.frame_post_draw
	EditorInterface.get_base_control().get_viewport().get_texture().get_image().save_png(
			ProjectSettings.globalize_path("res://../.scratch/game-workflow-editor.png"))
	EditorInterface.save_all_scenes()
	EditorInterface.close_scene()
	await frames()
	expect(run_button.disabled and pack_button.disabled, "Closing the world clears the dock selection")
	EditorInterface.set_plugin_enabled("opennova_world", false)
	await frames()
	expect(not is_instance_valid(panel), "Disabling the plugin removes its dock")
	EditorInterface.set_plugin_enabled("opennova_world", true)
	await frames()
	var restored := EditorInterface.get_base_control().find_child("GameData", true, false)
	expect(restored != null, "Reenabling the plugin restores one dock")
	if restored != null:
		expect((restored.get_node("%OutputDirectory") as LineEdit).text == output, "Machine-local output selection survives plugin reload")
	DirAccess.remove_absolute(ProjectSettings.globalize_path(scene_path))
	TestFs.remove_dir_recursive(directory)
	TestFs.remove_dir_recursive(output)
	finish()


func find_property(node: Node, caption: String) -> EditorProperty:
	if node is EditorProperty and (node as EditorProperty).label == caption:
		return node as EditorProperty
	for child in node.get_children():
		var found := find_property(child, caption)
		if found != null:
			return found
	return null


func finish() -> void:
	var path := ProjectSettings.globalize_path("res://../.scratch/game-workflow-editor-check.json")
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(JSON.stringify({"checks": checks, "failures": failures}, "  "))
	file.close()
	print("Game workflow editor check: ", checks.size(), " checks, ", failures.size(), " failures. ", path)
	if OS.get_cmdline_user_args().has("--game-workflow-check"):
		_tree.quit(0 if failures.is_empty() else 1)
