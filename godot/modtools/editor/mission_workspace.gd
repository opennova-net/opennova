class_name MissionEditorWorkspace
extends EditorWorkspace

const TerrainViewportScript = preload("res://modtools/terrain/terrain_viewport.gd")

var terrain_editor: Node
var _mount: ViewportMount


func _init(value: Node = null) -> void:
	terrain_editor = value


func _ensure_mount() -> ViewportMount:
	if _mount == null:
		_mount = ViewportMount.new(&"MissionViewport", func() -> Control: return TerrainViewportScript.new())
	return _mount


func set_terrain_editor(value: Node) -> void:
	terrain_editor = value
	if _mount != null:
		var viewport := _mount.get_viewport_node()
		if viewport != null:
			viewport.set_terrain_editor(terrain_editor)


func bind_to_editor(value: Node) -> void:
	set_terrain_editor(value)


func get_workspace_tooltip() -> String:
	return "Reserved for mission entities, objectives, and triggers."


func shows_camera_status() -> bool:
	return true


func activate() -> void:
	if terrain_editor != null and _mount != null and _mount.is_mounted():
		terrain_editor.set_viewport_active(true, false)


func deactivate() -> void:
	if terrain_editor != null:
		terrain_editor.set_viewport_active(false, false)


func mount_viewport(host: Control) -> void:
	if host == null or terrain_editor == null:
		return
	var viewport := _ensure_mount().mount(host)
	if viewport != null:
		viewport.set_terrain_editor(terrain_editor)
		viewport.set_edit_input_enabled(false)


func unmount_viewport(_host: Control) -> void:
	if terrain_editor != null:
		terrain_editor.set_viewport_active(false, false)
	if _mount != null:
		_mount.unmount()


func release_viewport() -> void:
	if terrain_editor != null:
		terrain_editor.set_viewport_active(false, false)
	if _mount != null:
		_mount.release()


func get_viewport_camera() -> Camera3D:
	if terrain_editor != null and terrain_editor.has_method("get_editor_camera"):
		return terrain_editor.get_editor_camera()
	return null


func get_workspace_id() -> String:
	return "mission"


func get_open_resource_kind() -> String:
	return "mission"


func get_workspace_label() -> String:
	return "Mission"


func get_project_title() -> String:
	return "Mission"


func get_status_tool() -> String:
	return "Mission"


func get_status_context() -> String:
	if terrain_editor != null and terrain_editor.has_method("get_terrain_name_value"):
		var terrain_name := String(terrain_editor.get_terrain_name_value()).strip_edges()
		if not terrain_name.is_empty():
			return "Viewing terrain: %s" % terrain_name
	return "Mission entity editing is planned; mission formats are not implemented yet."


func build_inspector(host: Control) -> void:
	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 12)
	margin.add_theme_constant_override("margin_top", 12)
	margin.add_theme_constant_override("margin_right", 12)
	margin.add_theme_constant_override("margin_bottom", 12)
	margin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	margin.size_flags_vertical = Control.SIZE_EXPAND_FILL
	host.add_child(margin)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 6)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	margin.add_child(box)

	var title := Label.new()
	title.text = "Coming soon"
	title.theme_type_variation = &"Heading"
	box.add_child(title)

	var body := Label.new()
	body.text = "Mission entity editing is planned; mission formats are not implemented yet."
	body.theme_type_variation = &"Muted"
	body.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(body)
