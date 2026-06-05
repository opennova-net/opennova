class_name MissionEditorWorkspace
extends EditorWorkspace

# Mission workspace: open a .bms mission and view its world. The mission's header
# selects the terrain + environment, which load into the shared terrain viewport
# (read-only here), and its placed objects are instanced under the terrain world
# root by the host-agnostic MissionObjectPlacer. Mission authoring (place / move /
# save entities) is deferred, so this is a viewer: Open is the only document
# action and the document never goes dirty.
#
# The load + resolve + place work lives in MissionController and the placer; this
# adapter is the EditorWorkspace shell binding (capability hooks + inspector).

const TerrainViewportScript = preload("res://modtools/terrain/terrain_viewport.gd")
const MissionControllerScript = preload("res://modtools/mission/mission_controller.gd")
const MissionInspectorScript = preload("res://modtools/mission/mission_inspector.gd")

var terrain_editor: Node
var _controller  # MissionController
var _mount: ViewportMount


func _init(value: Node = null) -> void:
	terrain_editor = value
	_controller = MissionControllerScript.new(value)
	# The controller fires `changed` on load / clear / select / dirty / save; refresh
	# the shell title + action-button state (Save enables, `*` appears) on each.
	_controller.changed.connect(_sync_shell_title)


func _ensure_mount() -> ViewportMount:
	if _mount == null:
		_mount = ViewportMount.new(&"MissionViewport", func() -> Control: return TerrainViewportScript.new())
	return _mount


func set_terrain_editor(value: Node) -> void:
	terrain_editor = value
	if _controller != null:
		_controller.set_terrain_editor(value)
	if _mount != null:
		var viewport := _mount.get_viewport_node()
		if viewport != null:
			viewport.set_terrain_editor(terrain_editor)


func bind_to_editor(value: Node) -> void:
	set_terrain_editor(value)


# --- Identity -----------------------------------------------------------------

func get_workspace_id() -> String:
	return "mission"


func get_workspace_label() -> String:
	return "Mission"


func get_workspace_tooltip() -> String:
	return "Open a mission to load its world and view its placed objects."


func get_open_resource_kind() -> String:
	return "mission"


func get_project_title() -> String:
	if _controller == null:
		return "Mission"
	return _controller.get_mission_title()


func get_status_tool() -> String:
	return "Mission"


func get_status_context() -> String:
	if _controller != null and _controller.is_loaded():
		var stats: Dictionary = _controller.get_stats()
		return "%s, %d objects" % [_controller.get_mission_title(), int(stats.get("placed", 0))]
	return "Open a .bms mission to load its world."


func shows_camera_status() -> bool:
	return true


# --- Lifecycle / viewport (shared, read-only terrain viewport) ----------------

func activate() -> void:
	if _controller != null:
		# Drop a loaded mission whose terrain was changed under it from the Terrain
		# workspace before re-showing its objects (else they float over a new world).
		_controller.reconcile_with_terrain()
		_controller.set_objects_visible(true)
	if terrain_editor != null and _mount != null and _mount.is_mounted():
		terrain_editor.set_viewport_active(true, false)


func deactivate() -> void:
	if _controller != null:
		# End any half-finished drag and drop the placement tool before leaving, so a
		# stray click after the user returns cannot resume either gesture.
		_controller.cancel_drag()
		_controller.disarm_placement()
		_controller.set_objects_visible(false)
	if terrain_editor != null:
		terrain_editor.set_viewport_active(false, false)


func mount_viewport(host: Control) -> void:
	if host == null or terrain_editor == null:
		return
	var viewport := _ensure_mount().mount(host)
	if viewport != null:
		viewport.set_terrain_editor(terrain_editor)
		# Terrain brush stays dormant; the controller handles picking / dragging via the
		# router's separate input_target instead.
		viewport.set_edit_input_enabled(false)
		viewport.set_input_target(_controller)


func unmount_viewport(_host: Control) -> void:
	_detach_input_target()
	if terrain_editor != null:
		terrain_editor.set_viewport_active(false, false)
	if _mount != null:
		_mount.unmount()


func release_viewport() -> void:
	_detach_input_target()
	if terrain_editor != null:
		terrain_editor.set_viewport_active(false, false)
	if _mount != null:
		_mount.release()


func _detach_input_target() -> void:
	if _controller != null:
		_controller.cancel_drag()
		_controller.disarm_placement()
	if _mount == null:
		return
	var viewport := _mount.get_viewport_node()
	if viewport != null and viewport.has_method("set_input_target"):
		viewport.set_input_target(null)


func get_viewport_camera() -> Camera3D:
	if terrain_editor != null and terrain_editor.has_method("get_editor_camera"):
		return terrain_editor.get_editor_camera()
	return null


# --- Open (the only document action this phase) -------------------------------

func can_open() -> bool:
	return terrain_editor != null


func get_open_action_label() -> String:
	return "Open Mission..."


func get_open_dialog_title() -> String:
	return "Open mission (.bms)"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.bms ; Mission"])


func get_open_dialog_dir() -> String:
	return _controller.get_last_open_dir() if _controller != null else ""


func get_current_resource_path() -> String:
	return _controller.get_current_path() if _controller != null else ""


func open_file(path: String) -> Error:
	if _controller == null:
		return ERR_UNAVAILABLE
	var err := int(_controller.open_mission(path))
	# Surface the controller's detailed outcome (e.g. "missing dvxi5.trn", or the
	# placement summary). On failure the shell also shows a generic error code; the
	# success message is the one that lands for the user.
	if editor_shell != null and editor_shell.has_method("show_status_message"):
		var status: String = _controller.get_last_status()
		if not status.is_empty():
			editor_shell.show_status_message(status, 5.0 if err == OK else 7.0)
	return err as Error


func has_unsaved_changes() -> bool:
	# Gates the shell's confirm-before-load-over once that guard is generalized; today
	# it tracks edits made by dragging entities (see MissionController).
	return _controller != null and _controller.is_dirty()


# --- Save ---------------------------------------------------------------------
# The base EditorWorkspace exposes these hooks; implementing them lights up the Save /
# Save As buttons and the `*` title marker with no shell changes. save_current() saves
# in place; if there is no path it returns a non-OK and the shell routes to Save As.

# The shell builds action buttons only on workspace switch (before a mission is open),
# using has_save_action()/has_save_as_action() for visibility; it then refreshes only
# the disabled state on each change. So visibility must NOT depend on a loaded mission
# (else the button is never created), while enablement still does via can_save*().

func has_save_action() -> bool:
	return _controller != null


func has_save_as_action() -> bool:
	return _controller != null


func can_save() -> bool:
	return _controller != null and _controller.is_dirty() and not _controller.get_current_path().is_empty()


func get_save_action_label() -> String:
	return "Save Mission"


func can_save_as() -> bool:
	return _controller != null and _controller.is_loaded()


func get_save_as_action_label() -> String:
	return "Save Mission As..."


func save_current() -> Error:
	if _controller == null:
		return ERR_UNAVAILABLE
	var err := int(_controller.save_current())
	_report_save_status(err)
	return err as Error


func save_as(dir_path: String) -> Error:
	if _controller == null:
		return ERR_UNAVAILABLE
	var err := int(_controller.save_as(dir_path))
	_report_save_status(err)
	return err as Error


func get_save_dialog_title() -> String:
	return "Choose where to save the mission (.bms)"


func get_save_dialog_dir() -> String:
	return _controller.get_last_open_dir() if _controller != null else ""


func _report_save_status(err: int) -> void:
	if editor_shell == null or not editor_shell.has_method("show_status_message"):
		return
	var status: String = _controller.get_last_status() if _controller != null else ""
	if not status.is_empty():
		editor_shell.show_status_message(status, 4.0 if err == OK else 6.0)


func _sync_shell_title() -> void:
	if editor_shell != null and editor_shell.has_method("sync_from_editor_state"):
		editor_shell.sync_from_editor_state()


# --- Inspector ----------------------------------------------------------------

func build_inspector(host: Control) -> void:
	var panel := MissionInspectorScript.new()
	host.add_child(panel)
	panel.setup(_controller)
