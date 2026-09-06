@tool
extends EditorScript
## Run from File > Run in Godot's Script editor in a disposable editor session.
## Exercises the real plugin UI, rendering, Save, Reload, and teardown.
## Writes a report and screenshots to .scratch/; see the preview addon README.

var failures: PackedStringArray = []
var state: Dictionary = {}
var _tree: SceneTree


func _run() -> void:
	run_check()


func expect(value: bool, message: String) -> void:
	if not value:
		failures.append(message)

func preview_of(world: Node) -> GameWorld:
	return world as GameWorld

func open_example_menu(node: Node) -> bool:
	if node is PopupMenu:
		for index in range(node.item_count):
			if node.get_item_text(index) == "OpenNova: Open example world":
				node.index_pressed.emit(index)
				return true
	for child in node.get_children():
		if open_example_menu(child):
			return true
	return false

func run_check() -> void:
	_tree = EditorInterface.get_base_control().get_tree()
	failures.clear()
	state.clear()
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path("res://../.scratch"))
	for frame in range(50):
		await _tree.process_frame
	EditorInterface.set_main_screen_editor("Script")
	expect(open_example_menu(EditorInterface.get_base_control()), "Example menu is present")
	for frame in range(80):
		await _tree.process_frame
	var world := EditorInterface.get_edited_scene_root() as GameWorld
	expect(world != null, "Example scene opens")
	if world == null:
		finish()
		return
	var preview := preview_of(world)
	expect(preview != null, "Editor plugin created its preview")
	if preview == null:
		finish()
		return
	expect(preview.is_preview_active(), "Preview is active: " + str(preview.get_preview_diagnostics()))
	expect(preview.get_sim() == null, "No Simulation")
	var water := world.get_node("Water") as Water
	expect(water.get_water_material().get_shader_parameter("u_reflection") == water.get_reflection_viewport().get_texture(),
			"Water reflection remains connected after editor scene entry")
	var bar := EditorInterface.get_base_control().find_child("WorldPreviewToolbar", true, false)
	expect(bar != null, "Native 3D toolbar present")
	if bar == null:
		finish()
		return
	expect(EditorInterface.get_editor_viewport_3d(0).get_camera_3d().global_position.y > 40.0, "Example automatically framed")
	(bar.get_node("Frame") as Button).pressed.emit()
	for frame in range(100):
		await _tree.process_frame
	var viewport := EditorInterface.get_editor_viewport_3d(0)
	state["camera"] = str(viewport.get_camera_3d().global_transform)
	state["preview_world"] = str(preview.get_world_3d().scenario)
	state["camera_world"] = str(viewport.get_camera_3d().get_world_3d().scenario)
	state["viewport_visible"] = viewport.get_parent().is_visible_in_tree()
	expect(viewport.get_parent().is_visible_in_tree(), "3D viewport is visible")
	expect(viewport.get_camera_3d().global_position.y > 40.0, "Camera framed above sample terrain")
	expect(preview.get_world_3d() == viewport.get_camera_3d().get_world_3d(), "Preview shares the editor camera render world")
	RenderingServer.force_draw()
	var rendered := viewport.get_texture().get_image()
	var roofs := 0
	var ground := 0
	for y in range(0, rendered.get_height(), 8):
		for x in range(0, rendered.get_width(), 8):
			var pixel := rendered.get_pixel(x, y)
			if pixel.r > 0.4 and pixel.r > pixel.g * 1.3 and pixel.g > pixel.b * 1.2:
				roofs += 1
			if pixel.g > pixel.r * 1.01 and pixel.g > pixel.b * 1.08:
				ground += 1
	expect(roofs > 100, "Rendered placed models")
	expect(ground > 100, "Rendered native terrain")
	state["roof_pixels"] = roofs
	state["terrain_pixels"] = ground
	expect(viewport.get_camera_3d().environment == preview.get_preview_environment(), "Loaded environment applied to editor view")
	viewport.get_texture().get_image().save_png(ProjectSettings.globalize_path("res://../.scratch/world-preview-viewport.png"))
	EditorInterface.get_base_control().get_viewport().get_texture().get_image().save_png(ProjectSettings.globalize_path("res://../.scratch/world-preview-editor.png"))
	var save_proof := "preview_save_%d" % Time.get_ticks_usec()
	var authored_name := world.name
	world.name = save_proof
	EditorInterface.mark_scene_as_unsaved()
	EditorInterface.save_scene()
	for frame in range(10):
		await _tree.process_frame
	var saved := FileAccess.get_file_as_string("res://examples/world_preview.tscn")
	expect(saved.contains(save_proof), "Editor Save persisted a new authored change")
	state["preview_scene_path"] = preview.scene_file_path
	world.name = authored_name
	EditorInterface.mark_scene_as_unsaved()
	EditorInterface.save_scene()
	for frame in range(10):
		await _tree.process_frame
	expect(not saved.contains("MissionObjects"), "Generated objects not serialized")
	expect(not saved.contains("TerrainData"), "Generated documents not serialized")
	expect(not saved.contains("CompositorEffect"), "Runtime compositor resources not serialized")
	expect(not saved.contains("shadow_caster_mask"), "Device initialization not serialized")
	var old_data := preview.get_terrain_data()
	(bar.get_node("Reload") as Button).pressed.emit()
	for frame in range(15):
		await _tree.process_frame
	expect(preview.get_terrain_data() != old_data, "Reload replaces native data on the same scene nodes")
	preview = preview_of(world)
	expect(preview != null and preview.is_preview_active(), "Reload rebuilt preview")
	var source := world.world_source
	source.mission_name = "missing.bms"
	(bar.get_node("Reload") as Button).pressed.emit()
	expect(not preview_of(world).is_preview_active(), "Missing mission clears preview")
	source.mission_name = "preview.bms"
	(bar.get_node("Reload") as Button).pressed.emit()
	for frame in range(15):
		await _tree.process_frame
	expect(preview_of(world).is_preview_active(), "Recovery from failed load")
	preview = preview_of(world)
	var camera := EditorInterface.get_editor_viewport_3d(0).get_camera_3d()
	var preview_environment := camera.environment
	EditorInterface.set_plugin_enabled("opennova_world", false)
	for frame in range(15):
		await _tree.process_frame
	expect(not preview.is_preview_active() and preview.get_terrain_data() == null, "Plugin disable clears scene node configuration")
	expect(camera.environment != preview_environment, "Camera environment restored")
	var original_environment := camera.environment
	var original_compositor := camera.compositor
	var original_height = RenderingServer.global_shader_parameter_get("opennova_water_height")
	RenderingServer.global_shader_parameter_set("opennova_water_height", 123.5)
	EditorInterface.set_plugin_enabled("opennova_world", true)
	for frame in range(20):
		await _tree.process_frame
	expect(preview_of(world).is_preview_active(), "Plugin reenable reloads the active scene")
	EditorInterface.set_plugin_enabled("opennova_world", false)
	for frame in range(15):
		await _tree.process_frame
	expect(RenderingServer.global_shader_parameter_get("opennova_water_height") == 123.5, "Shader globals restored")
	expect(camera.environment == original_environment, "Original camera environment preserved")
	expect(camera.compositor == original_compositor, "Original camera compositor preserved")
	RenderingServer.global_shader_parameter_set("opennova_water_height", original_height)
	EditorInterface.set_plugin_enabled("opennova_world", true)
	for frame in range(20):
		await _tree.process_frame
	preview = preview_of(world)
	EditorInterface.open_scene_from_path("res://game/world/game_world.tscn")
	for frame in range(20):
		await _tree.process_frame
	expect(not is_instance_valid(preview) or not preview.is_preview_active(), "Scene switch unloads preview")
	EditorInterface.set_plugin_enabled("opennova_world", false)
	for frame in range(10):
		await _tree.process_frame
	EditorInterface.set_plugin_enabled("opennova_world", true)
	for frame in range(10):
		await _tree.process_frame
	finish()

func finish() -> void:
	var file := FileAccess.open("res://../.scratch/world-preview-editor-check.json", FileAccess.WRITE)
	file.store_string(JSON.stringify({"failures": failures, "state": state}, "\t"))
	file.close()
	print("World preview editor check: ", "passed" if failures.is_empty() else str(failures))
	if OS.get_cmdline_user_args().has("--world-preview-check"):
		_tree.quit(0 if failures.is_empty() else 1)
