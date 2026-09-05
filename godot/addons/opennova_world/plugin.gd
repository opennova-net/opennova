@tool
extends EditorPlugin
## Godot owns scene composition; editing sessions own the native documents.
## Active scene nodes display those documents through a transient preview.

const EXAMPLE := "res://examples/world_preview.tscn"
const RETAIL := "res://examples/retail_world.tscn"
const Inspector := preload("res://addons/opennova_world/world_inspector.gd")
const RunSession := preload("res://modtools/game_run_session.gd")
const TOOLBAR := preload("res://addons/opennova_world/toolbar.tscn")

var _inspector: Inspector
var _session: WorldEditSession
var _sessions: Dictionary[String, WorldEditSession] = {}
var _scene_sessions: Dictionary[String, Array] = {}
var _save_failed := false
var _refresh_queued := false
var _copy_dialog: ConfirmationDialog
var _copy_name: LineEdit
var _reload_dialog: ConfirmationDialog
var _run := RunSession.new()
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


func _get_plugin_name() -> String:
	return "OpenNova World"


func _enter_tree() -> void:
	_toolbar = TOOLBAR.instantiate() as HBoxContainer
	_status = _toolbar.get_node("Status") as Label
	(_toolbar.get_node("Source") as Button).pressed.connect(_inspect_source)
	(_toolbar.get_node("Reload") as Button).pressed.connect(_request_reload)
	(_toolbar.get_node("Frame") as Button).pressed.connect(_focus_preview)
	(_toolbar.get_node("Folder") as Button).pressed.connect(_choose_folder)
	(_toolbar.get_node("Details") as Button).pressed.connect(_show_details)
	(_toolbar.get_node("Copy") as Button).pressed.connect(_choose_copy_name)
	(_toolbar.get_node("Save") as Button).pressed.connect(_save_external_data)
	(_toolbar.get_node("Play") as Button).pressed.connect(_play_world)
	(_toolbar.get_node("Stop") as Button).pressed.connect(_run.stop)
	_run.state_changed.connect(_run_state_changed)
	_run.status_changed.connect(_run_status_changed)
	_inspector = Inspector.new()
	_inspector.setup(get_edit_session, edit_native_value)
	_inspector.pending_changed.connect(_refresh_status)
	add_inspector_plugin(_inspector)
	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, _toolbar)
	_details = AcceptDialog.new()
	_details.title = "OpenNova world"
	_toolbar.add_child(_details)
	_folder = FileDialog.new()
	_folder.title = "Local game data folder"
	_folder.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	_folder.access = FileDialog.ACCESS_FILESYSTEM
	_folder.dir_selected.connect(_folder_selected)
	_toolbar.add_child(_folder)
	_copy_dialog = ConfirmationDialog.new()
	_copy_dialog.title = "Create editable world copy"
	_copy_dialog.ok_button_text = "Create Copy"
	_copy_dialog.dialog_hide_on_ok = false
	_copy_name = LineEdit.new()
	_copy_name.max_length = 12
	_copy_dialog.add_child(_copy_name)
	_copy_dialog.confirmed.connect(_create_copy)
	_toolbar.add_child(_copy_dialog)
	_reload_dialog = ConfirmationDialog.new()
	_reload_dialog.title = "Reload native world files"
	_reload_dialog.ok_button_text = "Discard and Reload"
	_reload_dialog.add_button("Save and Reload", true, "save")
	_reload_dialog.confirmed.connect(load_selected_world.bind(true))
	_reload_dialog.custom_action.connect(_save_and_reload)
	_toolbar.add_child(_reload_dialog)
	scene_changed.connect(_scene_changed)
	scene_closed.connect(_scene_closed)
	scene_saved.connect(_scene_saved)
	add_tool_menu_item("OpenNova: Open example world", _open_world.bind(EXAMPLE))
	add_tool_menu_item("OpenNova: Open retail world", _open_world.bind(RETAIL))
	_scene_changed(EditorInterface.get_edited_scene_root())


func _exit_tree() -> void:
	_generation += 1
	_run.shutdown()
	_release_world()
	remove_inspector_plugin(_inspector)
	_sessions.clear()
	_scene_sessions.clear()
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
		_refresh_status()


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
		load_selected_world()
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
	_refresh_status()
	_refresh_inspector()


func _process(_delta: float) -> void:
	_run.poll()
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


## Also the undo target when a copied WorldSource is selected.
func load_selected_world(reopen: bool = false) -> void:
	_release_preview()
	_session = null
	if not is_instance_valid(_world):
		return
	_set_source(_world.world_source)
	if _source == null:
		_status.text = "Assign a WorldSource in the Inspector."
		return
	var key := WorldEditSession.selection_key(_source, _local_directory())
	var session := _sessions.get(key) as WorldEditSession
	var error := OK
	if session == null:
		session = WorldEditSession.new()
		error = session.open(_source, _local_directory())
		if error == OK:
			_sessions[key] = session
			session.changed.connect(_session_changed)
	elif reopen:
		error = session.reload_from_disk()
	if error != OK:
		_status.text = "World load failed. See Details."
		_status.tooltip_text = session.get_last_error()
		_refresh_inspector()
		return
	_session = session
	var scene_path := EditorInterface.get_edited_scene_root().scene_file_path
	if not _scene_sessions.has(scene_path):
		_scene_sessions[scene_path] = []
	if not _scene_sessions[scene_path].has(session):
		_scene_sessions[scene_path].append(session)
	for shader_name in RenderingServer.global_shader_parameter_get_list():
		if String(shader_name).begins_with("opennova_"):
			_globals[shader_name] = RenderingServer.global_shader_parameter_get(shader_name)
	error = _session.load_preview(_world)
	if error == OK:
		_decode = DisplayDecode.new()
		var clear := _world.get_node_or_null("ClearColor") as WorldEnvironment
		_decode_compositor = _decode.create_view_compositor(clear.compositor if clear != null else null)
		_world.add_child(_decode, false, Node.INTERNAL_MODE_BACK)
	_stale = false
	_refresh_status()
	_refresh_inspector()


func _request_reload() -> void:
	if _source == null:
		return
	var selected := _sessions.get(WorldEditSession.selection_key(_source, _local_directory())) as WorldEditSession
	if selected != null and selected.is_dirty():
		_reload_dialog.dialog_text = "Pending changes in %s. Reload reads the files on disk." % ", ".join(selected.get_dirty_files())
		_reload_dialog.popup_centered(Vector2i(560, 160))
	else:
		load_selected_world(true)


func _save_and_reload(action: StringName) -> void:
	if action == &"save" and _save_sessions():
		_reload_dialog.hide()
		load_selected_world(true)


func get_edit_session(object: Object) -> WorldEditSession:
	if _stale or _session == null or not is_instance_valid(_world) or not object is Node:
		return null
	return _session if object == _world or _world.is_ancestor_of(object as Node) else null


func edit_native_value(session: WorldEditSession, field: WorldEditSession.Field,
		value: Variant, slot: int = 0) -> String:
	if session != _session or _stale or not is_instance_valid(_world):
		return "Reload this world's selected source before editing."
	var old_value: Variant = session.get_value(field, slot)
	if old_value == value:
		return ""
	var reason := session.validate_edit(field, value, slot)
	if not reason.is_empty():
		return reason
	var field_name := str(WorldEditSession.Field.keys()[field]).capitalize()
	var action := "%s: %s" % [session.get_file_name(field), field_name]
	if field >= WorldEditSession.Field.FOLIAGE_GRAPHIC:
		action += " (slot %d)" % (slot + 1)
	var undo := get_undo_redo()
	# Native dirtiness is supplied by _get_unsaved_status; the .tscn need not
	# change when only a BMS/TRN/ENV field changes.
	undo.create_action(action, UndoRedo.MERGE_DISABLE, _world, false, false)
	undo.add_do_method(session, "apply_value", field, value, slot)
	undo.add_undo_method(session, "apply_value", field, old_value, slot)
	undo.commit_action()
	return ""


func _session_changed(session: WorldEditSession) -> void:
	_refresh_status()
	if session == _session and not _refresh_queued:
		_refresh_queued = true
		_update_settings.call_deferred()


func _update_settings() -> void:
	_refresh_queued = false
	if _session != null and is_instance_valid(_world) and _world.is_preview_active():
		if _session.update_preview(_world) != OK:
			_status.text = "Settings refresh failed. Reload the preview."
		else:
			_refresh_status()


func _refresh_inspector() -> void:
	var object := EditorInterface.get_inspector().get_edited_object()
	if is_instance_valid(object):
		object.notify_property_list_changed()


func _refresh_status() -> void:
	var active := _session != null and is_instance_valid(_world) and not _stale
	(_toolbar.get_node("Copy") as Button).disabled = not active
	(_toolbar.get_node("Play") as Button).disabled = not active or _run.is_stopping()
	(_toolbar.get_node("Save") as Button).disabled = not _any_dirty()
	(_toolbar.get_node("Stop") as Button).disabled = not _run.is_running() and not _run.is_stopping()
	if active:
		var files := _session.get_dirty_files()
		files.append_array(_inspector.get_pending_files())
		var prefix := "Unsaved | " if not files.is_empty() else ""
		_status.text = "%s%s: %s" % [prefix, _source.mission_name, _world.get_preview_status().capitalize()]
		_status.tooltip_text = "\n".join(files) + "\n" + "\n".join(_world.get_preview_diagnostics())


func _any_dirty() -> bool:
	if _inspector != null and not _inspector.get_pending_files().is_empty():
		return true
	for session: WorldEditSession in _sessions.values():
		if session.is_dirty():
			return true
	return false


func _get_unsaved_status(for_scene: String) -> String:
	var sessions: Array = _sessions.values() if for_scene.is_empty() else _scene_sessions.get(for_scene, [])
	var files := PackedStringArray()
	for session: WorldEditSession in sessions:
		files.append_array(session.get_dirty_files())
	var scene := EditorInterface.get_edited_scene_root()
	if _inspector != null and (for_scene.is_empty() or (scene != null and scene.scene_file_path == for_scene)):
		files.append_array(_inspector.get_pending_files())
	return "Native world files have unsaved changes: " + ", ".join(files) if not files.is_empty() else ""


func _apply_changes() -> void:
	if _inspector != null:
		_inspector.flush_pending_edits()


func _scene_saved(_path: String) -> void:
	_save_sessions()


func _save_external_data() -> void:
	_save_sessions()


func _build() -> bool:
	return _save_sessions()


func _save_sessions() -> bool:
	_save_failed = false
	var errors := _inspector.flush_pending_edits()
	# Invalid widget text blocks Play, but must not prevent other valid native
	# edits from being saved when Godot is closing the scene or editor.
	for session: WorldEditSession in _sessions.values():
		if session.save() != OK:
			var reason := session.get_last_error()
			var recovery := session.write_recovery(ProjectSettings.globalize_path("user://world-recovery"))
			if not recovery.is_empty():
				reason += "\nNative recovery files: " + recovery
			else:
				reason += "\nRecovery could not be written. Keep the editor open and use Create Copy."
			errors.append(reason)
	_save_failed = not errors.is_empty()
	if _save_failed:
		_show_error("\n".join(errors))
		EditorInterface.mark_scene_as_unsaved.call_deferred()
	_refresh_status()
	return not _save_failed


func _scene_closed(path: String) -> void:
	# A successful save cleared dirtiness. Closing without saving is the
	# editor's explicit Discard choice. Failed saves retain their sessions.
	if _save_failed and _any_dirty():
		return
	_scene_sessions.erase(path)
	for key: String in _sessions.keys():
		var referenced := false
		for sessions: Array in _scene_sessions.values():
			referenced = referenced or sessions.has(_sessions[key])
		if not referenced:
			_sessions.erase(key)


func _choose_copy_name() -> void:
	if _session == null:
		return
	_copy_name.text = _source.mission_name.get_basename().left(7) + "_edit"
	_copy_name.tooltip_text = "Creates BMS, TRN, ENV and available mission sidecars in " + _session.get_directory()
	_copy_dialog.popup_centered(Vector2i(480, 110))
	_copy_name.grab_focus()
	_copy_name.select_all()


func _create_copy() -> void:
	if _session == null:
		return
	var source := _session.create_editable_copy(_copy_name.text.strip_edges())
	if source == null:
		_show_error(_session.get_last_error())
		return
	_copy_dialog.hide()
	var undo := get_undo_redo()
	undo.create_action("Select editable world copy", UndoRedo.MERGE_DISABLE, _world)
	undo.add_do_property(_world, "world_source", source)
	undo.add_undo_property(_world, "world_source", _source)
	undo.add_do_method(self, "load_selected_world")
	undo.add_undo_method(self, "load_selected_world")
	undo.commit_action()


func _play_world() -> void:
	if _session == null or _stale or not _save_sessions():
		return
	var conflict := _session.disk_conflict()
	if not conflict.is_empty():
		_show_error(conflict)
		return
	if not _run.run_world(_session.get_directory(), _session.get_source()):
		_show_error(_run.get_last_error())


func _run_state_changed(_state: Dictionary) -> void:
	_refresh_status()


func _run_status_changed(text: String, kind: StringName) -> void:
	(_toolbar.get_node("Play") as Button).tooltip_text = text
	if kind == &"error":
		_show_error(text)


func _show_error(text: String) -> void:
	_details.dialog_text = text
	_details.popup_centered(Vector2i(640, 180))


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
	_apply_changes()
	_release_preview()
	_set_source(null)
	if is_instance_valid(_world) and _world.tree_exiting.is_connected(_release_world):
		_world.tree_exiting.disconnect(_release_world)
	_world = null
	_session = null


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
		load_selected_world()


func _show_details() -> void:
	var lines := PackedStringArray(["Frozen authored world. Gameplay is not running.",
			"Only the primary 3D editor viewport drives this preview."])
	if _stale:
		lines.append("Source changed. Reload to apply the saved selection.")
	if is_instance_valid(_world):
		lines.append_array(_world.get_preview_diagnostics())
	if not _status.tooltip_text.is_empty():
		lines.append(_status.tooltip_text)
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
