@tool
extends EditorPlugin
## The authored GameWorld nodes hold the loaded native configuration.
## Native preview properties keep source-derived data out of scene saves.

const EXAMPLE := "res://examples/world_preview.tscn"
const RETAIL := "res://examples/retail_world.tscn"
const TOOLBAR := preload("res://addons/opennova_world/toolbar.tscn")

var _toolbar: HBoxContainer
var _status: Label
var _details: AcceptDialog
var _folder: FileDialog
var _world: GameWorld
var _decode: DisplayDecode
var _decode_compositor: Compositor
var _source: WorldSource
var _view_camera: Camera3D
var _view_environment: Environment
var _view_compositor: Compositor
var _globals: Dictionary[StringName, Variant] = {}
var _stale := false
var _generation := 0
var _framed_scenes: Dictionary[String, bool] = {}


func _enter_tree() -> void:
	_toolbar = TOOLBAR.instantiate() as HBoxContainer
	_status = _toolbar.get_node("Status") as Label
	(_toolbar.get_node("Source") as Button).pressed.connect(_inspect_source)
	(_toolbar.get_node("Reload") as Button).pressed.connect(_reload)
	(_toolbar.get_node("Frame") as Button).pressed.connect(_focus_preview)
	(_toolbar.get_node("Folder") as Button).pressed.connect(_choose_folder)
	(_toolbar.get_node("Details") as Button).pressed.connect(_show_details)
	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, _toolbar)
	_details = AcceptDialog.new()
	_details.title = "World preview"
	_toolbar.add_child(_details)
	_folder = FileDialog.new()
	_folder.title = "Local game data folder"
	_folder.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	_folder.access = FileDialog.ACCESS_FILESYSTEM
	_folder.dir_selected.connect(_folder_selected)
	_toolbar.add_child(_folder)
	scene_changed.connect(_scene_changed)
	add_tool_menu_item("OpenNova: Open example world", _open_world.bind(EXAMPLE))
	add_tool_menu_item("OpenNova: Open retail world", _open_world.bind(RETAIL))
	_scene_changed(EditorInterface.get_edited_scene_root())


func _exit_tree() -> void:
	_generation += 1
	_release_world()
	remove_tool_menu_item("OpenNova: Open example world")
	remove_tool_menu_item("OpenNova: Open retail world")
	remove_control_from_container(CONTAINER_SPATIAL_EDITOR_MENU, _toolbar)
	_toolbar.queue_free()


func _scene_changed(root: Node) -> void:
	_generation += 1
	_release_world()
	_world = _find_world(root)
	if is_instance_valid(_world):
		_world.tree_exiting.connect(_release_world, CONNECT_ONE_SHOT)
		_set_source(_world.world_source)
		_activate.call_deferred(_generation)
	else:
		_status.text = "Open a configured GameWorld scene."


func _find_world(node: Node) -> GameWorld:
	if node is GameWorld:
		return node as GameWorld
	if node != null:
		for child in node.get_children():
			var found := _find_world(child)
			if found != null:
				return found
	return null


func _activate(generation: int) -> void:
	# Let the editor finish restoring the scene and its navigation state.
	await get_tree().process_frame
	await get_tree().process_frame
	if generation == _generation and is_instance_valid(_world):
		_reload()
		var scene_path := EditorInterface.get_edited_scene_root().scene_file_path
		if not _framed_scenes.has(scene_path):
			_focus_preview()
			_framed_scenes[scene_path] = true


func _set_source(source: WorldSource) -> void:
	if _source != null and _source.changed.is_connected(_source_changed):
		_source.changed.disconnect(_source_changed)
	_source = source
	if _source != null:
		_source.changed.connect(_source_changed)


func _source_changed() -> void:
	_stale = true
	_status.text = "Source changed. Reload to update the preview."


func _process(_delta: float) -> void:
	if is_instance_valid(_world) and _world.world_source != _source:
		_set_source(_world.world_source)
		_source_changed()
	if not is_instance_valid(_world) or not _world.is_preview_active():
		return
	var viewport := EditorInterface.get_editor_viewport_3d(0)
	if viewport != null:
		var camera := viewport.get_camera_3d()
		if camera != null:
			_set_view_camera(camera)
			_world.refresh_preview(camera)


func _reload() -> void:
	_release_preview()
	if not is_instance_valid(_world):
		return
	_set_source(_world.world_source)
	if _source == null:
		_status.text = "Assign a WorldSource in the Inspector."
		return
	for shader_name in RenderingServer.global_shader_parameter_get_list():
		if String(shader_name).begins_with("opennova_"):
			_globals[shader_name] = RenderingServer.global_shader_parameter_get(shader_name)
	var error := _world.load_preview(_local_directory())
	if error == OK:
		_decode = DisplayDecode.new()
		var clear := _world.get_node_or_null("ClearColor") as WorldEnvironment
		_decode_compositor = _decode.create_view_compositor(clear.compositor if clear != null else null)
		_world.add_child(_decode, false, Node.INTERNAL_MODE_BACK)
	_stale = false
	_status.text = "%s: %s" % [_source.mission_name, _world.get_preview_status().capitalize()]
	if error != OK:
		_status.text = "Preview failed. See Details."
	_status.tooltip_text = "\n".join(_world.get_preview_diagnostics())


func _focus_preview() -> void:
	if not is_instance_valid(_world) or not _world.is_preview_active():
		return
	var view := _world.get_node_or_null("PreviewView") as Marker3D
	if view != null:
		var bookmark_viewport := EditorInterface.get_editor_viewport_3d(0)
		if bookmark_viewport != null:
			var bookmark_camera := bookmark_viewport.get_camera_3d()
			if bookmark_camera != null:
				bookmark_camera.global_transform = view.global_transform
		return
	var mission := _world.get_loaded_mission()
	var center := Vector3.ZERO
	var found := false
	for kind in [MissionData.KIND_MARKER, MissionData.KIND_BUILDING, MissionData.KIND_ITEM]:
		for entity: MissionEntityRecord in mission.get_entities(kind):
			if kind == MissionData.KIND_MARKER and entity.get_item_id() != 106001:
				continue
			center = MissionObjectPlacer.bms_to_godot_position(entity.get_position())
			found = true
			break
		if found:
			break
	if not found:
		var terrain := (_world.get_node("Terrain") as Terrain).get_terrain_data()
		if terrain != null:
			center.y = terrain.get_height_world(center)
	var viewport := EditorInterface.get_editor_viewport_3d(0)
	if viewport != null:
		var camera := viewport.get_camera_3d()
		if camera != null:
			# Godot 4.6 accepts external editor-camera transforms and updates
			# its navigation cursor from them on the next editor frame.
			center = _world.to_global(center)
			camera.look_at_from_position(center + Vector3(32, 24, 40), center)


func _set_view_camera(camera: Camera3D) -> void:
	if camera == _view_camera:
		return
	if is_instance_valid(_view_camera):
		_view_camera.environment = _view_environment
		_view_camera.compositor = _view_compositor
	_view_camera = camera
	_view_environment = null
	_view_compositor = null
	if camera != null:
		_view_environment = camera.environment
		_view_compositor = camera.compositor
		camera.environment = _world.get_preview_environment()
		camera.compositor = _decode_compositor


func _release_preview() -> void:
	_set_view_camera(null)
	if is_instance_valid(_decode):
		var parent := _decode.get_parent()
		if parent != null:
			parent.remove_child(_decode)
		_decode.free()
	_decode = null
	_decode_compositor = null
	if is_instance_valid(_world):
		_world.unload_preview()
	for shader_name: StringName in _globals:
		RenderingServer.global_shader_parameter_set(shader_name, _globals[shader_name])
	_globals.clear()


func _release_world() -> void:
	_release_preview()
	_set_source(null)
	if is_instance_valid(_world) and _world.tree_exiting.is_connected(_release_world):
		_world.tree_exiting.disconnect(_release_world)
	_world = null


func _inspect_source() -> void:
	if _source != null:
		EditorInterface.edit_resource(_source)
	elif is_instance_valid(_world):
		EditorInterface.inspect_object(_world)


func _local_directory() -> String:
	if _source == null or _source.install_key.is_empty():
		return ""
	return str(EditorInterface.get_editor_settings().get_project_metadata(
			"opennova_world", _source.install_key, ""))


func _choose_folder() -> void:
	if _source == null or _source.install_key.is_empty():
		_details.dialog_text = "Set an install_key on WorldSource to use a machine-local folder.\nUse data_directory for a shared project-relative source."
		_details.popup_centered()
		return
	_folder.current_dir = _local_directory()
	_folder.popup_centered_ratio(0.65)


func _folder_selected(path: String) -> void:
	if _source != null and not _source.install_key.is_empty():
		EditorInterface.get_editor_settings().set_project_metadata(
				"opennova_world", _source.install_key, path)
		_reload()


func _show_details() -> void:
	var lines := PackedStringArray(["Frozen authored world. Gameplay is not running.",
			"Only the primary 3D editor viewport drives this preview."])
	if _stale:
		lines.append("Source changed. Reload to apply the saved selection.")
	if is_instance_valid(_world):
		lines.append_array(_world.get_preview_diagnostics())
	_details.dialog_text = "\n".join(lines)
	_details.popup_centered(Vector2i(680, 240))


func _open_world(scene_path: String) -> void:
	EditorInterface.open_scene_from_path(scene_path)
	await get_tree().process_frame
	await get_tree().process_frame
	var root := EditorInterface.get_edited_scene_root()
	if root != null and root.scene_file_path == scene_path:
		EditorInterface.set_main_screen_editor("3D")
		_focus_preview()
