class_name MissionEditorWorkspace
extends EditorWorkspace

# Mission workspace: create or open a .bms/.mis mission and author its world. The mission's header
# selects the terrain + environment, which load into the shared terrain viewport, and its placed
# objects are instanced under the terrain world root by the shell-agnostic MissionObjectPlacer.
# New Mission builds an empty mission on the currently-loaded terrain; from there objects, zones,
# waypoints, and scripting are editable (with undo/redo + Save / Save As).
#
# The create + load + resolve + place + edit work lives in MissionController and the placer; this
# adapter is the EditorWorkspace shell binding (capability hooks + inspector).

const TerrainViewportScript = preload("res://modtools/terrain/terrain_viewport.gd")
const MissionControllerScript = preload("res://modtools/mission/mission_controller.gd")
const MissionInspectorScript = preload("res://modtools/mission/mission_inspector.gd")

var terrain_editor: Node
var _controller  # MissionController
var _mount: ViewportMount
# The live inspector + the shell's right dock (%AssetDock). The inspector splits its UI across the
# left pane (browser) and the dock (per-selection editor + Mission form). The shell forwards the
# dock via set_asset_dock BEFORE build_inspector on activation, so we cache it and hand it over when
# the inspector is built (or push it into an already-built inspector on a re-sync / teardown).
var _inspector  # MissionInspector (preloaded, no class_name)
var _detail_mount: Control
# The live "Terrain changed under N objects" confirm (see _prompt_reground), so a
# re-activate while it is open cannot stack a second one.
var _reground_dialog: ConfirmationDialog

# Mission-only terrain/foliage inputs are scoped to activation because the
# Terrain and Mission workspaces share one TerrainEditor scene.
var _mission_preview_context_active := false


func _init(value: Node = null) -> void:
	terrain_editor = value
	_controller = MissionControllerScript.new(value)
	# The controller fires `changed` on load / clear / select / dirty / save; refresh
	# the shell title + action-button state (Save enables, `*` appears) on each.
	_controller.changed.connect(_sync_shell)
	_controller.changed.connect(_sync_mission_preview_context)
	# Transient action feedback (undo / delete / place / rejected edits) flows through
	# status_reported; relay it to the shell status bar. Open / save keep their own poll.
	_controller.status_reported.connect(_on_controller_status)


func _ensure_mount() -> ViewportMount:
	if _mount == null:
		_mount = ViewportMount.new(&"MissionViewport", func() -> Control: return TerrainViewportScript.new())
	return _mount


func set_terrain_editor(value: Node) -> void:
	var previous := terrain_editor
	if _mission_preview_context_active and previous != null and previous != value \
			and previous.has_method("clear_mission_preview_context"):
		previous.clear_mission_preview_context()
	terrain_editor = value
	if _controller != null:
		_controller.set_terrain_editor(value)
	_sync_mission_preview_context()
	if _mount != null:
		var viewport := _mount.get_viewport_node()
		if viewport != null:
			viewport.set_terrain_editor(terrain_editor)


func bind_to_editor(value: Node) -> void:
	# The shell binds the app root; unwrap to the terrain domain editor (the
	# mission edit world lives under it). Bare TerrainEditor binds pass through.
	if value != null and value.has_method("get_terrain_editor"):
		value = value.get_terrain_editor()
	set_terrain_editor(value)


## Install the active mission's external terrain tile array (and preview
## time-of-day) onto the shared terrain preview.
func _sync_mission_preview_context() -> void:
	if not _mission_preview_context_active or terrain_editor == null \
			or not terrain_editor.has_method("set_mission_preview_context"):
		return
	var tile_info: NovaTerrainTileInfo = null
	if _controller != null and _controller.has_method("get_mission_tile_info"):
		tile_info = _controller.get_mission_tile_info()
	var preview_time_of_day := NAN
	if _controller != null and _controller.has_method("get_mission_preview_time_of_day"):
		preview_time_of_day = _controller.get_mission_preview_time_of_day()
	terrain_editor.set_mission_preview_context(tile_info, preview_time_of_day)


func _clear_mission_preview_context() -> void:
	if terrain_editor != null and terrain_editor.has_method("clear_mission_preview_context"):
		terrain_editor.clear_mission_preview_context()


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
	return "Open a mission to load its world."


func shows_camera_status() -> bool:
	return true


# --- Lifecycle / viewport (shared, read-only terrain viewport) ----------------

func activate() -> void:
	_mission_preview_context_active = true
	_sync_mission_preview_context()
	if _controller != null:
		# Drop a loaded mission whose terrain was swapped out underneath from the
		# Terrain workspace before re-showing its objects (else they float over a new
		# world). On the SAME terrain, a non-zero return means height edits left that
		# many objects off the ground — offer the one-step re-ground.
		var drift: int = _controller.reconcile_with_terrain()
		_controller.set_objects_visible(true)
		if drift > 0:
			_prompt_reground(drift)
	if terrain_editor != null and _mount != null and _mount.is_mounted():
		terrain_editor.set_viewport_active(true, false)


# Terrain heights changed under the loaded mission (same .trn): offer the bulk
# re-ground. A transient ConfirmationDialog parented to the shell (the music
# workspace's deactivate prompt pattern), with the shell theme set EXPLICITLY —
# an embedded Window does not resolve the in-tree theme through the Control
# parent chain (see the shell's _ensure_unsaved_dialog comment) — and exclusive
# like the shell's own confirms. Escape/X emit `canceled`, so every dismissal
# lands on the decline path (acknowledge: quiet until the next height edit).
func _prompt_reground(count: int) -> void:
	if editor_shell == null:
		return  # headless mount: the inspector's manual Re-ground button still covers it
	if _reground_dialog != null and is_instance_valid(_reground_dialog):
		return
	var dialog := ConfirmationDialog.new()
	dialog.name = "MissionRegroundDialog"
	dialog.title = "Terrain changed"
	dialog.dialog_text = "Terrain changed under %d object%s.\nRe-ground them to the new surface?" % [count, "" if count == 1 else "s"]
	dialog.exclusive = true
	if editor_shell is Control and (editor_shell as Control).theme != null:
		dialog.theme = (editor_shell as Control).theme
	dialog.get_ok_button().text = "Re-ground"
	dialog.get_cancel_button().text = "Leave as-is"
	dialog.confirmed.connect(func() -> void:
		# The is_loaded guard covers a stale confirm: a dialog that lost modality
		# (another exclusive sibling was up) can be answered after the mission was
		# cleared or swapped underneath it.
		if _controller.is_loaded():
			_controller.reground_drifted()  # reports "Re-grounded N..." via status_reported
		_reground_dialog = null
		dialog.queue_free())
	dialog.canceled.connect(func() -> void:
		_controller.acknowledge_terrain_drift()
		_reground_dialog = null
		dialog.queue_free())
	_reground_dialog = dialog
	_mount_under_shell(dialog)
	dialog.popup_centered()


func deactivate() -> void:
	_mission_preview_context_active = false
	_clear_mission_preview_context()
	# Leaving the workspace dismisses the re-ground question without answering it:
	# the revision is NOT adopted, so the prompt re-poses on the next activate
	# (unlike the explicit "Leave as-is"). Also keeps the dialog from floating over
	# other workspaces or colliding with their own exclusive prompts.
	if _reground_dialog != null and is_instance_valid(_reground_dialog):
		_reground_dialog.queue_free()
	_reground_dialog = null
	if _controller != null:
		# End any half-finished drag and drop the placement tool before leaving, so a
		# stray click after the user returns cannot resume either gesture.
		_controller.cancel_drag()
		_controller.disarm_placement()
		_controller.set_objects_visible(false)
	if terrain_editor != null:
		terrain_editor.set_viewport_active(false, false)


func mount_viewport(mount: Control) -> void:
	if mount == null or terrain_editor == null:
		return
	_sync_mission_preview_context()
	var viewport := _ensure_mount().mount(mount)
	if viewport != null:
		viewport.set_terrain_editor(terrain_editor)
		# Terrain brush stays dormant; the controller handles picking / dragging via the
		# router's separate input_target instead.
		viewport.set_edit_input_enabled(false)
		viewport.set_input_target(_controller)


func unmount_viewport(_released: Control) -> void:
	_detach_input_target()
	if terrain_editor != null:
		terrain_editor.set_viewport_active(false, false)
	if _mount != null:
		_mount.unmount()


func release_viewport() -> void:
	_mission_preview_context_active = false
	_clear_mission_preview_context()
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


# --- New (create a mission from scratch) --------------------------------------
# The shell builds the New button on workspace switch when has_new_action() is true (base returns
# can_new()); visibility must not depend on a loaded mission, so can_new() needs only a bound
# terrain editor. The controller checks for a loaded terrain and reports if there is none.

func can_new() -> bool:
	return _controller != null and terrain_editor != null


func get_new_action_label() -> String:
	return "New Mission"


func new_current() -> Error:
	if _controller == null:
		return ERR_UNAVAILABLE
	var err := int(_controller.new_mission())
	var status: String = _controller.get_last_status()
	if not status.is_empty():
		if not _notify_status(status, &"info" if err == OK else &"error") and err != OK:
			push_warning("New mission: " + status)
	return err as Error


# --- Open ---------------------------------------------------------------------

func can_open() -> bool:
	return terrain_editor != null


func get_open_action_label() -> String:
	return "Open Mission..."


func get_open_dialog_title() -> String:
	return "Open mission (.bms, .mis)"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.bms ; Binary Mission", "*.mis ; Mission Metafile"])


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
	# success message is the one that lands for the user. Without a shell (headless),
	# a failure still goes to the log rather than vanishing.
	var status: String = _controller.get_last_status()
	if not status.is_empty():
		if not _notify_status(status, &"success" if err == OK else &"error") and err != OK:
			push_warning("Mission open: " + status)
	return err as Error


# The domain document the EditorWorkspace base derives undo/redo + dirty from.
func get_editor_document() -> Object:
	return _controller


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


func uses_save_file_dialog() -> bool:
	return true


func get_save_file_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.bms ; Binary Mission", "*.mis ; Mission Metafile"])


func get_save_file_dialog_default_name() -> String:
	if _controller != null:
		var current: String = _controller.get_current_path().get_file()
		if not current.is_empty():
			return current
	return "mission.bms"


func save_as_file(path: String) -> Error:
	if _controller == null:
		return ERR_UNAVAILABLE
	var err := int(_controller.save_as_file(path))
	_report_save_status(err)
	return err as Error


func get_save_dialog_title() -> String:
	return "Choose where to save the mission"


func get_save_dialog_dir() -> String:
	return _controller.get_last_open_dir() if _controller != null else ""


func _report_save_status(err: int) -> void:
	var status: String = _controller.get_last_status() if _controller != null else ""
	if status.is_empty():
		return
	if not _notify_status(status, &"info" if err == OK else &"error") and err != OK:
		# No shell to show in (headless), but a failed save must not be silent.
		push_warning("Mission save: " + status)


# Surface a controller action's transient status in the shell status bar. Errors linger
# a little longer. No-op without a shell (headless tests read get_last_status instead).
func _on_controller_status(message: String, is_error: bool) -> void:
	if message.is_empty():
		return
	_notify_status(message, &"error" if is_error else &"success")


# --- Undo / redo --------------------------------------------------------------
# The base EditorWorkspace exposes these hooks; the controller drives the document's in-memory
# undo history. The live trigger is the viewport Ctrl+Z / Ctrl+Y (handled in MissionController);
# implementing the hooks makes undo/redo reachable for tests and a future shell toolbar with no
# shell change. Mirrors strings_workspace.gd.


# --- Inspector ----------------------------------------------------------------

func build_inspector(mount: Control) -> void:
	_inspector = MissionInspectorScript.new()
	mount.add_child(_inspector)
	# The dock mount was forwarded just before this (set_asset_dock at shell line 585, build_inspector
	# at 592), so it is already cached: the fresh inspector builds its editor + Mission form straight
	# into the dock with no reparent.
	_inspector.setup(_controller, _detail_mount)
	if editor_shell != null and _inspector.has_method("set_reference_services"):
		_inspector.set_reference_services(ResourceRefWidget.services_from_shell(editor_shell))


# --- Asset dock (the right pane) ----------------------------------------------
# Opt into %AssetDock and mount the per-selection editor + the Mission form there, keeping the left
# pane to just the mode tabs + the current mode's list/palette. The inspector owns the dock subtree
# and reparents it back under its own root on teardown (set_asset_dock(null)) before the shell frees
# the dock's children, so the editor widgets are never torn down.

func uses_asset_dock() -> bool:
	return true


func set_asset_dock(dock: Control) -> void:
	_detail_mount = dock
	# is_instance_valid guards the gap between switch-away (old inspector freed) and switch-back
	# (set_asset_dock fires before build_inspector rebuilds it): skip the stale ref, just cache.
	if _inspector != null and is_instance_valid(_inspector):
		_inspector.set_detail_mount(dock)


func sync_asset_dock() -> void:
	if _inspector != null and is_instance_valid(_inspector) and _detail_mount != null:
		_inspector.set_detail_mount(_detail_mount)
